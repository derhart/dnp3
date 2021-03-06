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

#include "EnableUnsolicitedTask.h"
#include "MasterTasks.h"

#include "opendnp3/app/APDUBuilders.h"

#include <openpal/executor/IExecutor.h>

using namespace openpal;

namespace opendnp3
{

EnableUnsolicitedTask::EnableUnsolicitedTask(IMasterApplication& app, ClassField enabledClasses_, openpal::TimeDuration retryPeriod_, openpal::Logger logger) :
	IMasterTask(app, 0, logger, nullptr, -1),
	enabledClasses(enabledClasses_),
	retryPeriod(retryPeriod_)
{

}

void EnableUnsolicitedTask::BuildRequest(APDURequest& request, uint8_t seq)
{
	build::EnableUnsolicited(request, enabledClasses.OnlyEventClasses(), seq);
}

bool EnableUnsolicitedTask::IsEnabled() const
{
	return enabledClasses.HasEventClass();
}

IMasterTask::ResponseResult EnableUnsolicitedTask::_OnResponse(const opendnp3::APDUResponseHeader& header, const openpal::ReadBufferView& objects)
{
	return ValidateNullResponse(header, objects) ? ResponseResult::OK_FINAL : ResponseResult::ERROR_BAD_RESPONSE;
}

void EnableUnsolicitedTask::OnResponseOK(openpal::MonotonicTimestamp)
{
	expiration = MonotonicTimestamp::Max();
}

void EnableUnsolicitedTask::OnResponseError(openpal::MonotonicTimestamp now)
{
	disabled = true;
	expiration = MonotonicTimestamp::Max();
}

void EnableUnsolicitedTask::_OnResponseTimeout(openpal::MonotonicTimestamp now)
{
	expiration = now.Add(retryPeriod);
}

void EnableUnsolicitedTask::_OnLowerLayerClose(openpal::MonotonicTimestamp)
{
	expiration = 0;
}


} //end ns

