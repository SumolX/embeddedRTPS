/**
 * Copyright © 2019 Lehrstuhl Informatik 11 - RWTH Aachen University
 * 
 * This file is part of embeddedRTPS.
 * 
 * You should have received a copy of the MIT License along with embeddedRTPS.
 * If not, see <https://mit-license.org>.
 */

#include "rtps/messages/MessageTypes.h"

#include <cstring>
#include <stdio.h>

using namespace rtps;

void doCopyAndMoveOn(uint8_t *dst, const uint8_t *&src, size_t size) {
  memcpy(dst, src, size);
  src += size;
}

bool rtps::deserializeMessage(const MessageProcessingInfo &info,
                              Header &header) {
  if (info.getRemainingSize() < Header::getRawSize()) {
    return false;
  }

  const uint8_t *currentPos = info.getPointerToCurrentPos();
  doCopyAndMoveOn(header.protocolName.data(), currentPos,
                  sizeof(std::array<uint8_t, 4>));
  doCopyAndMoveOn(reinterpret_cast<uint8_t *>(&header.protocolVersion),
                  currentPos, sizeof(ProtocolVersion_t));
  doCopyAndMoveOn(header.vendorId.vendorId.data(), currentPos,
                  header.vendorId.vendorId.size());
  doCopyAndMoveOn(header.guidPrefix.id.data(), currentPos,
                  header.guidPrefix.id.size());
  return true;
}

bool rtps::deserializeMessage(const MessageProcessingInfo &info,
                              SubmessageHeader &header) {
  if (info.getRemainingSize() < SubmessageHeader::getRawSize()) {
    return false;
  }

  const uint8_t *currentPos = info.getPointerToCurrentPos();
  header.submessageId = static_cast<SubmessageKind>(*currentPos++);
  header.flags = *(currentPos++);
  doCopyAndMoveOn(reinterpret_cast<uint8_t *>(&header.octetsToNextHeader),
                  currentPos, sizeof(uint16_t));
  return true;
}

bool rtps::deserializeMessage(const MessageProcessingInfo &info,
                              SubmessageData &msg) {
  if (info.getRemainingSize() < SubmessageHeader::getRawSize()) {
    return false;
  }
  if (!deserializeMessage(info, msg.header)) {
    return false;
  }

  // Check for length including data
  if (info.getRemainingSize() <
      SubmessageHeader::getRawSize() + msg.header.octetsToNextHeader) {
    return false;
  }

  const uint8_t *currentPos =
      info.getPointerToCurrentPos() + SubmessageHeader::getRawSize();

  doCopyAndMoveOn(reinterpret_cast<uint8_t *>(&msg.extraFlags), currentPos,
                  sizeof(uint16_t));
  doCopyAndMoveOn(reinterpret_cast<uint8_t *>(&msg.octetsToInlineQos),
                  currentPos, sizeof(uint16_t));
  doCopyAndMoveOn(msg.readerId.entityKey.data(), currentPos,
                  msg.readerId.entityKey.size());
  msg.readerId.entityKind = static_cast<EntityKind_t>(*currentPos++);
  doCopyAndMoveOn(msg.writerId.entityKey.data(), currentPos,
                  msg.writerId.entityKey.size());
  msg.writerId.entityKind = static_cast<EntityKind_t>(*currentPos++);
  doCopyAndMoveOn(reinterpret_cast<uint8_t *>(&msg.writerSN.high), currentPos,
                  sizeof(msg.writerSN.high));
  doCopyAndMoveOn(reinterpret_cast<uint8_t *>(&msg.writerSN.low), currentPos,
                  sizeof(msg.writerSN.low));
  return true;
}

bool rtps::deserializeMessage(const MessageProcessingInfo &info,
                              SubmessageHeartbeat &msg) {
  if (info.getRemainingSize() < SubmessageHeartbeat::getRawSize()) {
    return false;
  }
  if (!deserializeMessage(info, msg.header)) {
    return false;
  }

  const uint8_t *currentPos =
      info.getPointerToCurrentPos() + SubmessageHeader::getRawSize();

  doCopyAndMoveOn(msg.readerId.entityKey.data(), currentPos,
                  msg.readerId.entityKey.size());
  msg.readerId.entityKind = static_cast<EntityKind_t>(*currentPos++);
  doCopyAndMoveOn(msg.writerId.entityKey.data(), currentPos,
                  msg.writerId.entityKey.size());
  msg.writerId.entityKind = static_cast<EntityKind_t>(*currentPos++);
  doCopyAndMoveOn(reinterpret_cast<uint8_t *>(&msg.firstSN.high), currentPos,
                  sizeof(msg.firstSN.high));
  doCopyAndMoveOn(reinterpret_cast<uint8_t *>(&msg.firstSN.low), currentPos,
                  sizeof(msg.firstSN.low));
  doCopyAndMoveOn(reinterpret_cast<uint8_t *>(&msg.lastSN.high), currentPos,
                  sizeof(msg.lastSN.high));
  doCopyAndMoveOn(reinterpret_cast<uint8_t *>(&msg.lastSN.low), currentPos,
                  sizeof(msg.lastSN.low));
  doCopyAndMoveOn(reinterpret_cast<uint8_t *>(&msg.count.value), currentPos,
                  sizeof(msg.count.value));
  return true;
}

void rtps::deserializeSNS(const uint8_t *&position, SequenceNumberSet &set,
                          size_t num_bitfields) {

  doCopyAndMoveOn(reinterpret_cast<uint8_t *>(&set.base.high), position,
                  sizeof(SequenceNumber_t::high));
  doCopyAndMoveOn(reinterpret_cast<uint8_t *>(&set.base.low), position,
                  sizeof(SequenceNumber_t::low));
  doCopyAndMoveOn(reinterpret_cast<uint8_t *>(&set.numBits), position,
                  sizeof(uint32_t));

  // Ensure that we copy not more bits than our sequence number set can hold
  if (set.numBits != 0) {
    // equal to size = std::min(SNS_NUM_BYTES, num_bitfields)
    size_t size = num_bitfields > SNS_NUM_BYTES ? SNS_NUM_BYTES : num_bitfields;
    doCopyAndMoveOn(reinterpret_cast<uint8_t *>(set.bitMap.data()), position,
                    size);
    position += (num_bitfields - size);
  }
}

bool rtps::deserializeMessage(const MessageProcessingInfo &info,
                              SubmessageAckNack &msg) {
  const DataSize_t remainingSizeAtBeginning = info.getRemainingSize();
  if (remainingSizeAtBeginning <
      SubmessageAckNack::
          getRawSizeWithoutSNSet()) { // Size of SequenceNumberSet unknown
    return false;
  }
  if (!deserializeMessage(info, msg.header)) {
    return false;
  }

  const uint8_t *currentPos =
      info.getPointerToCurrentPos() + SubmessageHeader::getRawSize();

  doCopyAndMoveOn(msg.readerId.entityKey.data(), currentPos,
                  msg.readerId.entityKey.size());
  msg.readerId.entityKind = static_cast<EntityKind_t>(*currentPos++);
  doCopyAndMoveOn(msg.writerId.entityKey.data(), currentPos,
                  msg.writerId.entityKey.size());
  msg.writerId.entityKind = static_cast<EntityKind_t>(*currentPos++);

  size_t num_bitfields = msg.header.octetsToNextHeader - 4 - 4 - 8 - 4 - 4;
  deserializeSNS(currentPos, msg.readerSNState, num_bitfields);

  doCopyAndMoveOn(reinterpret_cast<uint8_t *>(&msg.count.value), currentPos,
                  sizeof(msg.count.value));
  return true;
}

bool rtps::deserializeMessage(const MessageProcessingInfo &info,
                              SubmessageGap &msg) {

  const DataSize_t remainingSizeAtBeginning = info.getRemainingSize();
  if (remainingSizeAtBeginning <
      SubmessageGap::getRawSizeWithoutSNSet()) { // Size of SequenceNumberSet
                                                 // unknown
    return false;
  }
  if (!deserializeMessage(info, msg.header)) {
    return false;
  }

  const uint8_t *currentPos =
      info.getPointerToCurrentPos() + SubmessageHeader::getRawSize();

  doCopyAndMoveOn(msg.readerId.entityKey.data(), currentPos,
                  msg.readerId.entityKey.size());
  msg.readerId.entityKind = static_cast<EntityKind_t>(*currentPos++);
  doCopyAndMoveOn(msg.writerId.entityKey.data(), currentPos,
                  msg.writerId.entityKey.size());
  msg.writerId.entityKind = static_cast<EntityKind_t>(*currentPos++);

  doCopyAndMoveOn(reinterpret_cast<uint8_t *>(&msg.gapStart.high), currentPos,
                  sizeof(msg.gapStart.high));
  doCopyAndMoveOn(reinterpret_cast<uint8_t *>(&msg.gapStart.low), currentPos,
                  sizeof(msg.gapStart.low));

  size_t num_bitfields = msg.header.octetsToNextHeader - 4 - 4 - 8 - 8 - 4;
      remainingSizeAtBeginning - (currentPos - info.getPointerToCurrentPos());
  deserializeSNS(currentPos, msg.gapList, num_bitfields);

  return true;
}
