/*
 *
 * Author: Andreas Wüstenberg (andreas.wuestenberg@rwth-aachen.de)
 */
#include "rtps/discovery/BuiltInTopicData.h"
#include "rtps/messages/MessageTypes.h"
#include <cstring>

using rtps::BuiltInTopicData;
using rtps::SMElement::ParameterId;

bool BuiltInTopicData::readFromUcdrBuffer(ucdrBuffer& buffer){

    while(ucdr_buffer_remaining(&buffer) >= 4){
        ParameterId pid;
        uint16_t length;
        ucdr_deserialize_uint16_t(&buffer, reinterpret_cast<uint16_t*>(&pid));
        ucdr_deserialize_uint16_t(&buffer, &length);

        if(ucdr_buffer_remaining(&buffer) < length){
            return false;
        }

        switch(pid){
            case ParameterId::PID_ENDPOINT_GUID:
                ucdr_deserialize_array_uint8_t(&buffer, endpointGuid.prefix.id.data(), endpointGuid.prefix.id.size());
                ucdr_deserialize_array_uint8_t(&buffer, endpointGuid.entityId.entityKey.data(), endpointGuid.entityId.entityKey.size());
                ucdr_deserialize_uint8_t(&buffer, reinterpret_cast<uint8_t*>(&endpointGuid.entityId.entityKind));
                break;
            case ParameterId::PID_RELIABILITY:
                ucdr_deserialize_uint32_t(&buffer, reinterpret_cast<uint32_t*>(&reliabilityKind));
                buffer.iterator+=8;
                //TODO Skip 8 bytes. don't know what they are yet
                break;
            case ParameterId::PID_SENTINEL:
                return true;
            case ParameterId::PID_TOPIC_NAME:
                uint32_t topicNameLength;
                ucdr_deserialize_uint32_t(&buffer, &topicNameLength);
                ucdr_deserialize_array_char(&buffer, topicName, topicNameLength);
                break;
            case ParameterId::PID_TYPE_NAME:
                uint32_t typeNameLength;
                ucdr_deserialize_uint32_t(&buffer, &typeNameLength);
                ucdr_deserialize_array_char(&buffer, typeName, typeNameLength);
                break;
            case ParameterId::PID_UNICAST_LOCATOR:
                unicastLocator.readFromUcdrBuffer(buffer);
                break;
            default:
                buffer.iterator+=length;
                buffer.last_data_size = 1;
        }

        uint32_t alignment = ucdr_buffer_alignment(&buffer, 4);
        buffer.iterator += alignment;
        buffer.last_data_size = 4; // 4 Byte alignment per element
    }
    return ucdr_buffer_remaining(&buffer) == 0;
}

bool BuiltInTopicData::serializeIntoUcdrBuffer(ucdrBuffer& buffer) const{
	// TODO Check if buffer length is sufficient
	const uint16_t guidSize = sizeof(GuidPrefix_t::id) + 4;

	ucdr_serialize_uint16_t(&buffer, ParameterId::PID_UNICAST_LOCATOR);
	ucdr_serialize_uint16_t(&buffer, sizeof(Locator));
	ucdr_serialize_array_uint8_t(&buffer, reinterpret_cast<const uint8_t*>(&unicastLocator), sizeof(Locator));

	// It's a 32 bit instead of 16 because it seems like the field is padded.
	const auto lenTopicName = static_cast<uint32_t>(strlen(topicName) + 1); // + \0
	uint16_t topicAlignment = 0;
	if(lenTopicName % 4 != 0){
		topicAlignment = static_cast<uint8_t>(4 - (lenTopicName % 4));
	}
	const auto totalLengthTopicNameField = static_cast<uint16_t>(sizeof(lenTopicName) + lenTopicName + topicAlignment);
	ucdr_serialize_uint16_t(&buffer, ParameterId::PID_TOPIC_NAME);
	ucdr_serialize_uint16_t(&buffer, totalLengthTopicNameField);
	ucdr_serialize_uint32_t(&buffer, lenTopicName);
	ucdr_serialize_array_char(&buffer, topicName, lenTopicName);
	ucdr_align_to(&buffer,4);

	// It's a 32 bit instead of 16 because it seems like the field is padded.
	const auto lenTypeName = static_cast<uint32_t>(strlen(typeName) + 1); // + \0
	uint16_t typeAlignment = 0;
	if(lenTypeName % 4 != 0){
		typeAlignment = static_cast<uint8_t>(4 - (lenTypeName % 4));
	}
	const auto totalLengthTypeNameField = static_cast<uint16_t>(sizeof(lenTypeName) + lenTypeName + typeAlignment);

	ucdr_serialize_uint16_t(&buffer, ParameterId::PID_TYPE_NAME);
	ucdr_serialize_uint16_t(&buffer, totalLengthTypeNameField);
	ucdr_serialize_uint32_t(&buffer, lenTypeName);
	ucdr_serialize_array_char(&buffer, typeName, lenTypeName);
	ucdr_align_to(&buffer,4);

	ucdr_serialize_uint16_t(&buffer, ParameterId::PID_KEY_HASH);
	ucdr_serialize_uint16_t(&buffer, guidSize);
	ucdr_serialize_array_uint8_t(&buffer, endpointGuid.prefix.id.data(), endpointGuid.prefix.id.size());
	ucdr_serialize_array_uint8_t(&buffer, endpointGuid.entityId.entityKey.data(), endpointGuid.entityId.entityKey.size());
	ucdr_serialize_uint8_t(&buffer, static_cast<uint8_t>(endpointGuid.entityId.entityKind));

	ucdr_serialize_uint16_t(&buffer, ParameterId::PID_ENDPOINT_GUID);
	ucdr_serialize_uint16_t(&buffer, guidSize);
	ucdr_serialize_array_uint8_t(&buffer, endpointGuid.prefix.id.data(), endpointGuid.prefix.id.size());
	ucdr_serialize_array_uint8_t(&buffer, endpointGuid.entityId.entityKey.data(), endpointGuid.entityId.entityKey.size());
	ucdr_serialize_uint8_t(&buffer, static_cast<uint8_t>(endpointGuid.entityId.entityKind));

	const uint8_t unidentifiedOffset = 8;
	ucdr_serialize_uint16_t(&buffer, ParameterId::PID_RELIABILITY);
	ucdr_serialize_uint16_t(&buffer, sizeof(ReliabilityKind_t) + unidentifiedOffset);
	ucdr_serialize_uint32_t(&buffer, static_cast<uint32_t>(reliabilityKind));
	ucdr_serialize_uint32_t(&buffer, 0); // unidentified additional value
	ucdr_serialize_uint32_t(&buffer, 0); // unidentified additional value


	ucdr_serialize_uint16_t(&buffer, ParameterId::PID_SENTINEL);
	ucdr_serialize_uint16_t(&buffer, 0);

	return true;
}
