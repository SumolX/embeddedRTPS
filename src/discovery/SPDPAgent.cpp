/**
 * Copyright © 2019 Lehrstuhl Informatik 11 - RWTH Aachen University
 * 
 * This file is part of embeddedRTPS.
 * 
 * You should have received a copy of the MIT License along with embeddedRTPS.
 * If not, see <https://mit-license.org>.
 */

#include "rtps/discovery/SPDPAgent.h"

#include "lwip/sys.h"
#include "rtps/discovery/ParticipantProxyData.h"
#include "rtps/entities/Participant.h"
#include "rtps/entities/Reader.h"
#include "rtps/entities/Writer.h"
#include "rtps/messages/MessageTypes.h"
#include "rtps/utils/Log.h"
#include "rtps/utils/udpUtils.h"

using rtps::SPDPAgent;
using rtps::SMElement::BuildInEndpointSet;
using rtps::SMElement::ParameterId;

void SPDPAgent::init(Participant &participant, BuiltInEndpoints &endpoints) {
  if (!createMutex(&m_mutex)) {
    SPDP_LOG("Could not alloc mutex");
    return;
  }
  mp_participant = &participant;
  m_buildInEndpoints = endpoints;
  m_buildInEndpoints.spdpReader->registerCallback(receiveCallback, this);

  ucdr_init_buffer(&m_microbuffer, m_outputBuffer.data(),
                   m_outputBuffer.size());
  // addInlineQos();
  addParticipantParameters();
  initialized = true;
}

void SPDPAgent::start() {
  if (m_running) {
    return;
  }
  m_running = true;
  auto t =
      sys_thread_new("SPDPThread", runBroadcast, this,
                     Config::SPDP_WRITER_STACKSIZE, Config::SPDP_WRITER_PRIO);
}

void SPDPAgent::stop() { m_running = false; }

void SPDPAgent::runBroadcast(void *args) {
  SPDPAgent &agent = *static_cast<SPDPAgent *>(args);
  const DataSize_t size = ucdr_buffer_length(&agent.m_microbuffer);
  agent.m_buildInEndpoints.spdpWriter->newChange(
      ChangeKind_t::ALIVE, agent.m_microbuffer.init, size);
  while (agent.m_running) {
#ifdef OS_IS_FREERTOS
    vTaskDelay(pdMS_TO_TICKS(Config::SPDP_RESEND_PERIOD_MS));
#else
    sys_msleep(Config::SPDP_RESEND_PERIOD_MS);
#endif
    agent.m_buildInEndpoints.spdpWriter->setAllChangesToUnsent();
    if (agent.m_cycleHB == Config::SPDP_CYCLECOUNT_HEARTBEAT) {
      agent.m_cycleHB = 0;
      agent.mp_participant->checkAndResetHeartbeats();
    } else {
      agent.m_cycleHB++;
    }
  }
}

void SPDPAgent::receiveCallback(void *callee,
                                const ReaderCacheChange &cacheChange) {
  auto agent = static_cast<SPDPAgent *>(callee);
  agent->handleSPDPPackage(cacheChange);
}

void SPDPAgent::handleSPDPPackage(const ReaderCacheChange &cacheChange) {
  if (!initialized) {
    SPDP_LOG("Callback called without initialization\n");
    return;
  }

  Lock lock{m_mutex};
  if (cacheChange.size > m_inputBuffer.size()) {
    SPDP_LOG("Input buffer to small\n");
    return;
  }

  // Something went wrong deserializing remote participant
  if (!cacheChange.copyInto(m_inputBuffer.data(), m_inputBuffer.size())) {
    return;
  }

  ucdrBuffer buffer;
  ucdr_init_buffer(&buffer, m_inputBuffer.data(), m_inputBuffer.size());

  if (cacheChange.kind == ChangeKind_t::ALIVE) {
    configureEndianessAndOptions(buffer);
    volatile bool success =
        m_proxyDataBuffer.readFromUcdrBuffer(buffer, mp_participant);
    if (success) {
      // TODO In case we store the history we can free the history mutex here
      processProxyData();
    } else {
      SPDP_LOG("ParticipantProxyData deserializtaion failed\n");
    }
  } else {
    // TODO RemoveParticipant
  }
}

void SPDPAgent::configureEndianessAndOptions(ucdrBuffer &buffer) {
  std::array<uint8_t, 2> encapsulation{};
  // Endianess doesn't matter for this since those are single bytes
  ucdr_deserialize_array_uint8_t(&buffer, encapsulation.data(),
                                 encapsulation.size());
  if (encapsulation == SMElement::SCHEME_PL_CDR_LE) {
    buffer.endianness = UCDR_LITTLE_ENDIANNESS;
  } else {
    buffer.endianness = UCDR_BIG_ENDIANNESS;
  }
  // Reuse encapsulation buffer to skip options
  ucdr_deserialize_array_uint8_t(&buffer, encapsulation.data(),
                                 encapsulation.size());
}

void SPDPAgent::processProxyData() {
  if (m_proxyDataBuffer.m_guid.prefix.id == mp_participant->m_guidPrefix.id) {
    return; // Our own packet
  }

  SPDP_LOG("Message from GUID = %u %u %u %u", m_proxyDataBuffer.m_guid.prefix.id[4], m_proxyDataBuffer.m_guid.prefix.id[5], m_proxyDataBuffer.m_guid.prefix.id[6], m_proxyDataBuffer.m_guid.prefix.id[7]);
  const rtps::ParticipantProxyData *remote_part;
  remote_part =
      mp_participant->findRemoteParticipant(m_proxyDataBuffer.m_guid.prefix);
  if (remote_part != nullptr) {
    SPDP_LOG("Not adding this participant");
    mp_participant->refreshRemoteParticipantLiveliness(
        m_proxyDataBuffer.m_guid.prefix);
    return; // Already in our list
  }

  if (mp_participant->addNewRemoteParticipant(m_proxyDataBuffer)) {
    addProxiesForBuiltInEndpoints();
    m_buildInEndpoints.spdpWriter->setAllChangesToUnsent();
#if SPDP_VERBOSE && RTPS_GLOBAL_VERBOSE
    char buffer[64];
    guidPrefix2Str(m_proxyDataBuffer.m_guid.prefix, buffer, sizeof(buffer));
    SPDP_LOG("Added new participant with guid: %s", buffer);
  } else {
    SPDP_LOG("Failed to add new participant");
  }
#else
  } else {
    while (1) {
      SPDP_LOG("failed to add remote participant");
    }
  }
#endif
}

bool SPDPAgent::addProxiesForBuiltInEndpoints() {

  LocatorIPv4 *locator = nullptr;

  // Check if the remote participants has a locator in our subnet
  for (unsigned int i = 0;
       i < m_proxyDataBuffer.m_metatrafficUnicastLocatorList.size(); i++) {
    LocatorIPv4 *l = &(m_proxyDataBuffer.m_metatrafficUnicastLocatorList[i]);
    if (l->isValid() && l->isSameSubnet()) {
      locator = l;
      break;
    }
  }

  if (!locator) {
    return false;
  }

#if SPDP_VERBOSE
  auto ip4addr = locator->getIp4Address();
  const char *addr = ip4addr.toString().c_str();
#endif

  if (m_proxyDataBuffer.hasPublicationReader()) {
    const ReaderProxy proxy{{m_proxyDataBuffer.m_guid.prefix,
                             ENTITYID_SEDP_BUILTIN_PUBLICATIONS_READER},
                            *locator,
                            true};
    m_buildInEndpoints.sedpPubWriter->addNewMatchedReader(proxy);
  }

  if (m_proxyDataBuffer.hasSubscriptionReader()) {
    const ReaderProxy proxy{{m_proxyDataBuffer.m_guid.prefix,
                             ENTITYID_SEDP_BUILTIN_SUBSCRIPTIONS_READER},
                            *locator,
                            true};
    m_buildInEndpoints.sedpSubWriter->addNewMatchedReader(proxy);
  }

  if (m_proxyDataBuffer.hasPublicationWriter()) {
    const WriterProxy proxy{{m_proxyDataBuffer.m_guid.prefix,
                             ENTITYID_SEDP_BUILTIN_PUBLICATIONS_WRITER},
                            *locator,
                            true};
    m_buildInEndpoints.sedpPubReader->addNewMatchedWriter(proxy);
    m_buildInEndpoints.sedpPubReader->sendPreemptiveAckNack(proxy);
  }

  if (m_proxyDataBuffer.hasSubscriptionWriter()) {
    const WriterProxy proxy{{m_proxyDataBuffer.m_guid.prefix,
                             ENTITYID_SEDP_BUILTIN_SUBSCRIPTIONS_WRITER},
                            *locator,
                            true};
    m_buildInEndpoints.sedpSubReader->addNewMatchedWriter(proxy);
    m_buildInEndpoints.sedpPubReader->sendPreemptiveAckNack(proxy);
  }

  return true;
}

void SPDPAgent::addInlineQos() {
  ucdr_serialize_uint16_t(&m_microbuffer, ParameterId::PID_KEY_HASH);
  ucdr_serialize_uint16_t(&m_microbuffer, 16);
  ucdr_serialize_array_uint8_t(&m_microbuffer,
                               mp_participant->m_guidPrefix.id.data(),
                               sizeof(GuidPrefix_t::id));
  ucdr_serialize_array_uint8_t(&m_microbuffer,
                               ENTITYID_BUILD_IN_PARTICIPANT.entityKey.data(),
                               sizeof(EntityId_t::entityKey));
  ucdr_serialize_uint8_t(
      &m_microbuffer,
      static_cast<uint8_t>(ENTITYID_BUILD_IN_PARTICIPANT.entityKind));

  endCurrentList();
}

void SPDPAgent::endCurrentList() {
  ucdr_serialize_uint16_t(&m_microbuffer, ParameterId::PID_SENTINEL);
  ucdr_serialize_uint16_t(&m_microbuffer, 0);
}

void SPDPAgent::addParticipantParameters() {
  const uint16_t zero_options = 0;
  const uint16_t protocolVersionSize =
      sizeof(PROTOCOLVERSION.major) + sizeof(PROTOCOLVERSION.minor);
  const uint16_t vendorIdSize = Config::VENDOR_ID.vendorId.size();
  const uint16_t locatorSize = sizeof(FullLengthLocator);
  const uint16_t durationSize =
      sizeof(Duration_t::seconds) + sizeof(Duration_t::fraction);
  const uint16_t entityKeySize = 3;
  const uint16_t entityKindSize = 1;
  const uint16_t entityIdSize = entityKeySize + entityKindSize;
  const uint16_t guidSize = sizeof(GuidPrefix_t::id) + entityIdSize;

  const FullLengthLocator userUniCastLocator =
      getUserUnicastLocator(mp_participant->m_participantId);
  const FullLengthLocator builtInUniCastLocator =
      getBuiltInUnicastLocator(mp_participant->m_participantId);
  const FullLengthLocator builtInMultiCastLocator =
      getBuiltInMulticastLocator();

  ucdr_serialize_array_uint8_t(&m_microbuffer,
                               rtps::SMElement::SCHEME_PL_CDR_LE.data(),
                               rtps::SMElement::SCHEME_PL_CDR_LE.size());
  ucdr_serialize_uint16_t(&m_microbuffer, zero_options);

  ucdr_serialize_uint16_t(&m_microbuffer, ParameterId::PID_PROTOCOL_VERSION);
  ucdr_serialize_uint16_t(&m_microbuffer, protocolVersionSize + 2);
  ucdr_serialize_uint8_t(&m_microbuffer, PROTOCOLVERSION.major);
  ucdr_serialize_uint8_t(&m_microbuffer, PROTOCOLVERSION.minor);
  ucdr_advance_buffer(&m_microbuffer, 2);

  ucdr_serialize_uint16_t(&m_microbuffer, ParameterId::PID_VENDORID);
  ucdr_serialize_uint16_t(&m_microbuffer, vendorIdSize + 2);
  ucdr_serialize_array_uint8_t(&m_microbuffer,
                               Config::VENDOR_ID.vendorId.data(), vendorIdSize);
  ucdr_advance_buffer(&m_microbuffer, 2);

  ucdr_serialize_uint16_t(&m_microbuffer,
                          ParameterId::PID_DEFAULT_UNICAST_LOCATOR);
  ucdr_serialize_uint16_t(&m_microbuffer, locatorSize);
  ucdr_serialize_array_uint8_t(
      &m_microbuffer, reinterpret_cast<const uint8_t *>(&userUniCastLocator),
      locatorSize);

  ucdr_serialize_uint16_t(&m_microbuffer,
                          ParameterId::PID_METATRAFFIC_UNICAST_LOCATOR);
  ucdr_serialize_uint16_t(&m_microbuffer, locatorSize);
  ucdr_serialize_array_uint8_t(
      &m_microbuffer, reinterpret_cast<const uint8_t *>(&builtInUniCastLocator),
      locatorSize);

  ucdr_serialize_uint16_t(&m_microbuffer,
                          ParameterId::PID_METATRAFFIC_MULTICAST_LOCATOR);
  ucdr_serialize_uint16_t(&m_microbuffer, locatorSize);
  ucdr_serialize_array_uint8_t(
      &m_microbuffer,
      reinterpret_cast<const uint8_t *>(&builtInMultiCastLocator), locatorSize);

  ucdr_serialize_uint16_t(&m_microbuffer,
                          ParameterId::PID_PARTICIPANT_LEASE_DURATION);
  ucdr_serialize_uint16_t(&m_microbuffer, durationSize);
  ucdr_serialize_int32_t(&m_microbuffer,
                         Config::SPDP_DEFAULT_REMOTE_LEASE_DURATION.seconds);
  ucdr_serialize_uint32_t(&m_microbuffer,
                          Config::SPDP_DEFAULT_REMOTE_LEASE_DURATION.fraction);

  ucdr_serialize_uint16_t(&m_microbuffer, ParameterId::PID_PARTICIPANT_GUID);
  ucdr_serialize_uint16_t(&m_microbuffer, guidSize);
  ucdr_serialize_array_uint8_t(&m_microbuffer,
                               mp_participant->m_guidPrefix.id.data(),
                               sizeof(GuidPrefix_t::id));
  ucdr_serialize_array_uint8_t(&m_microbuffer,
                               ENTITYID_BUILD_IN_PARTICIPANT.entityKey.data(),
                               entityKeySize);
  ucdr_serialize_uint8_t(
      &m_microbuffer,
      static_cast<uint8_t>(ENTITYID_BUILD_IN_PARTICIPANT.entityKind));

  ucdr_serialize_uint16_t(&m_microbuffer,
                          ParameterId::PID_BUILTIN_ENDPOINT_SET);
  ucdr_serialize_uint16_t(&m_microbuffer, sizeof(BuildInEndpointSet));
  ucdr_serialize_uint32_t(
      &m_microbuffer, BuildInEndpointSet::DISC_BIE_PARTICIPANT_ANNOUNCER |
                          BuildInEndpointSet::DISC_BIE_PARTICIPANT_DETECTOR |
                          BuildInEndpointSet::DISC_BIE_PUBLICATION_ANNOUNCER |
                          BuildInEndpointSet::DISC_BIE_PUBLICATION_DETECTOR |
                          BuildInEndpointSet::DISC_BIE_SUBSCRIPTION_ANNOUNCER |
                          BuildInEndpointSet::DISC_BIE_SUBSCRIPTION_DETECTOR);

  endCurrentList();
}

#undef SPDP_VERBOSE
