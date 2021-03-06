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
#ifndef OPENDNP3_GROUPVARIATIONRECORD_H
#define OPENDNP3_GROUPVARIATIONRECORD_H

#include <cstdint>

#include <openpal/util/Uncopyable.h>

#include "opendnp3/gen/QualifierCode.h"
#include "opendnp3/gen/TimestampMode.h"
#include "opendnp3/gen/GroupVariation.h"


namespace opendnp3
{

// tells if type carries a timestamp
bool TypeHasTimestamp(GroupVariation gv);

enum class GroupVariationType : int
{
    STATIC,
    EVENT,
    OTHER
};

struct EnumAndType
{
	EnumAndType(GroupVariation enumeration_, GroupVariationType type_) :
		enumeration(enumeration_), type(type_)
	{}

	GroupVariation enumeration;
	GroupVariationType type;
};

class GroupVariationRecord
{

public:

	static EnumAndType GetEnumAndType(uint8_t group, uint8_t variation);

	static GroupVariationRecord GetRecord(uint8_t group, uint8_t variation);

	static GroupVariationType GetType(uint8_t group, uint8_t variation);

	GroupVariationRecord(uint8_t group_, uint8_t variation_, GroupVariation enumeration_, GroupVariationType type_);

	GroupVariationRecord() : enumeration(GroupVariation::UNKNOWN), type(GroupVariationType::OTHER), group(0), variation(0)
	{}

	GroupVariation enumeration;
	GroupVariationType type;
	uint8_t group;
	uint8_t variation;
};

class HeaderRecord : public GroupVariationRecord
{
public:

	HeaderRecord() : qualifier(QualifierCode::UNDEFINED)
	{}

	HeaderRecord(const GroupVariationRecord& gv, QualifierCode qualifier_);	

	QualifierCode qualifier;
};

}

#endif
