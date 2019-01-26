/*
 *
 * Author: Andreas Wüstenberg (andreas.wuestenberg@rwth-aachen.de)
 */

#include "rtps/discovery/SPDPAgent.h"
#include "rtps/messages/MessageTypes.h"
#include "rtps/utils/udpUtils.h"
#include "rtps/entities/Participant.h"
#include "rtps/entities/Writer.h"
#include "rtps/entities/Reader.h"
#include "lwip/sys.h"
#include "rtps/discovery/ParticipantProxyData.h"

using rtps::SPDPAgent;
using rtps::SMElement::ParameterId;
using rtps::SMElement::BuildInEndpointSet;


#define SPDP_VERBOSE 0

#if SPDP_VERBOSE
#include "rtps/utils/printutils.h"
#endif

SPDPAgent::~SPDPAgent(){
    if(initialized){
        sys_mutex_free(&m_mutex);
    }
}

void SPDPAgent::init(Participant& participant, BuiltInEndpoints& endpoints){
    if(sys_mutex_new(&m_mutex) != ERR_OK){
#if SPDP_VERBOSE
        printf("Could not alloc mutex");
#endif
        return;
    }
    mp_participant = &participant;
    m_buildInEndpoints = endpoints;
    m_buildInEndpoints.spdpReader->registerCallback(receiveCallback, this);

    ucdr_init_buffer(&m_microbuffer, m_outputBuffer.data(), m_outputBuffer.size());
    //addInlineQos();
    addParticipantParameters();
    initialized = true;
}

void SPDPAgent::start(){
    if(m_running){
        return;
    }
    m_running = true;
    sys_thread_new("SPDPThread", runBroadcast, this, Config::SPDP_WRITER_STACKSIZE, Config::SPDP_WRITER_PRIO);
}

void SPDPAgent::stop(){
    m_running = false;
}


void SPDPAgent::runBroadcast(void *args){
    SPDPAgent& agent = *static_cast<SPDPAgent*>(args);
    const DataSize_t size = ucdr_buffer_length(&agent.m_microbuffer);
    agent.m_buildInEndpoints.spdpWriter->newChange(ChangeKind_t::ALIVE, agent.m_microbuffer.init, size);
    while(agent.m_running){
        sys_msleep(Config::SPDP_RESEND_PERIOD_MS);
        agent.m_buildInEndpoints.spdpWriter->unsentChangesReset();
    }
}

void SPDPAgent::receiveCallback(void *callee, ReaderCacheChange& cacheChange) {
    auto agent = static_cast<SPDPAgent*>(callee);
    agent->handleSPDPPackage(cacheChange);
}

void SPDPAgent::handleSPDPPackage(ReaderCacheChange& cacheChange){
    if(!initialized){
#if SPDP_VERBOSE
        printf("SPDP: Callback called without initialization\n");
#endif
        return;
    }

    Lock lock{m_mutex};
    if(cacheChange.size > m_inputBuffer.size()){
#if SPDP_VERBOSE
        printf("SPDP: Input buffer to small\n");
#endif
        return;
    }
    cacheChange.copyInto(m_inputBuffer.data(), m_inputBuffer.size());

    ucdrBuffer buffer;
    ucdr_init_buffer(&buffer, m_inputBuffer.data(), cacheChange.size);

    if(cacheChange.kind == ChangeKind_t::ALIVE){
        configureEndianessAndOptions(buffer);
        volatile bool success = m_proxyDataBuffer.readFromUcdrBuffer(buffer);
        if(success){
            // TODO In case we store the history we can free the history mutex here
            processProxyData();
        }
    }else{
        // TODO RemoveParticipant
    }
}

void SPDPAgent::configureEndianessAndOptions(ucdrBuffer& buffer){
    std::array<uint8_t,2> encapsulation{};
    // Endianess doesn't matter for this since those are single bytes
    ucdr_deserialize_array_uint8_t(&buffer, encapsulation.data(), encapsulation.size());
    if(encapsulation == SMElement::SCHEME_PL_CDR_LE) {
        buffer.endianness = UCDR_LITTLE_ENDIANNESS;
    }else{
        buffer.endianness = UCDR_BIG_ENDIANNESS;
    }
    // Reuse encapsulation buffer to skip options
    ucdr_deserialize_array_uint8_t(&buffer, encapsulation.data(), encapsulation.size());
}

void SPDPAgent::processProxyData(){
    if(m_proxyDataBuffer.m_guid.prefix.id == mp_participant->m_guidPrefix.id){
        return; // Our own packet
    }

    if(mp_participant->findRemoteParticipant(m_proxyDataBuffer.m_guid.prefix) != nullptr){
        return; // Already in our list
    }

    // New participant, help him join fast by broadcasting data again
    m_buildInEndpoints.spdpWriter->unsentChangesReset();
    if(mp_participant->addNewRemoteParticipant(m_proxyDataBuffer)) {
        addProxiesForBuiltInEndpoints();

#if SPDP_VERBOSE
        printf("Added new participant with guid: ");
        printGuidPrefix(m_proxyDataBuffer.m_guid.prefix);
        printf("\n");
    }else{
        printf("Failed to add new participant");
#endif
    }
}

void SPDPAgent::addProxiesForBuiltInEndpoints(){
    if (m_proxyDataBuffer.hasPublicationWriter()){
        const WriterProxy proxy{{m_proxyDataBuffer.m_guid.prefix, ENTITYID_SEDP_BUILTIN_PUBLICATIONS_WRITER},
                                m_proxyDataBuffer.m_metatrafficMulticastLocatorList[0]};
        m_buildInEndpoints.sedpPubReader->addNewMatchedWriter(proxy);
    }

    if (m_proxyDataBuffer.hasSubscriptionWriter()){
        const WriterProxy proxy{{m_proxyDataBuffer.m_guid.prefix, ENTITYID_SEDP_BUILTIN_SUBSCRIPTIONS_WRITER},
                                m_proxyDataBuffer.m_metatrafficMulticastLocatorList[0]};
        m_buildInEndpoints.sedpSubReader->addNewMatchedWriter(proxy);
    }

    if(m_proxyDataBuffer.hasPublicationReader()){
        const ReaderProxy proxy{{m_proxyDataBuffer.m_guid.prefix, ENTITYID_SEDP_BUILTIN_PUBLICATIONS_READER},
                                m_proxyDataBuffer.m_metatrafficUnicastLocatorList[0]};
        m_buildInEndpoints.sedpPubWriter->addNewMatchedReader(proxy);
    }

    if(m_proxyDataBuffer.hasSubscriptionReader()){
        const ReaderProxy proxy{{m_proxyDataBuffer.m_guid.prefix, ENTITYID_SEDP_BUILTIN_SUBSCRIPTIONS_READER},
                                m_proxyDataBuffer.m_metatrafficUnicastLocatorList[0]};
        m_buildInEndpoints.sedpSubWriter->addNewMatchedReader(proxy);
    }
}


void SPDPAgent::addInlineQos(){
    ucdr_serialize_uint16_t(&m_microbuffer, ParameterId::PID_KEY_HASH);
    ucdr_serialize_uint16_t(&m_microbuffer, 16);
    ucdr_serialize_array_uint8_t(&m_microbuffer, mp_participant->m_guidPrefix.id.data(), sizeof(GuidPrefix_t::id));
    ucdr_serialize_array_uint8_t(&m_microbuffer, ENTITYID_BUILD_IN_PARTICIPANT.entityKey.data(), sizeof(EntityId_t::entityKey));
    ucdr_serialize_uint8_t(&m_microbuffer,       static_cast<uint8_t>(ENTITYID_BUILD_IN_PARTICIPANT.entityKind));

    endCurrentList();
}

void SPDPAgent::endCurrentList(){
    ucdr_serialize_uint16_t(&m_microbuffer, ParameterId::PID_SENTINEL);
    ucdr_serialize_uint16_t(&m_microbuffer, 0);
}

void SPDPAgent::addParticipantParameters(){
    const uint16_t zero_options = 0;
    const uint16_t protocolVersionSize = sizeof(PROTOCOLVERSION.major) + sizeof(PROTOCOLVERSION.minor);
    const uint16_t vendorIdSize = Config::VENDOR_ID.vendorId.size();
    const uint16_t locatorSize = sizeof(Locator);
    const uint16_t durationSize = sizeof(Duration_t::seconds) + sizeof(Duration_t::fraction);
    const uint16_t entityKeySize = 3;
    const uint16_t entityKindSize = 1;
    const uint16_t entityIdSize = entityKeySize + entityKindSize;
    const uint16_t guidSize = sizeof(GuidPrefix_t::id) + entityIdSize;

    const Locator userUniCastLocator = getUserUnicastLocator(mp_participant->m_participantId);
    const Locator builtInUniCastLocator = getBuiltInUnicastLocator(mp_participant->m_participantId);
    const Locator builtInMultiCastLocator = getBuiltInMulticastLocator();

    ucdr_serialize_array_uint8_t(&m_microbuffer, rtps::SMElement::SCHEME_PL_CDR_LE.data(), rtps::SMElement::SCHEME_PL_CDR_LE.size());
    ucdr_serialize_uint16_t(&m_microbuffer, zero_options);

    ucdr_serialize_uint16_t(&m_microbuffer, ParameterId::PID_PROTOCOL_VERSION);
    ucdr_serialize_uint16_t(&m_microbuffer, protocolVersionSize + 2);
    ucdr_serialize_uint8_t(&m_microbuffer,  PROTOCOLVERSION.major);
    ucdr_serialize_uint8_t(&m_microbuffer,  PROTOCOLVERSION.minor);
    m_microbuffer.iterator += 2;      // padding
    m_microbuffer.last_data_size = 4; // to 4 byte

    ucdr_serialize_uint16_t(&m_microbuffer,      ParameterId::PID_VENDORID);
    ucdr_serialize_uint16_t(&m_microbuffer,      vendorIdSize + 2);
    ucdr_serialize_array_uint8_t(&m_microbuffer, Config::VENDOR_ID.vendorId.data(), vendorIdSize);
    m_microbuffer.iterator += 2;      // padding
    m_microbuffer.last_data_size = 4; // to 4 byte

    ucdr_serialize_uint16_t(&m_microbuffer,      ParameterId::PID_DEFAULT_UNICAST_LOCATOR);
    ucdr_serialize_uint16_t(&m_microbuffer,      locatorSize);
    ucdr_serialize_array_uint8_t(&m_microbuffer, reinterpret_cast<const uint8_t*>(&userUniCastLocator), locatorSize);

    ucdr_serialize_uint16_t(&m_microbuffer,      ParameterId::PID_METATRAFFIC_UNICAST_LOCATOR);
    ucdr_serialize_uint16_t(&m_microbuffer,      locatorSize);
    ucdr_serialize_array_uint8_t(&m_microbuffer, reinterpret_cast<const uint8_t*>(&builtInUniCastLocator), locatorSize);

    ucdr_serialize_uint16_t(&m_microbuffer,      ParameterId::PID_METATRAFFIC_MULTICAST_LOCATOR);
    ucdr_serialize_uint16_t(&m_microbuffer,      locatorSize);
    ucdr_serialize_array_uint8_t(&m_microbuffer, reinterpret_cast<const uint8_t*>(&builtInMultiCastLocator), locatorSize);

    ucdr_serialize_uint16_t(&m_microbuffer,      ParameterId::PID_PARTICIPANT_LEASE_DURATION);
    ucdr_serialize_uint16_t(&m_microbuffer,      durationSize);
    ucdr_serialize_int32_t(&m_microbuffer,       Config::SPDP_LEASE_DURATION.seconds);
    ucdr_serialize_uint32_t(&m_microbuffer,      Config::SPDP_LEASE_DURATION.fraction);

    ucdr_serialize_uint16_t(&m_microbuffer,      ParameterId::PID_PARTICIPANT_GUID);
    ucdr_serialize_uint16_t(&m_microbuffer,      guidSize);
    ucdr_serialize_array_uint8_t(&m_microbuffer, mp_participant->m_guidPrefix.id.data(), sizeof(GuidPrefix_t::id));
    ucdr_serialize_array_uint8_t(&m_microbuffer, ENTITYID_BUILD_IN_PARTICIPANT.entityKey.data(), entityKeySize);
    ucdr_serialize_uint8_t(&m_microbuffer,       static_cast<uint8_t>(ENTITYID_BUILD_IN_PARTICIPANT.entityKind));

    ucdr_serialize_uint16_t(&m_microbuffer,      ParameterId::PID_BUILTIN_ENDPOINT_SET);
    ucdr_serialize_uint16_t(&m_microbuffer,      sizeof(BuildInEndpointSet));
    ucdr_serialize_uint32_t(&m_microbuffer,      BuildInEndpointSet::DISC_BIE_PARTICIPANT_ANNOUNCER |
                                                 BuildInEndpointSet::DISC_BIE_PARTICIPANT_DETECTOR |
                                                 BuildInEndpointSet::DISC_BIE_PUBLICATION_ANNOUNCER |
                                                 BuildInEndpointSet::DISC_BIE_PUBLICATION_DETECTOR |
                                                 BuildInEndpointSet::DISC_BIE_SUBSCRIPTION_ANNOUNCER |
                                                 BuildInEndpointSet::DISC_BIE_SUBSCRIPTION_DETECTOR);

    endCurrentList();
}

#undef SPDP_VERBOSE


