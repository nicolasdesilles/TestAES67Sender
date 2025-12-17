#include "RavennaNodeManager.h"
#include "ravennakit/core/log.hpp"
#include "ravennakit/core/net/interfaces/network_interface.hpp"
#include "ravennakit/core/net/interfaces/network_interface_config.hpp"
#include "ravennakit/aes67/aes67_packet_time.hpp"
#include "ravennakit/core/util/wrapping_uint.hpp"
#include "ravennakit/ptp/ptp_definitions.hpp"
#include "ravennakit/ptp/types/ptp_clock_identity.hpp"

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <thread>
#include <cmath>
#include <sstream>
#include <fstream>
#include <chrono>
#include <cstdlib>

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

void RavennaNodeManager::NodeSubscriber::nmos_node_config_updated(const rav::nmos::Node::Configuration& config)
{
    owner_.setNmosConfigSnapshot(config);
}

void RavennaNodeManager::NodeSubscriber::nmos_node_status_changed(const rav::nmos::Node::Status status, const rav::nmos::Node::StatusInfo& registry_info)
{
    owner_.setNmosStatusSnapshot(status, registry_info);
}

const char* RavennaNodeManager::nmosStatusToString(rav::nmos::Node::Status status)
{
    switch (status)
    {
        case rav::nmos::Node::Status::disabled: return "disabled";
        case rav::nmos::Node::Status::discovering: return "discovering";
        case rav::nmos::Node::Status::connecting: return "connecting";
        case rav::nmos::Node::Status::connected: return "connected";
        case rav::nmos::Node::Status::registered: return "registered";
        case rav::nmos::Node::Status::p2p: return "p2p";
        case rav::nmos::Node::Status::error: return "error";
    }
    return "unknown";
}

void RavennaNodeManager::setNmosConfigSnapshot(const rav::nmos::Node::Configuration& config)
{
    std::lock_guard<std::mutex> lock(nmosMutex_);
    nmosConfigSnapshot_ = config;
}

void RavennaNodeManager::setNmosStatusSnapshot(rav::nmos::Node::Status status, const rav::nmos::Node::StatusInfo& info)
{
    std::lock_guard<std::mutex> lock(nmosMutex_);
    nmosStatus_ = status;
    nmosRegistryInfo_ = info;
}

std::string RavennaNodeManager::getNmosStatusText() const
{
    std::lock_guard<std::mutex> lock(nmosMutex_);

    std::string s = std::string(nmosStatusToString(nmosStatus_));
    if (!nmosRegistryInfo_.address.empty())
    {
        // registry_info.address may already contain scheme/host/port (URL). Avoid appending another port.
        s += " (" + nmosRegistryInfo_.address + ")";
    }
    return s;
}

std::string RavennaNodeManager::getNmosDiagnostics() const
{
    std::lock_guard<std::mutex> lock(nmosMutex_);

    std::string out;
    out += "Status: " + std::string(nmosStatusToString(nmosStatus_)) + "\n";
    out += "Mode: " + std::string(rav::nmos::to_string(nmosConfigSnapshot_.operation_mode)) + "\n";
    out += "Node API port: " + std::to_string(nmosConfigSnapshot_.api_port) + "\n";

    out += "Configured registry address: ";
    out += configuredRegistryAddress_.empty() ? "(mDNS auto)" : configuredRegistryAddress_;
    out += "\n";

    out += "Registry: ";
    if (!nmosRegistryInfo_.address.empty())
    {
        // Show address and api_port separately to avoid "http://host:port:port" formatting.
        out += nmosRegistryInfo_.address;
        if (!nmosRegistryInfo_.name.empty())
            out += " (" + nmosRegistryInfo_.name + ")";
        out += "\n";
        out += "Registry API port: " + std::to_string(nmosRegistryInfo_.api_port);
    }
    else
    {
        out += "(none)";
    }
    out += "\n";

    out += "NMOS Node ID: " + (nmosNodeId_.is_nil() ? std::string("(unknown)") : boost::uuids::to_string(nmosNodeId_)) + "\n";
    out += "NMOS Device ID: " + (nmosDeviceId_.is_nil() ? std::string("(unknown)") : boost::uuids::to_string(nmosDeviceId_)) + "\n";
    return out;
}

std::string RavennaNodeManager::getSenderDiagnostics() const
{
    std::stringstream ss;
    ss << "Interface: " << (currentInterface_.empty() ? "(auto)" : currentInterface_) << "\n";
    ss << "Active: " << (isActive_.load() ? "Yes" : "No") << "\n";
    ss << "Senders: " << activeSenders_.size() << "\n";
    ss << "NMOS port: " << nmosPort_ << "\n";

    for (size_t i = 0; i < activeSenders_.size(); ++i)
    {
        const auto& s = activeSenders_[i];
        ss << "\nSender[" << i << "]\n";
        ss << "  ID: " << s.id.to_string() << "\n";
        ss << "  Session: " << s.config.session_name << "\n";
        ss << "  Enabled: " << (s.enabled ? "Yes" : "No") << "\n";
        ss << "  Format: " << (s.config.audio_format.sample_rate) << " Hz, "
           << static_cast<int>(s.config.audio_format.num_channels) << " ch, "
           << rav::to_string(s.config.audio_format.encoding) << "\n";
        ss << "  Packet time: "
           << static_cast<int>(s.config.packet_time.fraction.numerator) << "/"
           << static_cast<int>(s.config.packet_time.fraction.denominator)
           << " (signaled_ptime_ms=" << s.config.packet_time.signaled_ptime(s.config.audio_format.sample_rate) << ")\n";
        ss << "  Payload type: " << static_cast<int>(s.config.payload_type) << "\n";
        ss << "  TTL: " << static_cast<int>(s.config.ttl) << "\n";
        if (!s.config.destinations.empty())
        {
            const auto& d = s.config.destinations[0];
            ss << "  Dest: " << d.endpoint.address().to_string() << ":" << d.endpoint.port()
               << " (enabled=" << (d.enabled ? "true" : "false") << ")\n";
        }
    }
    return ss.str();
}

// RavennaNodeManager implementation
RavennaNodeManager::RavennaNodeManager()
    : isActive_(false)
    , rtpTimestamp_(0)
{
}

RavennaNodeManager::~RavennaNodeManager()
{
    shutdown();
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

    // Subscribe to node callbacks (NMOS status/config updates, etc.)
    nodeSubscriber_ = std::make_unique<NodeSubscriber>(*this);
    node_->subscribe(nodeSubscriber_.get()).wait();
    
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
    
    // Create PTP subscriber (after network interface config is set)
    // Note: We don't call set_ptp_instance_configuration() - the RavennaNode
    // handles PTP setup automatically based on network interface config.
    // This follows the pattern used in ravennakit examples.
    ptpSubscriber_ = std::make_unique<PtpSubscriber>();
    node_->subscribe_to_ptp_instance(ptpSubscriber_.get()).wait();
    
    // #region agent log
    DEBUG_LOG("RavennaNodeManager.cpp:initialize:ptp_config_success", "PTP subscriber created and subscribed", "{\"domain\":0}");
    // #endregion
    
    // #region agent log
    DEBUG_LOG("RavennaNodeManager.cpp:initialize:ptp_subscriber_added", "PTP subscriber added", "{}");
    // #endregion
    
    // Delay to allow macOS to set up multicast routing for newly-built binaries
    // This helps with the "first launch after build" issue where PTP packets aren't received
    // macOS performs security checks on new binaries which can delay multicast reception
    // #region agent log
    DEBUG_LOG("RavennaNodeManager.cpp:initialize:multicast_settle_delay", "Waiting for multicast routing to settle (500ms)", "{}");
    // #endregion
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // Record initialization time for PTP retry logic
    ptpInitTime_ = std::chrono::steady_clock::now();
    
    // Enable NMOS (IS-04 and IS-05 are enabled by default when NMOS is enabled)
    rav::nmos::Node::Configuration nmosConfig;
    nmosConfig.id = boost::uuids::random_generator()();  // Required: generate a unique UUID for this node
    nmosConfig.enabled = true;
    nmosNodeId_ = nmosConfig.id;
    nmosConfig.api_port = 80;  // Default to 80 (as tested), fallback to 5555 if binding fails
    nmosConfig.label = "TestAES67Sender";
    nmosConfig.description = "RAVENNA AES67 Sender";

    // Prefer registry operation.
    // - If NMOS_REGISTRY_ADDRESS is set (e.g. "http://easy-nmos.local:8010"), use manual mode.
    // - Otherwise, use mDNS registry discovery (falls back to p2p only if no registry is available).
    configuredRegistryAddress_.clear();
    if (const char* envRegistry = std::getenv("NMOS_REGISTRY_ADDRESS"); envRegistry && std::string(envRegistry).size() > 0)
    {
        configuredRegistryAddress_ = envRegistry;

        // Allow passing just host/ip (e.g. "nmos-registry.local" or "192.168.12.161")
        // - if no scheme: assume http
        // - if no port: assume 80 (matches nmos-cpp docker default; if you need a different port, specify it explicitly)
        const bool hasScheme = (configuredRegistryAddress_.find("://") != std::string::npos);
        if (!hasScheme)
            configuredRegistryAddress_ = "http://" + configuredRegistryAddress_;

        // crude port detection: if there's no ':' after the scheme separator, append :8010
        const auto schemePos = configuredRegistryAddress_.find("://");
        const auto hostStart = (schemePos == std::string::npos) ? 0u : static_cast<unsigned>(schemePos + 3);
        const auto hostPort = configuredRegistryAddress_.substr(hostStart);
        const bool hasPort = (hostPort.find(':') != std::string::npos);
        if (!hasPort)
            configuredRegistryAddress_ += ":80";

        nmosConfig.operation_mode = rav::nmos::OperationMode::manual;
        nmosConfig.registry_address = configuredRegistryAddress_;
    }
    else
    {
        nmosConfig.operation_mode = rav::nmos::OperationMode::mdns_p2p;
    }

    setNmosConfigSnapshot(nmosConfig);
    
    // #region agent log
    DEBUG_LOG("RavennaNodeManager.cpp:initialize:nmos_config", "NMOS configuration prepared", "{\"id\":\"" + boost::uuids::to_string(nmosConfig.id) + "\",\"port\":" + std::to_string(nmosConfig.api_port) + "}");
    // #endregion
    
    auto nmosResult = node_->set_nmos_configuration(nmosConfig).get();
    if (!nmosResult)
    {
        // If binding to 80 failed, retry with a non-privileged port (common on macOS if launched without permissions)
        nmosConfig.api_port = 5555;
        setNmosConfigSnapshot(nmosConfig);
        auto retryResult = node_->set_nmos_configuration(nmosConfig).get();
        if (retryResult)
        {
            nmosPort_ = nmosConfig.api_port;
            DEBUG_LOG("RavennaNodeManager.cpp:initialize:nmos_success", "NMOS configuration succeeded (retry)", "{\"port\":" + std::to_string(nmosConfig.api_port) + "}");
            try { nmosDeviceId_ = node_->get_nmos_device_id().get(); } catch (...) {}
        }
        else
        {
            nmosResult = retryResult;
        }
    }

    if (!nmosResult)
    {
        // #region agent log
        std::string errorMsg = nmosResult.error();
        std::string escapedError;
        for (char c : errorMsg) {
            if (c == '"') escapedError += "\\\"";
            else if (c == '\\') escapedError += "\\\\";
            else escapedError += c;
        }
        DEBUG_LOG("RavennaNodeManager.cpp:initialize:nmos_failed", "NMOS configuration failed", "{\"error\":\"" + escapedError + "\"}");
        // #endregion
    }
    else
    {
        nmosPort_ = nmosConfig.api_port;
        // #region agent log
        DEBUG_LOG("RavennaNodeManager.cpp:initialize:nmos_success", "NMOS configuration succeeded", "{\"port\":" + std::to_string(nmosConfig.api_port) + "}");
        // #endregion

        // Cache NMOS device id for UI (best-effort)
        try
        {
            nmosDeviceId_ = node_->get_nmos_device_id().get();
        }
        catch (...)
        {
            // Ignore
        }
    }
    
    // #region agent log
    DEBUG_LOG("RavennaNodeManager.cpp:initialize:exit", "Initialization complete", "{\"interface\":\"" + currentInterface_ + "\"}");
    // #endregion
    
    return true;
}

bool RavennaNodeManager::start(const std::string& sessionName)
{
    // #region agent log
    DEBUG_LOG("RavennaNodeManager.cpp:start:entry", "start() called", "{\"sessionName\":\"" + sessionName + "\"}");
    // #endregion
    
    if (!node_)
    {
        // #region agent log
        DEBUG_LOG("RavennaNodeManager.cpp:start:no_node", "node_ is null - cannot start", "{}");
        // #endregion
        return false;
    }
    
    // If already active, do nothing (single-sender UI)
    if (isActive_)
    {
        // #region agent log
        DEBUG_LOG("RavennaNodeManager.cpp:start:already_active", "Already active - returning true", "{}");
        // #endregion
        return true; // Already started
    }
    
    // Build a sender configuration for this new sender (index based on current count)
    const size_t senderIndex = activeSenders_.size();
    auto senderConfig = createSenderConfig(senderIndex, true);
    senderConfig.session_name = sessionName;
    
    // #region agent log
    {
        std::stringstream configData;
        configData << "{\"senderIndex\":" << senderIndex 
                   << ",\"sessionName\":\"" << sessionName 
                   << "\",\"destAddress\":\"" << senderConfig.destinations[0].endpoint.address().to_string() << "\""
                   << ",\"destPort\":" << senderConfig.destinations[0].endpoint.port()
                   << ",\"enabled\":" << (senderConfig.enabled ? "true" : "false") << "}";
        DEBUG_LOG("RavennaNodeManager.cpp:start:config", "Enabling sender 0", configData.str());
    }
    // #endregion
    
    // Create the sender (enabled)
    auto result = node_->create_sender(senderConfig).get();
    if (!result)
    {
        // #region agent log
        std::string errorMsg = result.error();
        std::string escapedError;
        for (char c : errorMsg) {
            if (c == '"') escapedError += "\\\"";
            else if (c == '\\') escapedError += "\\\\";
            else if (c == '\n') escapedError += "\\n";
            else escapedError += c;
        }
        DEBUG_LOG("RavennaNodeManager.cpp:start:create_failed", "create_sender() failed", "{\"error\":\"" + escapedError + "\"}");
        // #endregion
        return false;
    }
    
    SenderInstance instance;
    instance.id = *result;
    instance.config = senderConfig;
    instance.enabled = true;
    activeSenders_.push_back(instance);
    isActive_ = true;
    rtpTimestamp_ = ptpSubscriber_ ? ptpSubscriber_->get_local_clock().now().to_rtp_timestamp32(kSampleRate) : 0;
    
    // #region agent log
    DEBUG_LOG("RavennaNodeManager.cpp:start:success", "Sender created and enabled", "{\"senderId\":\"" + instance.id.to_string() + "\"}");
    // #endregion
    
    return true;
}

void RavennaNodeManager::stop()
{
    if (!node_)
    {
        return;
    }
    
    // #region agent log
    DEBUG_LOG("RavennaNodeManager.cpp:stop:entry", "Stopping RAVENNA node manager", "{\"isActive\":" + std::string(isActive_ ? "true" : "false") + ",\"numActive\":" + std::to_string(activeSenders_.size()) + "}");
    // #endregion
    
    // Remove all active senders
    for (auto& sender : activeSenders_)
    {
        if (sender.id.is_valid())
        {
            node_->remove_sender(sender.id).wait();
            // #region agent log
            DEBUG_LOG("RavennaNodeManager.cpp:stop:sender_removed", "Sender removed", "{\"senderId\":\"" + sender.id.to_string() + "\"}");
            // #endregion
        }
    }
    activeSenders_.clear();
    
    // #region agent log
    DEBUG_LOG("RavennaNodeManager.cpp:stop:complete", "Streaming stopped and senders removed", "{}");
    // #endregion
    
    isActive_ = false;
}

void RavennaNodeManager::shutdown()
{
    if (!node_)
    {
        return;
    }
    
    // #region agent log
    DEBUG_LOG("RavennaNodeManager.cpp:shutdown:entry", "Shutting down RAVENNA node manager", "{\"isActive\":" + std::string(isActive_ ? "true" : "false") + ",\"numActive\":" + std::to_string(activeSenders_.size()) + "}");
    // #endregion
    
    // Remove all senders completely on shutdown
    for (auto& sender : activeSenders_)
    {
        if (sender.id.is_valid())
        {
            node_->remove_sender(sender.id).wait();
            // #region agent log
            DEBUG_LOG("RavennaNodeManager.cpp:shutdown:sender_removed", "Sender removed", "{\"senderId\":\"" + sender.id.to_string() + "\"}");
            // #endregion
        }
    }
    activeSenders_.clear();
    
    // Unsubscribe from PTP instance to properly release multicast group memberships
    if (ptpSubscriber_)
    {
        node_->unsubscribe_from_ptp_instance(ptpSubscriber_.get()).wait();
        // #region agent log
        DEBUG_LOG("RavennaNodeManager.cpp:shutdown:ptp_unsubscribed", "PTP subscriber unsubscribed", "{}");
        // #endregion
    }

    // Unsubscribe from node callbacks before destroying the node
    if (nodeSubscriber_)
    {
        node_->unsubscribe(nodeSubscriber_.get()).wait();
        nodeSubscriber_.reset();
    }
    
    // Reset the node to trigger proper cleanup of sockets and multicast memberships
    node_.reset();
    
    // #region agent log
    DEBUG_LOG("RavennaNodeManager.cpp:shutdown:complete", "RAVENNA node manager fully shut down", "{}");
    // #endregion
    
    isActive_ = false;
}

bool RavennaNodeManager::isActive() const
{
    return isActive_ && !activeSenders_.empty() && activeSenders_[0].id.is_valid();
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
    
    // Send audio data with PTP-synced RTP timestamp to the first active sender
    if (activeSenders_.empty() || !activeSenders_[0].id.is_valid())
    {
        return false;
    }
    
    if (!node_->send_audio_data_realtime(activeSenders_[0].id, bufferView, rtpTimestamp_))
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
        const std::string gmId = parent.grandmaster_identity.to_string();
        // Check if grandmaster ID is valid (not all zeros)
        const bool hasValidGM = (gmId != "00-00-00-00-00-00-00-00");
        
        if (hasValidGM)
        {
            diagnostics += "Has Grandmaster: Yes\n";
            diagnostics += "Grandmaster ID: " + gmId + "\n";
            diagnostics += "Parent Port ID: " + parent.parent_port_identity.clock_identity.to_string() + "\n";
            diagnostics += "GM Priority1: " + std::to_string(parent.grandmaster_priority1) + "\n";
            diagnostics += "GM Priority2: " + std::to_string(parent.grandmaster_priority2) + "\n";
        }
        else
        {
            diagnostics += "Has Grandmaster: No (searching...)\n";
            diagnostics += "Grandmaster ID: None (waiting for PTP announce)\n";
        }
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

bool RavennaNodeManager::checkAndRetryPtpIfStuck()
{
    // Stop if we've exhausted retries
    if (!node_ || !ptpSubscriber_ || ptpRetryCount_ >= 3)
    {
        return false;
    }
    
    // Check if we've been waiting long enough (10 seconds since init)
    const auto elapsed = std::chrono::steady_clock::now() - ptpInitTime_;
    if (elapsed < std::chrono::seconds(10))
    {
        return false; // Not enough time has passed
    }
    
    // Check if PTP is stuck in listening state (no grandmaster detected)
    const auto state = ptpSubscriber_->getPortState();
    const bool hasValidGrandmaster = ptpSubscriber_->hasParent() && 
        ptpSubscriber_->getParent().grandmaster_identity.to_string() != "00-00-00-00-00-00-00-00";
    
    if (state == rav::ptp::State::listening && !hasValidGrandmaster)
    {
        ptpRetryCount_++;
        
        // #region agent log
        DEBUG_LOG("RavennaNodeManager.cpp:checkAndRetryPtpIfStuck", "PTP stuck in listening state, attempting simple retry", "{\"state\":\"listening\",\"hasValidGrandmaster\":false,\"retryCount\":" + std::to_string(ptpRetryCount_) + "}");
        // #endregion
        
        // Simple approach: Just unsubscribe and re-subscribe
        // Don't call set_ptp_instance_configuration() as it may corrupt PTP state
        node_->unsubscribe_from_ptp_instance(ptpSubscriber_.get()).wait();
        
        // Wait a bit for cleanup
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        
        // Re-subscribe
        node_->subscribe_to_ptp_instance(ptpSubscriber_.get()).wait();
        
        // Reset the init time to give it another 10 seconds
        ptpInitTime_ = std::chrono::steady_clock::now();
        
        // #region agent log
        DEBUG_LOG("RavennaNodeManager.cpp:checkAndRetryPtpIfStuck:complete", "PTP simple retry completed", "{\"retryCount\":" + std::to_string(ptpRetryCount_) + "}");
        // #endregion
        
        return true;
    }
    
    return false;
}

boost::asio::ip::address_v4 RavennaNodeManager::generateMulticastAddress(size_t senderIndex)
{
    rav::NetworkInterfaceConfig netConfigForMulticast;
    netConfigForMulticast.set_interface(rav::rank::primary, currentInterface_);
    auto interfaceAddrs = netConfigForMulticast.get_interface_ipv4_addresses();
    
    if (!interfaceAddrs.empty()) {
        auto ifaceBytes = interfaceAddrs[0].to_bytes();
        // Generate unique multicast address for each sender in 239.x.y.z range
        // Use senderIndex to create unique addresses: 239.{octet3}.{octet4}.{1+senderIndex}
        // This gives us up to 254 unique addresses per interface (1-254)
        uint8_t lastOctet = static_cast<uint8_t>(1 + (senderIndex % 254));
        return boost::asio::ip::address_v4({239, ifaceBytes[2], ifaceBytes[3], lastOctet});
    }
    // Fallback to a standard AES67 multicast address range
    uint8_t lastOctet = static_cast<uint8_t>(1 + (senderIndex % 254));
    return boost::asio::ip::address_v4({239, 69, 1, lastOctet});
}

rav::RavennaSender::Configuration RavennaNodeManager::createSenderConfig(size_t senderIndex, bool enabled)
{
    rav::RavennaSender::Configuration config;
    config.session_name = "TestAES67Sender_Ch" + std::to_string(senderIndex + 1);
    
    // Set audio format: 48kHz, 24-bit, mono
    config.audio_format.encoding = kEncoding;
    config.audio_format.sample_rate = kSampleRate;
    config.audio_format.num_channels = kNumChannels;
    config.audio_format.byte_order = rav::AudioFormat::ByteOrder::be;
    config.audio_format.ordering = rav::AudioFormat::ChannelOrdering::interleaved;
    
    // Set packet time (1ms is standard for AES67)
    config.packet_time = rav::aes67::PacketTime::ms_1();
    
    // Set payload type (dynamic range 96-127)
    // Each sender gets a unique payload type: 98, 99, 100, ... (wrapping at 127 back to 96)
    config.payload_type = static_cast<uint8_t>(98 + (senderIndex % 30));
    
    // Set TTL for multicast
    config.ttl = 15;
    
    // Set destination multicast address (unique for each sender)
    auto multicastAddr = generateMulticastAddress(senderIndex);
    config.destinations.emplace_back(
        rav::RavennaSender::Destination {
            rav::rank::primary,
            {multicastAddr, static_cast<uint16_t>(5004 + senderIndex)}, // Unique port per sender
            true
        }
    );
    
    config.enabled = enabled;
    
    return config;
}

// (createAllSenders removed - senders are now created on demand)

} // namespace AudioApp

