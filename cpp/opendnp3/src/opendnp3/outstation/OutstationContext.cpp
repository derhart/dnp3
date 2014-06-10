/**
 * Licensed to Green Energy Corp (www.greenenergycorp.com) under one or
 * more contributor license agreements. See the NOTICE file distributed
 * with this work for additional information regarding copyright ownership.
 * Green Energy Corp licenses this file to you under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except in
 * compliance with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This project was forked on 01/01/2013 by Automatak, LLC and modifications
 * may have been made to this file. Automatak, LLC licenses these modifications
 * to you under the terms of the License.
 */

#include "OutstationContext.h"

#include "opendnp3/StaticSizeConfiguration.h"
#include "opendnp3/LogLevels.h"

#include "opendnp3/app/APDUBuilders.h"
#include "opendnp3/app/APDUParser.h"
#include "opendnp3/app/APDUHeaderParser.h"

#include "opendnp3/outstation/ReadHandler.h"
#include "opendnp3/outstation/WriteHandler.h"
#include "opendnp3/outstation/IINHelpers.h"
#include "opendnp3/outstation/CommandActionAdapter.h"
#include "opendnp3/outstation/CommandResponseHandler.h"
#include "opendnp3/outstation/ConstantCommandAction.h"
#include "opendnp3/outstation/EventWriter.h"

#include "opendnp3/outstation/ClassBasedRequestHandler.h"

#include <openpal/LogMacros.h>

using namespace openpal;

namespace opendnp3
{

OutstationContext::OutstationContext(
		const OutstationConfig& config,
		IExecutor& executor,
		LogRoot& root,
		ILowerLayer& lower,
		ICommandHandler& commandHandler,
		ITimeWriteHandler& timeWriteHandler,
		Database& database,
		const EventBufferFacade& buffers) :
	
	params(config.params),
	eventConfig(config.defaultEventResponses),
	logger(root.GetLogger()),	
	pExecutor(&executor),
	pLower(&lower),
	pCommandHandler(&commandHandler),
	pTimeWriteHandler(&timeWriteHandler),
	pDatabase(&database),
	eventBuffer(buffers),
	isOnline(false),
	isTransmitting(false),
	pState(&OutstationStateIdle::Inst()),
	pConfirmTimer(nullptr),
	pUnsolTimer(nullptr),
	unsolPackTimerExpired(false),
	rxFragCount(0),		
	operateExpectedSeq(0),
	operateExpectedFragCount(0),
	solSeqN(0),
	unsolSeqN(0),
	expectedSolConfirmSeq(0),
	expectedUnsolConfirmSeq(0),
	completedNullUnsol(false),	
	rspContext(&database, &eventBuffer, StaticResponseTypes(config.defaultStaticResponses))	
{
	pDatabase->SetEventBuffer(eventBuffer);
	staticIIN.Set(IINBit::DEVICE_RESTART);

	if (params.maxTxFragSize < sizes::MIN_APDU_SIZE)
	{
		FORMAT_LOG_BLOCK(logger, flags::WARN, 
			"setting maxTxFragSize of %u to minimum of %u", 
			static_cast<unsigned int>(params.maxTxFragSize), 
			static_cast<unsigned int>(sizes::MIN_APDU_SIZE));

		params.maxTxFragSize = sizes::MIN_APDU_SIZE;
	}

	if (params.allowUnsolicited)
	{
		// this will cause us to start going through the NULL unsolicited sequence				
		this->OnEnterIdleState();
	}
		
	auto notify = [this]() { this->OnNewEvents(); };
	auto post = [notify, this] { pExecutor->PostLambda(notify); };
	database.SetEventHandler(Bind(post));
}

IINField OutstationContext::GetDynamicIIN()
{
	IINField ret;
	auto count = eventBuffer.UnselectedEvents();
	if (count.numClass1)
	{
		ret.Set(IINBit::CLASS1_EVENTS);
	}
	if (count.numClass2)
	{
		ret.Set(IINBit::CLASS2_EVENTS);
	}
	if (count.numClass3)
	{
		ret.Set(IINBit::CLASS3_EVENTS);
	}
	if (eventBuffer.IsOverflown())
	{
		ret.Set(IINBit::EVENT_BUFFER_OVERFLOW);
	}
	return ret;
}

IINField OutstationContext::GetResponseIIN()
{
	return this->staticIIN | GetDynamicIIN();
}

APDUResponse OutstationContext::StartNewResponse()
{	
	return APDUResponse(txBuffer.GetWriteBuffer(params.maxTxFragSize));
}

void OutstationContext::ConfigureUnsolHeader(APDUResponse& unsol)
{	
	build::NullUnsolicited(unsol, this->unsolSeqN, this->GetResponseIIN());	
}

void OutstationContext::SetOnline()
{
	isOnline = true;
}

void OutstationContext::SetOffline()
{
	isOnline = false;
	unsolPackTimerExpired = false;
	isTransmitting = false;
	pState = &OutstationStateIdle::Inst();
	lastValidRequest.Clear();
	eventBuffer.Reset();
	rspContext.Reset();
	CancelConfirmTimer();
	CancelUnsolTimer();
}

bool OutstationContext::IsOperateSequenceValid()
{	
	return (rxFragCount == operateExpectedFragCount) && (solSeqN == operateExpectedSeq);	
}

bool OutstationContext::IsIdle()
{
	return isOnline && !isTransmitting && pState == &OutstationStateIdle::Inst();
}

bool OutstationContext::CancelConfirmTimer()
{
	return CancelTimer(pConfirmTimer);
}

bool OutstationContext::CancelUnsolTimer()
{
	return CancelTimer(pUnsolTimer);
}

bool OutstationContext::CancelTimer(openpal::ITimer*& pTimer)
{
	if (pTimer)
	{
		pTimer->Cancel();
		pTimer = nullptr;
		return true;
	}
	else
	{
		return false;
	}
}

void OutstationContext::ExamineAPDU(const openpal::ReadOnlyBuffer& fragment)
{
	APDURecord request;
	auto result = APDUHeaderParser::ParseRequest(fragment, request, &logger);
	if (result == APDUHeaderParser::Result::OK)
	{
		// outstations should only process single fragment messages
		if ((request.control.FIR && request.control.FIN) && !request.control.CON)
		{
			if (request.control.UNS)
			{
				if (request.function == FunctionCode::CONFIRM)
				{
					pState->OnUnsolConfirm(this, request);
				}
				else
				{
					SIMPLE_LOG_BLOCK(logger, flags::WARN, "Received non-confirm unsol message");
				}
			}
			else
			{
				if (request.function == FunctionCode::CONFIRM)
				{
					pState->OnSolConfirm(this, request);
				}
				else
				{
					this->OnReceiveSolRequest(request, fragment);
				}
			}
		}
		else
		{
			FORMAT_LOG_BLOCK(logger, flags::WARN,
				"Ignoring fragment with unexpected control field - FIR: %u FIN: %u CON: %u",
				request.control.FIN,
				request.control.FIN,
				request.control.CON);
		}
	}
	else
	{
		SIMPLE_LOG_BLOCK(logger, flags::ERR, "ignoring malformed request header");
	}
}

void OutstationContext::OnReceiveSolRequest(const APDURecord& request, const openpal::ReadOnlyBuffer& fragment)
{
	// analyze this request to see how it compares to the last request
	auto firstRequest = lastValidRequest.IsEmpty();
	auto equality = APDURequest::Compare(fragment, lastValidRequest);
	auto dest = rxBuffer.GetWriteBuffer();
	this->lastValidRequest = fragment.CopyTo(dest);

	if (firstRequest)
	{			
		this->solSeqN = request.control.SEQ;
		this->pState->OnNewRequest(this, request, APDUEquality::NONE);
	}
	else
	{		
		if (this->solSeqN == request.control.SEQ)
		{
			if (equality == APDUEquality::FULL_EQUALITY)
			{
				this->pState->OnRepeatRequest(this, request);
			}
			else // new operation with same SEQ
			{
				this->pState->OnNewRequest(this, request, equality);
			}
		}
		else  // completely new sequence #
		{
			this->pState->OnNewRequest(this, request, equality);
		}
	}

}

void OutstationContext::RespondToRequest(const APDURecord& request, APDUEquality equality)
{
	auto response = StartNewResponse();
	response.SetFunction(FunctionCode::RESPONSE);
	response.SetControl(request.control);
	IINField iin = BuildResponse(request, response, equality);	
	response.SetIIN(iin | this->GetResponseIIN());
	if (response.GetControl().CON)
	{
		expectedSolConfirmSeq = request.control.SEQ;
		pState = &OutstationStateSolConfirmWait::Inst();
	}
	this->BeginResponseTx(response.ToReadOnly());
}

void OutstationContext::BeginResponseTx(const ReadOnlyBuffer& response)
{	
	isTransmitting = true;
	lastResponse = response;
	pLower->BeginTransmit(response);	
}

void OutstationContext::BeginUnsolTx(const ReadOnlyBuffer& response)
{
	isTransmitting = true;
	this->expectedUnsolConfirmSeq = unsolSeqN;
	this->unsolSeqN = AppControlField::NextSeq(unsolSeqN);
	pLower->BeginTransmit(response);
}

IINField OutstationContext::BuildResponse(const APDURecord& request, APDUResponse& response, APDUEquality equality)
{
	switch (request.function)
	{		
		case(FunctionCode::READ) :
			return HandleRead(request, response);		
		case(FunctionCode::WRITE) :
			return HandleWrite(request);	
		case(FunctionCode::SELECT) :
			return HandleSelect(request, response);
		case(FunctionCode::OPERATE) :
			return HandleOperate(request, response, equality);
		case(FunctionCode::DIRECT_OPERATE) :
			return HandleDirectOperate(request, response);
		case(FunctionCode::DELAY_MEASURE) :
			return HandleDelayMeasure(request, response);
		case(FunctionCode::DISABLE_UNSOLICITED) :
			return HandleDisableUnsolicited(request, response);
		case(FunctionCode::ENABLE_UNSOLICITED) :
			return HandleEnableUnsolicited(request, response);
		default:
			return IINField(IINBit::FUNC_NOT_SUPPORTED);
	}
}

void OutstationContext::ContinueMultiFragResponse(uint8_t seq)
{
	auto response = this->StartNewResponse();
	response.SetFunction(FunctionCode::RESPONSE);

	openpal::Transaction tx(this->pDatabase);	
	auto control = this->rspContext.LoadSolicited(response, eventConfig);
	control.SEQ = seq;
	response.SetControl(control);
	response.SetIIN(this->staticIIN | this->GetDynamicIIN());
	if (response.GetControl().CON)
	{
		expectedSolConfirmSeq = seq;
		pState = &OutstationStateSolConfirmWait::Inst();
	}
	this->BeginResponseTx(response.ToReadOnly());
}

void OutstationContext::OnEnterIdleState()
{
	// post these calls so the stack can unwind
	auto lambda = [this]() { this->CheckForIdleState(); };
	pExecutor->PostLambda(lambda);
}

void OutstationContext::CheckForIdleState()
{
	this->CheckForUnsolicited();	
}

void OutstationContext::OnNewEvents()
{
	this->CheckForUnsolicited();	
}

void OutstationContext::CheckForUnsolicited()
{
	if (this->IsIdle() && params.allowUnsolicited && (pUnsolTimer == nullptr))
	{
		if (completedNullUnsol)
		{
			auto criteria = SelectionCriteria::ForomUnsolMask(params.unsolClassMask);
			if (criteria.HasSelection())
			{
				auto unsol = this->StartNewResponse();
				auto initialSize = unsol.Size();
						
				{
					// even though we're not loading static data, we need to lock 
					// the database since it updates the event buffer					
					openpal::Transaction tx(pDatabase);
					auto iterator = eventBuffer.SelectEvents(criteria);
					auto writer = unsol.GetWriter();
					EventWriter::WriteEventHeaders(writer, iterator, eventConfig);
				}
			
				if (unsol.Size() > initialSize) // were any events written?
				{
					this->ConfigureUnsolHeader(unsol);
					pState = &OutstationStateUnsolConfirmWait::Inst();
					this->BeginUnsolTx(unsol.ToReadOnly());
				}
				else
				{ 
					eventBuffer.Reset();
				}
			}
		}
		else
		{
			// send a NULL unsolcited message			
			pState = &OutstationStateUnsolConfirmWait::Inst();
			auto unsol = this->StartNewResponse();
			this->ConfigureUnsolHeader(unsol);
			this->BeginUnsolTx(unsol.ToReadOnly());
		}
	}
}

bool OutstationContext::StartConfirmTimer()
{
	if (pConfirmTimer)
	{	
		return false;		
	}
	else
	{
		auto timeout = [this]() { this->OnSolConfirmTimeout(); };
		pConfirmTimer = pExecutor->Start(params.solConfirmTimeout, Bind(timeout));
		return true;
	}
}

bool OutstationContext::StartUnsolRetryTimer()
{
	if (pUnsolTimer)
	{
		return false;		
	}
	else
	{
		auto timeout = [this]() { this->OnUnsolRetryTimeout(); };
		pUnsolTimer = pExecutor->Start(params.unsolRetryTimeout, Bind(timeout));
		return true;
	}
}

void OutstationContext::OnSolConfirmTimeout()
{
	pState->OnConfirmTimeout(this);
}

void OutstationContext::OnUnsolRetryTimeout()
{
	pUnsolTimer = nullptr;
	this->CheckForUnsolicited();
}

IINField OutstationContext::HandleRead(const APDURecord& request, APDUResponse& response)
{
	rspContext.Reset();
	ReadHandler handler(logger, rspContext);
	auto result = APDUParser::ParseTwoPass(request.objects, &handler, &logger, APDUParser::Context(false)); // don't expect range/count context on a READ
	if (result == APDUParser::Result::OK)
	{
		// Do a transaction on the database (lock) for multi-threaded environments
		// if the request contained static variations, we double buffer (copy) the entire static database.
		// this ensures that multi-fragmented responses see a consistent snapshot of the state
		openpal::Transaction tx(pDatabase);
		pDatabase->DoubleBuffer();
		auto control = rspContext.LoadSolicited(response, eventConfig);
		control.SEQ = request.control.SEQ;
		response.SetControl(control);
		return handler.Errors();
	}
	else
	{
		rspContext.Reset();
		return IINField(IINBit::PARAM_ERROR);
	}
}

IINField OutstationContext::HandleWrite(const APDURecord& request)
{
	WriteHandler handler(logger, pTimeWriteHandler, &staticIIN);
	auto result = APDUParser::ParseTwoPass(request.objects, &handler, &logger);
	if (result == APDUParser::Result::OK)
	{
		return handler.Errors();
	}
	else
	{
		return IINFromParseResult(result);
	}
}

IINField OutstationContext::HandleDirectOperate(const APDURecord& request, APDUResponse& response)
{
	// since we're echoing, make sure there's enough size before beginning
	if (request.objects.Size() > response.Remaining())
	{
		FORMAT_LOG_BLOCK(logger, flags::WARN, "Igonring command request due to payload size of %i", request.objects.Size());
		return IINField(IINBit::PARAM_ERROR);
	}
	else
	{
		CommandActionAdapter adapter(pCommandHandler, false);
		CommandResponseHandler handler(logger, params.maxControlsPerRequest, &adapter, response);
		auto result = APDUParser::ParseTwoPass(request.objects, &handler, &logger);
		return IINFromParseResult(result);
	}
}

IINField OutstationContext::HandleSelect(const APDURecord& request, APDUResponse& response)
{
	// since we're echoing, make sure there's enough size before beginning
	if (request.objects.Size() > response.Remaining())
	{
		FORMAT_LOG_BLOCK(logger, flags::WARN, "Igonring command request due to payload size of %i", request.objects.Size());
		return IINField(IINBit::PARAM_ERROR);
	}
	else
	{
		CommandActionAdapter adapter(pCommandHandler, true);
		CommandResponseHandler handler(logger, params.maxControlsPerRequest, &adapter, response);
		auto result = APDUParser::ParseTwoPass(request.objects, &handler, &logger);
		if (result == APDUParser::Result::OK)
		{
			if (handler.AllCommandsSuccessful())
			{				
				operateExpectedFragCount = rxFragCount + 1;
				operateExpectedSeq = AppControlField::NextSeq(solSeqN);
				selectTime = pExecutor->GetTime();
				return IINField::Empty;
			}
			else
			{
				return IINField::Empty;
			}
		}
		else
		{
			return IINFromParseResult(result);
		}
	}
}

IINField OutstationContext::HandleOperate(const APDURecord& request, APDUResponse& response, APDUEquality equality)
{
	// since we're echoing, make sure there's enough size before beginning
	if (request.objects.Size() > response.Remaining())
	{
		FORMAT_LOG_BLOCK(logger, flags::WARN, "Igonring command request due to payload size of %i", request.objects.Size());
		return IINField(IINBit::PARAM_ERROR);
	}
	else
	{
		if (this->IsOperateSequenceValid())
		{
			auto elapsed = pExecutor->GetTime().milliseconds - selectTime.milliseconds;
			if (elapsed < params.selectTimeout.GetMilliseconds())
			{
				if (equality == APDUEquality::OBJECT_HEADERS_EQUAL)
				{					
					CommandActionAdapter adapter(pCommandHandler, false);
					CommandResponseHandler handler(logger, params.maxControlsPerRequest, &adapter, response);
					auto result = APDUParser::ParseTwoPass(request.objects, &handler, &logger);
					return IINFromParseResult(result);					
				}
				else
				{
					return HandleCommandWithConstant(request, response, CommandStatus::NO_SELECT);
				}
			}
			else
			{
				return HandleCommandWithConstant(request, response, CommandStatus::TIMEOUT);

			}
		}
		else
		{
			return HandleCommandWithConstant(request, response, CommandStatus::NO_SELECT);
		}
	}
}

IINField OutstationContext::HandleDelayMeasure(const APDURecord& request, APDUResponse& response)
{
	if (request.objects.IsEmpty())
	{
		auto writer = response.GetWriter();
		Group52Var2 value = { 0 }; 	// respond with 0 time delay
		writer.WriteSingleValue<UInt8, Group52Var2>(QualifierCode::UINT8_CNT, value);
		return IINField::Empty;
	}
	else
	{
		// there shouldn't be any trailing headers in delay measure request, no need to even parse
		return IINField(IINBit::FUNC_NOT_SUPPORTED);
	}
}

IINField OutstationContext::HandleDisableUnsolicited(const APDURecord& request, APDUResponse& response)
{
	ClassBasedRequestHandler handler(logger);
	auto result = APDUParser::ParseTwoPass(request.objects, &handler, &logger);
	if (result == APDUParser::Result::OK)
	{
		params.unsolClassMask &= ~handler.GetClassMask();
		return handler.Errors();
	}
	else
	{
		return IINFromParseResult(result);
	}
}

IINField OutstationContext::HandleEnableUnsolicited(const APDURecord& request, APDUResponse& response)
{
	ClassBasedRequestHandler handler(logger);
	auto result = APDUParser::ParseTwoPass(request.objects, &handler, &logger);
	if (result == APDUParser::Result::OK)
	{
		params.unsolClassMask |= handler.GetClassMask();
		return handler.Errors();
	}
	else
	{
		return IINFromParseResult(result);
	}
}

IINField OutstationContext::HandleCommandWithConstant(const APDURecord& request, APDUResponse& response, CommandStatus status)
{
	ConstantCommandAction constant(status);
	CommandResponseHandler handler(logger, params.maxControlsPerRequest, &constant, response);
	auto result = APDUParser::ParseTwoPass(request.objects, &handler, &logger);
	return IINFromParseResult(result);
}


}
