
//
// Licensed to Green Energy Corp (www.greenenergycorp.com) under one or
// more contributor license agreements. See the NOTICE file distributed
// with this work for additional information regarding copyright ownership.
// Green Energy Corp licenses this file to you under the Apache License,
// Version 2.0 (the "License"); you may not use this file except in
// compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// This file was forked on 01/01/2013 by Automatak, LLC and modifications
// have been made to this file. Automatak, LLC licenses these modifications to
// you under the GNU Affero General Public License Version 3.0
// (the "Additional License"). You may not use these modifications except in
// compliance with the additional license. You may obtain a copy of the Additional
// License at
//
// http://www.gnu.org/licenses/agpl.html
//
// Contact Automatak, LLC for a commercial license to these modifications
//
#include <boost/test/unit_test.hpp>

#include <APL/Log.h>
#include <APL/IOServiceThreadPool.h>
#include <APL/Exception.h>

using namespace std;
using namespace boost;
using namespace boost::asio;
using namespace boost::system;
using namespace apl;


BOOST_AUTO_TEST_SUITE(ASIOThreadPoolTestSuite)

BOOST_AUTO_TEST_CASE(ThrowsExceptionWithZeroConcurrency)
{
	EventLog log;
	BOOST_CHECK_THROW(IOServiceThreadPool pool(log.GetLogger(LEV_INFO, "pool"), 0), ArgumentException);	
}

BOOST_AUTO_TEST_CASE(CleanConstructionDestruction)
{
	EventLog log;
	IOServiceThreadPool pool(log.GetLogger(LEV_INFO, "pool"), 4);	
}

BOOST_AUTO_TEST_CASE(ThreadPoolShutsdownCleanlyEvenIfALotOfWorkIsSubmitted)
{
        EventLog log;
        IOServiceThreadPool pool(log.GetLogger(LEV_INFO, "pool"), 4);
		for(size_t i=0; i<100000; ++i) pool.GetIOService()->post([](){});        
}


BOOST_AUTO_TEST_CASE(StrandsSequenceCallbacksViaStrandPost)
{
	EventLog log;
	IOServiceThreadPool pool(log.GetLogger(LEV_INFO, "pool"), 8);

	size_t iterations = 100000;

	strand s1(*pool.GetIOService());

	int count1 = 0;

	for(size_t i=0; i<iterations; ++i) s1.post([&count1](){ ++count1; });

	pool.Shutdown();
	BOOST_REQUIRE_EQUAL(iterations, count1);
}

BOOST_AUTO_TEST_CASE(StrandsSequenceCallbacksViaStrandWrap)
{
	EventLog log;
	IOServiceThreadPool pool(log.GetLogger(LEV_INFO, "pool"), 8);
	size_t iterations = 100000;

	io_service* pService = pool.GetIOService();

	strand s1(*pService);

	int count1 = 0;

	for(size_t i=0; i<iterations; ++i) pService->post(s1.wrap([&count1](){ ++count1; }));

	pool.Shutdown();
	BOOST_REQUIRE_EQUAL(iterations, count1);
}

BOOST_AUTO_TEST_SUITE_END()
