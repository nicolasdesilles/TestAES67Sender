#include "RavennaNodeManager.h"
#include "ravennakit/core/log.hpp"
#include "ravennakit/core/net/interfaces/network_interface.hpp"
#include "ravennakit/core/net/interfaces/network_interface_config.hpp"
#include "ravennakit/aes67/aes67_packet_time.hpp"
#include "ravennakit/core/util/wrapping_uint.hpp"
#include "ravennakit/ptp/ptp_definitions.hpp"
#include "ravennakit/ptp/types/ptp_clock_identity.hpp"

#include <thread>
#include <cmath>
#include <sstream>
#include <fstream>
#include <chrono>

// #region agent log
#define DEBUG_LOG(loc, msg, data) do { \
    std::ofstream logFile("/Users/nicolasdesilles/Desktop/TestAES67Sender/.cursor/debug.log", std::ios::app); \
    if (logFile.is_open()) { \
        logFile << "{\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"location\":\"" << loc << "\",\"message\":\"" << msg << "\",\"data\":" << data << ",\"timestamp\":" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() << "}\n"; \
        logFile.close(); \
    } \
} while(0)
// #endregion

namespace AudioApp
{

// RavennaNodeManager implementation
RavennaNodeManager::RavennaNodeManager()
    : senderId_()
    , isActive_(false)
    , rtpTimestamp_(0)
{
}

RavennaNodeManager::~RavennaNodeManager()
{
    stop();
}

bool RavennaNodeManager::initialize()
{
    // #region agent log
    DEBUG_LOG("RavennaNodeManager.cpp:initialize:entry", "Initializing RAVENNA node", "{}");
    // #endregion
    
    // Perform system checks
    rav::do_system_checks();
    
    // Set log level from environment (optional, but good practice)
    rav::set_log_level_from_env();
    
    // Create the RAVENNA node
    node_ = std::make_unique<rav::RavennaNode>();
    
    // #region agent log
    DEBUG_LOG("RavennaNodeManager.cpp:initialize:node_created", "RAVENNA node created", "{}");
    // #endregion
    
    // Configure the node
    rav::RavennaNode::Configuration nodeConfig;
    nodeConfig.enable_dnssd_node_discovery = true;
    nodeConfig.enable_dnssd_session_advertisement = true;
    nodeConfig.enable_dnssd_session_discovery = true;
    
    node_->set_configuration(nodeConfig).wait();
    
    // Auto-select network interface
    // Prefer wired interfaces (ethernet) over WiFi for PTP stability
    // This must be done BEFORE configuring PTP, as PTP ports are created based on network interfaces
    auto interfaces = rav::NetworkInterface::get_all();
    
    // #region agent log
    std::stringstream ifaceList;
    ifaceList << "{\"count\":" << (interfaces ? interfaces->size() : 0) << ",\"interfaces\":[";
    if (interfaces) {
        for (size_t i = 0; i < interfaces->size(); ++i) {
            if (i > 0) ifaceList << ",";
            ifaceList << "{\"name\":\"" << interfaces->at(i).get_identifier() << "\",\"type\":" << static_cast<int>(interfaces->at(i).get_type()) << ",\"hasAddresses\":" << (!interfaces->at(i).get_addresses().empty() ? "true" : "false") << "}";
        }
    }
    ifaceList << "]}";
    DEBUG_LOG("RavennaNodeManager.cpp:initialize:interfaces_listed", "Network interfaces found", ifaceList.str());
    // #endregion
    
    if (interfaces && !interfaces->empty())
    {
        // First pass: look for wired (ethernet) interfaces
        bool foundWired = false;
        for (const auto& iface : *interfaces)
        {
            // Prefer wired interfaces (wired_ethernet) over WiFi
            if (!iface.get_addresses().empty() && 
                iface.get_type() != rav::NetworkInterface::Type::loopback &&
                iface.get_type() == rav::NetworkInterface::Type::wired_ethernet)
            {
                currentInterface_ = iface.get_identifier();
                foundWired = true;
                
                // #region agent log
                std::stringstream ifaceData;
                ifaceData << "{\"selected\":\"" << iface.get_identifier() << "\",\"type\":\"ethernet\",\"addresses\":[";
                bool first = true;
                for (const auto& addr : iface.get_addresses()) {
                    if (!first) ifaceData << ",";
                    ifaceData << "\"" << addr.to_string() << "\"";
                    first = false;
                }
                ifaceData << "]}";
                DEBUG_LOG("RavennaNodeManager.cpp:initialize:interface_selected", "Selected wired network interface", ifaceData.str());
                // #endregion
                
                // Create network interface config
                rav::NetworkInterfaceConfig netConfig;
                netConfig.set_interface(rav::rank::primary, iface.get_identifier());
                
                // #region agent log
                // Check IPv4 addresses before setting config
                auto ipv4Addrs = netConfig.get_interface_ipv4_addresses();
                std::stringstream ipv4Data;
                ipv4Data << "{\"count\":" << ipv4Addrs.size() << ",\"addresses\":[";
                bool firstAddr = true;
                for (const auto& addr : ipv4Addrs) {
                    if (!firstAddr) ipv4Data << ",";
                    ipv4Data << "\"" << addr.to_string() << "\"";
                    firstAddr = false;
                }
                ipv4Data << "]}";
                DEBUG_LOG("RavennaNodeManager.cpp:initialize:ipv4_addresses_before", "IPv4 addresses for interface", ipv4Data.str());
                // #endregion
                
                node_->set_network_interface_config(netConfig).wait();
                
                // #region agent log
                // Check IPv4 addresses after setting config (to verify they're still available)
                auto ipv4AddrsAfter = netConfig.get_interface_ipv4_addresses();
                std::stringstream ipv4DataAfter;
                ipv4DataAfter << "{\"count\":" << ipv4AddrsAfter.size() << ",\"addresses\":[";
                firstAddr = true;
                for (const auto& addr : ipv4AddrsAfter) {
                    if (!firstAddr) ipv4DataAfter << ",";
                    ipv4DataAfter << "\"" << addr.to_string() << "\"";
                    firstAddr = false;
                }
                ipv4DataAfter << "]}";
                DEBUG_LOG("RavennaNodeManager.cpp:initialize:interface_set", "Network interface config set", "{\"interface\":\"" + currentInterface_ + "\",\"ipv4Addresses\":" + ipv4DataAfter.str() + "}");
                // #endregion
                
                
                break;
            }
        }
        
        // Second pass: if no wired interface found, fall back to any non-loopback interface
        if (!foundWired)
        {
            for (const auto& iface : *interfaces)
            {
                if (!iface.get_addresses().empty() && iface.get_type() != rav::NetworkInterface::Type::loopback)
                {
                    currentInterface_ = iface.get_identifier();
                    
                    // #region agent log
                    std::stringstream ifaceData;
                    ifaceData << "{\"selected\":\"" << iface.get_identifier() << "\",\"type\":\"fallback\",\"addresses\":[";
                    bool first = true;
                    for (const auto& addr : iface.get_addresses()) {
                        if (!first) ifaceData << ",";
                        ifaceData << "\"" << addr.to_string() << "\"";
                        first = false;
                    }
                    ifaceData << "]}";
                    DEBUG_LOG("RavennaNodeManager.cpp:initialize:interface_selected", "Selected fallback network interface", ifaceData.str());
                    // #endregion
                    
                    // Create network interface config
                    rav::NetworkInterfaceConfig netConfig;
                    netConfig.set_interface(rav::rank::primary, iface.get_identifier());
                    
                    node_->set_network_interface_config(netConfig).wait();
                    
                    // #region agent log
                    DEBUG_LOG("RavennaNodeManager.cpp:initialize:interface_set", "Network interface config set", "{\"interface\":\"" + currentInterface_ + "\"}");
                    // #endregion
                    
                    break;
                }
            }
        }
    }
    
    // Configure PTP domain (default is 0, which is standard for AES67)
    // This must be done AFTER setting network interface so PTP ports exist
    rav::ptp::Instance::Configuration ptpConfig;
    ptpConfig.domain_number = 0; // AES67 standard domain
    
    // #region agent log
    DEBUG_LOG("RavennaNodeManager.cpp:initialize:ptp_config_before", "Setting PTP configuration", "{\"domain\":0}");
    // #endregion
    
    auto ptpResult = node_->set_ptp_instance_configuration(ptpConfig).get();
    if (!ptpResult)
    {
        // Log error but continue
        RAV_LOG_ERROR("Failed to set PTP configuration: {}", ptpResult.error());
        // #region agent log
        DEBUG_LOG("RavennaNodeManager.cpp:initialize:ptp_config_failed", "PTP configuration failed", "{\"error\":\"" + ptpResult.error() + "\"}");
        // #endregion
    }
    else
    {
        // #region agent log
        DEBUG_LOG("RavennaNodeManager.cpp:initialize:ptp_config_success", "PTP configuration set successfully", "{\"domain\":0}");
        // #endregion
    }
    
    // Create PTP subscriber (after network interface and PTP config are set)
    ptpSubscriber_ = std::make_unique<PtpSubscriber>();
    node_->subscribe_to_ptp_instance(ptpSubscriber_.get()).wait();
    
    // #region agent log
    DEBUG_LOG("RavennaNodeManager.cpp:initialize:ptp_subscriber_added", "PTP subscriber added", "{}");
    // #endregion
    
    // Enable NMOS (IS-04 and IS-05 are enabled by default when NMOS is enabled)
    rav::nmos::Node::Configuration nmosConfig;
    nmosConfig.enabled = true;
    nmosConfig.label = "TestAES67Sender";
    nmosConfig.description = "RAVENNA AES67 Sender";
    auto nmosResult = node_->set_nmos_configuration(nmosConfig).get();
    if (!nmosResult)
    {
        // NMOS setup failed, but we can continue without it
        // (it's optional for basic RAVENNA functionality)
    }
    
    // #region agent log
    DEBUG_LOG("RavennaNodeManager.cpp:initialize:exit", "Initialization complete", "{\"interface\":\"" + currentInterface_ + "\"}");
    // #endregion
    
    return true;
}

bool RavennaNodeManager::start(const std::string& sessionName)
{
    if (!node_)
    {
        return false;
    }
    
    if (isActive_)
    {
        return true; // Already started
    }
    
    // Configure the sender
    rav::RavennaSender::Configuration senderConfig;
    senderConfig.session_name = sessionName;
    
    // Set audio format: 48kHz, 24-bit, mono
    senderConfig.audio_format.encoding = kEncoding;
    senderConfig.audio_format.sample_rate = kSampleRate;
    senderConfig.audio_format.num_channels = kNumChannels;
    senderConfig.audio_format.byte_order = rav::AudioFormat::ByteOrder::be; // Big endian for network
    senderConfig.audio_format.ordering = rav::AudioFormat::ChannelOrdering::interleaved;
    
    // Set packet time (1ms is standard for AES67)
    senderConfig.packet_time = rav::aes67::PacketTime::ms_1();
    
    // Set payload type (dynamic range 96-127, 98 is commonly used)
    senderConfig.payload_type = 98;
    
    // Set TTL for multicast
    senderConfig.ttl = 15;
    
    // Set destination (multicast on port 5004, standard for AES67)
    senderConfig.destinations.emplace_back(
        rav::RavennaSender::Destination {
            rav::rank::primary,
            {boost::asio::ip::address_v4::any(), 5004},
            true
        }
    );
    
    senderConfig.enabled = true;
    
    // Create the sender
    auto result = node_->create_sender(senderConfig).get();
    if (!result)
    {
        return false;
    }
    
    senderId_ = *result;
    isActive_ = true;
    rtpTimestamp_ = 0;
    
    return true;
}

void RavennaNodeManager::stop()
{
    if (!node_ || !isActive_)
    {
        return;
    }
    
    // Remove the sender
    if (senderId_.is_valid())
    {
        node_->remove_sender(senderId_).wait();
        senderId_ = rav::Id();
    }
    
    isActive_ = false;
}

bool RavennaNodeManager::isActive() const
{
    return isActive_ && senderId_.is_valid();
}

bool RavennaNodeManager::sendAudio(const float* audioData, int numSamples)
{
    if (!isActive() || !audioData || numSamples <= 0)
    {
        return false;
    }
    
    // Check if PTP is synchronized before sending
    if (!isPtpSynchronized())
    {
        // PTP not synchronized yet, don't send audio
        return false;
    }
    
    // Get PTP clock and convert to RTP timestamp
    const auto& clock = ptpSubscriber_->get_local_clock();
    const auto ptpTimestamp = clock.now().to_rtp_timestamp32(kSampleRate);
    
    // Calculate drift between PTP time and our current RTP timestamp
    // Positive drift means audio device is ahead of PTP clock
    const auto drift = rav::WrappingUint32(ptpTimestamp).diff(rav::WrappingUint32(rtpTimestamp_));
    
    // If drift is too large, resync to PTP time
    // This handles cases where the audio device sample rate doesn't exactly match
    constexpr uint32_t kMaxDrift = 1024; // ~21ms at 48kHz
    if (static_cast<uint32_t>(std::abs(drift)) > kMaxDrift)
    {
        rtpTimestamp_ = ptpTimestamp;
    }
    else
    {
        // Use PTP-synced timestamp, but only if it's ahead of our current timestamp
        // This ensures we don't send packets with timestamps in the past
        if (ptpTimestamp >= rtpTimestamp_)
        {
            rtpTimestamp_ = ptpTimestamp;
        }
    }
    
    // Create audio buffer view from float data
    // Note: ravennakit expects float samples in range [-1.0, 1.0]
    // AudioBufferView expects an array of channel pointers
    const float* channels[] = { audioData };
    rav::AudioBufferView<const float> bufferView(channels, kNumChannels, static_cast<size_t>(numSamples));
    
    // Send audio data with PTP-synced RTP timestamp
    if (!node_->send_audio_data_realtime(senderId_, bufferView, rtpTimestamp_))
    {
        return false;
    }
    
    // Increment RTP timestamp by number of samples sent
    rtpTimestamp_ += static_cast<uint32_t>(numSamples);
    
    return true;
}

std::vector<std::string> RavennaNodeManager::getAvailableInterfaces() const
{
    std::vector<std::string> interfaceNames;
    
    auto interfaces = rav::NetworkInterface::get_all();
    if (interfaces)
    {
        for (const auto& iface : *interfaces)
        {
            // Check if interface is up (has addresses) and not loopback
            if (!iface.get_addresses().empty() && iface.get_type() != rav::NetworkInterface::Type::loopback)
            {
                interfaceNames.push_back(iface.get_identifier());
            }
        }
    }
    
    return interfaceNames;
}

bool RavennaNodeManager::setNetworkInterface(const std::string& interfaceName)
{
    if (!node_)
    {
        return false;
    }
    
    auto interfaces = rav::NetworkInterface::get_all();
    if (!interfaces)
    {
        return false;
    }
    
    // Find the interface by identifier
    for (const auto& iface : *interfaces)
    {
        if (iface.get_identifier() == interfaceName && !iface.get_addresses().empty())
        {
            currentInterface_ = interfaceName;
            
            // Create network interface config
            rav::NetworkInterfaceConfig netConfig;
            netConfig.set_interface(rav::rank::primary, interfaceName);
            
            node_->set_network_interface_config(netConfig).wait();
            return true;
        }
    }
    
    return false;
}

std::string RavennaNodeManager::getCurrentInterface() const
{
    return currentInterface_;
}

bool RavennaNodeManager::isPtpSynchronized()
{
    return ptpSubscriber_ && ptpSubscriber_->isSynchronized();
}

std::string RavennaNodeManager::getPtpDiagnostics() const
{
    if (!ptpSubscriber_)
    {
        return "PTP Subscriber not initialized";
    }
    
    std::string diagnostics;
    
    // Clock status
    const auto& clock = ptpSubscriber_->get_local_clock();
    const bool calibrated = clock.is_calibrated();
    const bool locked = clock.is_locked();
    diagnostics += "Clock Calibrated: " + std::string(calibrated ? "Yes" : "No") + "\n";
    diagnostics += "Clock Locked: " + std::string(locked ? "Yes" : "No") + "\n";
    
    // Port state
    const auto state = ptpSubscriber_->getPortState();
    diagnostics += "Port State: " + std::string(rav::ptp::to_string(state)) + "\n";
    
    // Parent/Grandmaster info
    const bool hasParent = ptpSubscriber_->hasParent();
    if (hasParent)
    {
        const auto& parent = ptpSubscriber_->getParent();
        diagnostics += "Has Grandmaster: Yes\n";
        diagnostics += "Grandmaster ID: " + parent.grandmaster_identity.to_string() + "\n";
        diagnostics += "Parent Port ID: " + parent.parent_port_identity.clock_identity.to_string() + "\n";
        diagnostics += "GM Priority1: " + std::to_string(parent.grandmaster_priority1) + "\n";
        diagnostics += "GM Priority2: " + std::to_string(parent.grandmaster_priority2) + "\n";
    }
    else
    {
        diagnostics += "Has Grandmaster: No (no parent detected)\n";
    }
    
    // #region agent log
    std::stringstream diagData;
    diagData << "{\"calibrated\":" << (calibrated ? "true" : "false") << ",\"locked\":" << (locked ? "true" : "false") << ",\"state\":\"" << rav::ptp::to_string(state) << "\",\"hasParent\":" << (hasParent ? "true" : "false") << "}";
    DEBUG_LOG("RavennaNodeManager.cpp:getPtpDiagnostics", "PTP diagnostics queried", diagData.str());
    // #endregion
    
    return diagnostics;
}

bool RavennaNodeManager::setPtpDomain(uint8_t domainNumber)
{
    if (!node_)
    {
        return false;
    }
    
    rav::ptp::Instance::Configuration ptpConfig;
    ptpConfig.domain_number = domainNumber;
    
    auto result = node_->set_ptp_instance_configuration(ptpConfig).get();
    return result.has_value();
}

} // namespace AudioApp

