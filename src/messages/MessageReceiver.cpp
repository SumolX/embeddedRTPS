/**
 * Copyright © 2019 Lehrstuhl Informatik 11 - RWTH Aachen University
 * 
 * This file is part of embeddedRTPS.
 * 
 * You should have received a copy of the MIT License along with embeddedRTPS.
 * If not, see <https://mit-license.org>.
 */

#include "rtps/messages/MessageReceiver.h"

#include <rtps/entities/Participant.h>

#include "rtps/entities/Reader.h"
#include "rtps/entities/Writer.h"
#include "rtps/messages/MessageTypes.h"
#include "rtps/utils/Log.h"

using rtps::MessageReceiver;

#if RECV_VERBOSE && RTPS_GLOBAL_VERBOSE
#ifndef RECV_LOG
#include "rtps/utils/strutils.h"
#define RECV_LOG(...)                                                          \
  if (true) {                                                                  \
    printf("[RECV] ");                                                         \
    printf(__VA_ARGS__);                                                       \
    printf("\n");                                                              \
  }
#endif
#else
#define RECV_LOG(...) //
#endif

MessageReceiver::MessageReceiver(Participant *part) : mp_part(part) {}

void MessageReceiver::resetState() {
  sourceGuidPrefix = GUIDPREFIX_UNKNOWN;
  sourceVersion = PROTOCOLVERSION;
  sourceVendor = VENDOR_UNKNOWN;
  haveTimeStamp = false;
}

bool MessageReceiver::processMessage(const uint8_t *data, DataSize_t size) {
  resetState();
  MessageProcessingInfo msgInfo(data, size);

  if (!processHeader(msgInfo)) {
    return false;
  }
  SubmessageHeader submsgHeader;
  while (msgInfo.nextPos < msgInfo.size) {
    if (!deserializeMessage(msgInfo, submsgHeader)) {
      return false;
    }
    processSubmessage(msgInfo, submsgHeader);
  }

  return true;
}

bool MessageReceiver::processHeader(MessageProcessingInfo &msgInfo) {
  Header header;
  if (!deserializeMessage(msgInfo, header)) {
    return false;
  }

  if (header.guidPrefix.id == mp_part->m_guidPrefix.id) {
    RECV_LOG("[MessageReceiver]: Received own message.\n");
    return false; // Don't process our own packet
  }

  if (header.protocolName != RTPS_PROTOCOL_NAME ||
      header.protocolVersion.major != PROTOCOLVERSION.major) {
    return false;
  }

  sourceGuidPrefix = header.guidPrefix;
  sourceVendor = header.vendorId;
  sourceVersion = header.protocolVersion;

  msgInfo.nextPos += Header::getRawSize();
  return true;
}

bool MessageReceiver::processSubmessage(MessageProcessingInfo &msgInfo,
                                        const SubmessageHeader &submsgHeader) {
  bool success = false;

  switch (submsgHeader.submessageId) {
  case SubmessageKind::ACKNACK:
    RECV_LOG("Processing AckNack submessage\n");
    success = processAckNackSubmessage(msgInfo);
    break;
  case SubmessageKind::DATA:
    RECV_LOG("Processing Data submessage\n");
    success = processDataSubmessage(msgInfo, submsgHeader);
    break;
  case SubmessageKind::HEARTBEAT:
    RECV_LOG("Processing Heartbeat submessage\n");
    success = processHeartbeatSubmessage(msgInfo);
    break;
  case SubmessageKind::INFO_DST:
    RECV_LOG("Info_DST submessage not relevant.\n");
    success = true; // Not relevant
    break;
  case SubmessageKind::GAP:
    RECV_LOG("Processing GAP submessage\n");
    success = processGapSubmessage(msgInfo);
    break;
  case SubmessageKind::INFO_TS:
    RECV_LOG("Info_TS submessage not relevant.\n");
    success = true; // Not relevant now
    break;
  default:
    RECV_LOG("Submessage of type %u currently not supported. Skipping..\n",
             static_cast<uint8_t>(submsgHeader.submessageId));
    success = false;
  }
  msgInfo.nextPos +=
      submsgHeader.octetsToNextHeader + SubmessageHeader::getRawSize();
  return success;
}

bool MessageReceiver::processDataSubmessage(
    MessageProcessingInfo &msgInfo, const SubmessageHeader &submsgHeader) {
  SubmessageData dataSubmsg;
  if (!deserializeMessage(msgInfo, dataSubmsg)) {
    return false;
  }

  const uint8_t *serializedData =
      msgInfo.getPointerToCurrentPos() + SubmessageData::getRawSize();

  const DataSize_t size = submsgHeader.octetsToNextHeader -
                          SubmessageData::getRawSize() +
                          SubmessageHeader::getRawSize();

  RECV_LOG("Received data message size %u", (int)size);

  Reader *reader;
  if (dataSubmsg.readerId == ENTITYID_UNKNOWN) {
#if RECV_VERBOSE && RTPS_GLOBAL_VERBOSE
    char buffer[64];
    guid2Str(Guid_t{sourceGuidPrefix, dataSubmsg.writerId}, buffer, sizeof(buffer));
    RECV_LOG("Received ENTITYID_UNKNOWN readerID, searching for writer ID = %s", buffer);
#endif
    reader = mp_part->getReaderByWriterId(
        Guid_t{sourceGuidPrefix, dataSubmsg.writerId});
    if (reader != nullptr)
      RECV_LOG("Found reader!");
  } else {
    reader = mp_part->getReader(dataSubmsg.readerId);
#if RECV_VERBOSE && RTPS_GLOBAL_VERBOSE
    auto reader_by_writer = mp_part->getReaderByWriterId(
        Guid_t{sourceGuidPrefix, dataSubmsg.writerId});

    if (reader_by_writer == nullptr && reader != nullptr) {
      char buffer[64];
      guid2Str(Guid_t{sourceGuidPrefix, dataSubmsg.writerId}, buffer, sizeof(buffer));
      RECV_LOG("FOUND By READER ID, NOT BY WRITER ID = %s", buffer);
    }
#endif
  }
  if (reader != nullptr) {
    Guid_t writerGuid{sourceGuidPrefix, dataSubmsg.writerId};
    ReaderCacheChange change{ChangeKind_t::ALIVE, writerGuid,
                             dataSubmsg.writerSN, serializedData, size};
    reader->newChange(change);
  } else {
#if RECV_VERBOSE && RTPS_GLOBAL_VERBOSE
    char buffer[64];
    entityId2Str(dataSubmsg.readerId, buffer, sizeof(buffer));
    RECV_LOG("Couldn't find a reader with id: %s", buffer);
#endif
  }

  return true;
}

bool MessageReceiver::processHeartbeatSubmessage(
    MessageProcessingInfo &msgInfo) {
  SubmessageHeartbeat submsgHB;
  if (!deserializeMessage(msgInfo, submsgHB)) {
    return false;
  }

  Reader *reader = mp_part->getReader(submsgHB.readerId);
  if (reader != nullptr) {
    reader->onNewHeartbeat(submsgHB, sourceGuidPrefix);
    mp_part->refreshRemoteParticipantLiveliness(sourceGuidPrefix);
    return true;
  } else {
    return false;
  }
}

bool MessageReceiver::processAckNackSubmessage(MessageProcessingInfo &msgInfo) {
  SubmessageAckNack submsgAckNack;
  if (!deserializeMessage(msgInfo, submsgAckNack)) {
    return false;
  }

  Writer *writer = mp_part->getWriter(submsgAckNack.writerId);
  if (writer != nullptr) {
    writer->onNewAckNack(submsgAckNack, sourceGuidPrefix);
    return true;
  } else {
    return false;
  }
}

bool MessageReceiver::processGapSubmessage(MessageProcessingInfo &msgInfo) {
  SubmessageGap submsgGap;
  if (!deserializeMessage(msgInfo, submsgGap)) {
    return false;
  }

  Reader *reader = mp_part->getReader(submsgGap.readerId);
  if (reader != nullptr) {
    reader->onNewGapMessage(submsgGap, sourceGuidPrefix);
    return true;
  } else {
    return false;
  }
}
#undef RECV_VERBOSE
