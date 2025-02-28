/**
 * Copyright © 2019 Lehrstuhl Informatik 11 - RWTH Aachen University
 * 
 * This file is part of embeddedRTPS.
 * 
 * You should have received a copy of the MIT License along with embeddedRTPS.
 * If not, see <https://mit-license.org>.
 */

#pragma once

#include <chrono>
#include <array>

#include "rtps/config.h"
#include "rtps/messages/MessageTypes.h"
#include "ucdr/microcdr.h"
#include "rtps/common/Locator.h"

namespace rtps {

class Participant;
using SMElement::ParameterId;

typedef uint32_t BuiltinEndpointSet_t;

class ParticipantProxyData {
public:
  ParticipantProxyData() { onAliveSignal(); }
  ParticipantProxyData(Guid_t guid);

  ProtocolVersion_t m_protocolVersion = PROTOCOLVERSION;
  Guid_t m_guid = Guid_t{GUIDPREFIX_UNKNOWN, ENTITYID_UNKNOWN};
  VendorId_t m_vendorId = VENDOR_UNKNOWN;
  bool m_expectsInlineQos = false;
  BuiltinEndpointSet_t m_availableBuiltInEndpoints;
  std::array<LocatorIPv4, Config::SPDP_MAX_NUM_LOCATORS>
      m_metatrafficUnicastLocatorList;
  std::array<LocatorIPv4, Config::SPDP_MAX_NUM_LOCATORS>
      m_metatrafficMulticastLocatorList;
  std::array<LocatorIPv4, Config::SPDP_MAX_NUM_LOCATORS>
      m_defaultUnicastLocatorList;
  std::array<LocatorIPv4, Config::SPDP_MAX_NUM_LOCATORS>
      m_defaultMulticastLocatorList;
  Count_t m_manualLivelinessCount{1};
  Duration_t m_leaseDuration = Config::SPDP_DEFAULT_REMOTE_LEASE_DURATION;
#if defined(unix) || defined(__unix__)
  std::chrono::time_point<std::chrono::high_resolution_clock>
      m_lastLivelinessReceivedTimestamp;
#else
  TickType_t m_lastLivelinessReceivedTickCount = 0;
#endif
  void reset();

  bool readFromUcdrBuffer(ucdrBuffer &buffer, Participant *participant);

  inline bool hasParticipantWriter();
  inline bool hasParticipantReader();
  inline bool hasPublicationWriter();
  inline bool hasPublicationReader();
  inline bool hasSubscriptionWriter();
  inline bool hasSubscriptionReader();

  inline void onAliveSignal();
  inline bool isAlive();
  inline uint32_t getAliveSignalAgeInMilliseconds();

private:
  bool readLocatorIntoList(
      ucdrBuffer &buffer,
      std::array<LocatorIPv4, Config::SPDP_MAX_NUM_LOCATORS> &list);

  static const BuiltinEndpointSet_t
      DISC_BUILTIN_ENDPOINT_PARTICIPANT_ANNOUNCER = 1 << 0;
  static const BuiltinEndpointSet_t DISC_BUILTIN_ENDPOINT_PARTICIPANT_DETECTOR =
      1 << 1;
  static const BuiltinEndpointSet_t
      DISC_BUILTIN_ENDPOINT_PUBLICATION_ANNOUNCER = 1 << 2;
  static const BuiltinEndpointSet_t DISC_BUILTIN_ENDPOINT_PUBLICATION_DETECTOR =
      1 << 3;
  static const BuiltinEndpointSet_t
      DISC_BUILTIN_ENDPOINT_SUBSCRIPTION_ANNOUNCER = 1 << 4;
  static const BuiltinEndpointSet_t
      DISC_BUILTIN_ENDPOINT_SUBSCRIPTION_DETECTOR = 1 << 5;
  static const BuiltinEndpointSet_t
      DISC_BUILTIN_ENDPOINT_PARTICIPANT_PROXY_ANNOUNCER = 1 << 6;
  static const BuiltinEndpointSet_t
      DISC_BUILTIN_ENDPOINT_PARTICIPANT_PROXY_DETECTOR = 1 << 7;
  static const BuiltinEndpointSet_t
      DISC_BUILTIN_ENDPOINT_PARTICIPANT_STATE_ANNOUNCER = 1 << 8;
  static const BuiltinEndpointSet_t
      DISC_BUILTIN_ENDPOINT_PARTICIPANT_STATE_DETECTOR = 1 << 9;
  static const BuiltinEndpointSet_t
      BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_DATA_WRITER = 1 << 10;
  static const BuiltinEndpointSet_t
      BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_DATA_READER = 1 << 11;
};

// Needs to be in header because they are marked with inline
bool ParticipantProxyData::hasParticipantWriter() {
  return (m_availableBuiltInEndpoints &
          DISC_BUILTIN_ENDPOINT_PARTICIPANT_ANNOUNCER) == 1;
}

bool ParticipantProxyData::hasParticipantReader() {
  return (m_availableBuiltInEndpoints &
          DISC_BUILTIN_ENDPOINT_PARTICIPANT_DETECTOR) != 0;
}

bool ParticipantProxyData::hasPublicationWriter() {
  return (m_availableBuiltInEndpoints &
          DISC_BUILTIN_ENDPOINT_PUBLICATION_ANNOUNCER) != 0;
}

bool ParticipantProxyData::hasPublicationReader() {
  return (m_availableBuiltInEndpoints &
          DISC_BUILTIN_ENDPOINT_PUBLICATION_DETECTOR) != 0;
}

bool ParticipantProxyData::hasSubscriptionWriter() {
  return (m_availableBuiltInEndpoints &
          DISC_BUILTIN_ENDPOINT_SUBSCRIPTION_ANNOUNCER) != 0;
}

bool ParticipantProxyData::hasSubscriptionReader() {
  return (m_availableBuiltInEndpoints &
          DISC_BUILTIN_ENDPOINT_SUBSCRIPTION_DETECTOR) != 0;
}

void ParticipantProxyData::onAliveSignal() {
#if defined(unix) || defined(__unix__)
  m_lastLivelinessReceivedTimestamp = std::chrono::high_resolution_clock::now();
#else
  m_lastLivelinessReceivedTickCount = xTaskGetTickCount();
#endif
}

uint32_t ParticipantProxyData::getAliveSignalAgeInMilliseconds() {
#if defined(unix) || defined(__unix__)
  auto now = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> duration =
      now - m_lastLivelinessReceivedTimestamp;
  return duration.count();
#else
  return (xTaskGetTickCount() - m_lastLivelinessReceivedTickCount) *
         (1000 / configTICK_RATE_HZ);
#endif
}

/*
 *  Returns true if last heartbeat within lease duration, else false
 */
bool ParticipantProxyData::isAlive() {
  uint32_t lease_in_ms =
      m_leaseDuration.seconds * 1000 + m_leaseDuration.fraction * 1e-6;

  uint32_t max_lease_in_ms =
      Config::SPDP_MAX_REMOTE_LEASE_DURATION.seconds * 1000 +
      Config::SPDP_MAX_REMOTE_LEASE_DURATION.fraction * 1e-6;

  auto heatbeat_age_in_ms = getAliveSignalAgeInMilliseconds();

  if (heatbeat_age_in_ms > std::min(lease_in_ms, max_lease_in_ms)) {
    return false;
  }
  return true;
}

}
