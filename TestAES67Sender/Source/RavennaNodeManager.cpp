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
#include <boost/uuid/string_generator.hpp>
#include <juce_core/juce_core.h>

#include <thread>
#include <cmath>
#include <sstream>
#include <fstream>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <algorithm>
#include <array>

#ifdef __APPLE__
#include <pthread.h>
#endif

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

juce::File RavennaNodeManager::getNodeIdFile() const
{
    auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                   .getChildFile("TestAES67Sender");
    if (!dir.exists())
        (void)dir.createDirectory();
    return dir.getChildFile("nmos_node_id.txt");
}

boost::uuids::uuid RavennaNodeManager::loadOrCreateNodeId()
{
    auto file = getNodeIdFile();
    if (file.existsAsFile())
    {
        const auto text = file.loadFileAsString().trim();
        if (text.isNotEmpty())
        {
            try
            {
                boost::uuids::string_generator gen;
                return gen(text.toStdString());
            }
            catch (...)
            {
                // fallthrough to new id
            }
        }
    }

    const auto newId = boost::uuids::random_generator()();
    persistNodeId(newId);
    return newId;
}

void RavennaNodeManager::persistNodeId(const boost::uuids::uuid& id)
{
    auto file = getNodeIdFile();
    (void)file.create();
    file.replaceWithText(juce::String(boost::uuids::to_string(id)));
}

std::optional<std::pair<std::string, std::string>> RavennaNodeManager::resolveLocalNmosIdsForLabel(const std::string& label) const
{
    if (nmosPort_ == 0)
        return std::nullopt;

    const auto urlString = "http://127.0.0.1:" + std::to_string(nmosPort_) + "/x-nmos/node/v1.3/senders";
    juce::URL url { juce::String(urlString) };
    auto stream = url.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                                            .withConnectionTimeoutMs(1000)
                                            .withNumRedirectsToFollow(1));
    if (!stream)
        return std::nullopt;

    auto text = stream->readEntireStreamAsString();
    juce::var json = juce::JSON::parse(text);
    if (!json.isArray())
        return std::nullopt;

    for (const auto& item : *json.getArray())
    {
        auto* obj = item.getDynamicObject();
        if (!obj)
            continue;
        const auto idVar = obj->getProperty("id");
        const auto flowVar = obj->getProperty("flow_id");
        const auto labelVar = obj->getProperty("label");
        if (labelVar.isString() && labelVar.toString().toStdString() == label)
        {
            if (idVar.isString() && flowVar.isString())
            {
                return std::make_pair(idVar.toString().toStdString(), flowVar.toString().toStdString());
            }
        }
    }
    return std::nullopt;
}

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

std::string RavennaNodeManager::getRegistryBaseUrl() const
{
    std::lock_guard<std::mutex> lock(nmosMutex_);

    auto url = configuredRegistryAddress_;
    if (url.empty())
    {
        if (!nmosRegistryInfo_.address.empty())
        {
            url = nmosRegistryInfo_.address;
            const bool hasScheme = (url.find("://") != std::string::npos);
            if (!hasScheme)
                url = "http://" + url;

            // If no explicit port in address, append discovered api_port when available
            const auto schemePos = url.find("://");
            const auto hostStart = (schemePos == std::string::npos) ? 0u : static_cast<unsigned>(schemePos + 3);
            const auto hostPort = url.substr(hostStart);
            const bool hasPort = (hostPort.find(':') != std::string::npos);
            if (!hasPort && nmosRegistryInfo_.api_port != 0)
                url += ":" + std::to_string(nmosRegistryInfo_.api_port);
        }
    }
    return url;
}

std::string RavennaNodeManager::getSenderDiagnostics() const
{
    std::stringstream ss;
    ss << "Interface: " << (currentInterface_.empty() ? "(auto)" : currentInterface_) << "\n";
    ss << "Active: " << (isActive_.load() ? "Yes" : "No") << "\n";
    ss << "Senders: " << activeSenders_.size() << "\n";
    ss << "NMOS port: " << nmosPort_ << "\n";

    ss << "\n--- SENDER STATUS ---\n";
    ss << "Mode: paced + drift compensated (per-sender ASRC)\n";
    ss << "PTP calibrated: " << ((ptpSubscriber_ && ptpSubscriber_->get_local_clock().is_calibrated()) ? "Yes" : "No") << "\n";

    for (size_t i = 0; i < activeSenders_.size(); ++i)
    {
        const auto& s = activeSenders_[i];
        ss << "\nSender[" << i << "]\n";
        ss << "  ID: " << s.id.to_string() << "\n";
        ss << "  Session: " << s.config.session_name << "\n";
        ss << "  Enabled: " << (s.enabled ? "Yes" : "No") << "\n";
        ss << "  Input channel: " << (s.logicalInputChannel + 1) << "\n";
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
        if (!s.nmosSenderId.empty())
            ss << "  NMOS sender_id: " << s.nmosSenderId << "\n";
        if (!s.nmosFlowId.empty())
            ss << "  NMOS flow_id: " << s.nmosFlowId << "\n";

        if (s.stats)
        {
            ss << "  Sent: " << s.stats->sentPackets.load(std::memory_order_relaxed)
               << " packets (" << s.stats->sentFrames.load(std::memory_order_relaxed) << " frames)\n";
            ss << "  RTP ts: " << s.stats->lastRtpTimestamp.load(std::memory_order_relaxed) << "\n";
            ss << "  Drift: " << std::fixed << std::setprecision(1) << s.stats->ratioPpm.load(std::memory_order_relaxed)
               << " ppm (min=" << s.stats->ratioPpmMin.load(std::memory_order_relaxed)
               << ", max=" << s.stats->ratioPpmMax.load(std::memory_order_relaxed) << ")\n";
            ss << "  Buffered: fifo=" << s.stats->fifoOnly.load(std::memory_order_relaxed)
               << " asrc=" << s.stats->asrcOnly.load(std::memory_order_relaxed)
               << " err=" << s.stats->lastFifoError.load(std::memory_order_relaxed) << "\n";
            ss << "  Underflows: " << s.stats->asrcUnderflows.load(std::memory_order_relaxed)
               << "  Overflows: " << s.stats->fifoOverflows.load(std::memory_order_relaxed) << "\n";
        }
    }
    return ss.str();
}

// RavennaNodeManager implementation
RavennaNodeManager::RavennaNodeManager()
    : isActive_(false)
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
    nmosConfig.id = loadOrCreateNodeId();  // Persisted Node ID for registry friendliness
    nmosConfig.enabled = true;
    nmosNodeId_ = nmosConfig.id;

    // Prefer unprivileged port by default; allow override via env NMOS_NODE_PORT
    uint16_t desiredNmosPort = 5555;
    if (const char* envPort = std::getenv("NMOS_NODE_PORT"); envPort && std::strlen(envPort) > 0)
    {
        const int parsed = std::atoi(envPort);
        if (parsed > 0 && parsed < 65535)
            desiredNmosPort = static_cast<uint16_t>(parsed);
    }
    nmosConfig.api_port = desiredNmosPort;
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
        // - if no port: assume 8010 (common easy-nmos / nmos-cpp default)
        const bool hasScheme = (configuredRegistryAddress_.find("://") != std::string::npos);
        if (!hasScheme)
            configuredRegistryAddress_ = "http://" + configuredRegistryAddress_;

        const auto schemePos = configuredRegistryAddress_.find("://");
        const auto hostStart = (schemePos == std::string::npos) ? 0u : static_cast<unsigned>(schemePos + 3);
        const auto hostPort = configuredRegistryAddress_.substr(hostStart);
        const bool hasPort = (hostPort.find(':') != std::string::npos);
        if (!hasPort)
            configuredRegistryAddress_ += ":8010";

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
    if (!nmosResult && nmosConfig.api_port != 5555)
    {
        // Retry with fallback unprivileged port if custom port failed
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

    // If already active, do nothing
    if (isActive_)
    {
        DEBUG_LOG("RavennaNodeManager.cpp:start:already_active", "Already active - returning true", "{}");
        return true;
    }

    auto id = createSender(sessionName, 0);
    const bool ok = id.is_valid();
    if (ok)
    {
        DEBUG_LOG("RavennaNodeManager.cpp:start:success", "Sender created and enabled", "{\"senderId\":\"" + id.to_string() + "\"}");
    }
    else
    {
        DEBUG_LOG("RavennaNodeManager.cpp:start:create_failed", "Failed to create initial sender", "{}");
    }
    return ok;
}

rav::Id RavennaNodeManager::createSender(const std::string& sessionName, int logicalInputChannel)
{
    if (!node_)
        return {};

    if (activeSenders_.size() >= 64)
        return {};

    const size_t senderIndex = activeSenders_.size();
    auto senderConfig = createSenderConfig(senderIndex, true);
    senderConfig.session_name = sessionName;

    {
        std::stringstream configData;
        configData << "{\"senderIndex\":" << senderIndex
                   << ",\"sessionName\":\"" << sessionName
                   << "\",\"destAddress\":\"" << senderConfig.destinations[0].endpoint.address().to_string() << "\""
                   << ",\"destPort\":" << senderConfig.destinations[0].endpoint.port()
                   << ",\"enabled\":" << (senderConfig.enabled ? "true" : "false") << "}";
        DEBUG_LOG("RavennaNodeManager.cpp:createSender:config", "Creating sender", configData.str());
    }

    auto result = node_->create_sender(senderConfig).get();
    if (!result)
    {
        std::string errorMsg = result.error();
        std::string escapedError;
        for (char c : errorMsg) {
            if (c == '"') escapedError += "\\\"";
            else if (c == '\\') escapedError += "\\\\";
            else if (c == '\n') escapedError += "\\n";
            else escapedError += c;
        }
        DEBUG_LOG("RavennaNodeManager.cpp:createSender:create_failed", "create_sender() failed", "{\"error\":\"" + escapedError + "\"}");
        return {};
    }

    SenderInstance instance;
    instance.id = *result;
    instance.config = senderConfig;
    instance.enabled = true;
    instance.logicalInputChannel = std::max(0, logicalInputChannel);
    instance.stats = std::make_shared<SenderInstance::RuntimeStats>();
    resetPipeline(instance.pipeline);

    if (auto ids = resolveLocalNmosIdsForLabel(sessionName))
    {
        instance.nmosSenderId = ids->first;
        instance.nmosFlowId = ids->second;
    }

    activeSenders_.push_back(std::move(instance));
    isActive_ = true;

    // Ensure paced sender thread is running when we have at least one sender
    if (!sendThreadRunning_.load(std::memory_order_acquire))
        startSendThread();

    return instance.id;
}

bool RavennaNodeManager::removeSender(rav::Id id)
{
    if (!node_ || !id.is_valid())
        return false;

    auto it = std::find_if(activeSenders_.begin(), activeSenders_.end(), [&](const SenderInstance& s) { return s.id == id; });
    if (it == activeSenders_.end())
        return false;

    node_->remove_sender(id).wait();
    DEBUG_LOG("RavennaNodeManager.cpp:removeSender", "Sender removed", "{\"senderId\":\"" + id.to_string() + "\"}");
    activeSenders_.erase(it);
    if (activeSenders_.empty())
    {
        isActive_ = false;
        stopSendThread();
    }
    return true;
}

bool RavennaNodeManager::setSenderInputChannel(rav::Id id, int logicalInputChannel)
{
    auto it = std::find_if(activeSenders_.begin(), activeSenders_.end(), [&](const SenderInstance& s) { return s.id == id; });
    if (it == activeSenders_.end())
        return false;

    it->logicalInputChannel = std::max(0, logicalInputChannel);
    it->pipeline.timestampInitialized = false; // re-sync timestamp in paced thread
    it->pipeline.rtpTimestamp = 0;
    return true;
}

void RavennaNodeManager::stop()
{
    if (!node_)
    {
        return;
    }

    stopSendThread();

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

    // Stop sender thread first (it may call into node_)
    stopSendThread();
    
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
    return isActive_ && !activeSenders_.empty();
}

void RavennaNodeManager::processAudio(const float* const* inputChannelData,
                                      int numInputChannels,
                                      int numSamples,
                                      const juce::BigInteger& activeInputs)
{
    if (!isActive() || !inputChannelData || numInputChannels <= 0 || numSamples <= 0)
        return;

    // Build mapping from logical active-channel index -> actual device channel index
    std::vector<int> logicalToPhysical;
    logicalToPhysical.reserve(static_cast<size_t>(numInputChannels));
    for (int ch = 0; ch < numInputChannels; ++ch)
    {
        if (activeInputs[ch])
        {
            logicalToPhysical.push_back(ch);
        }
    }

    for (auto& sender : activeSenders_)
    {
        if (!sender.enabled || !sender.id.is_valid())
            continue;

        if (sender.logicalInputChannel < 0 || sender.logicalInputChannel >= static_cast<int>(logicalToPhysical.size()))
            continue;

        const int physicalIndex = logicalToPhysical[static_cast<size_t>(sender.logicalInputChannel)];
        if (physicalIndex < 0 || physicalIndex >= numInputChannels)
            continue;

        const float* channelData = inputChannelData[physicalIndex];
        if (channelData == nullptr)
            continue;

        // Producer side: enqueue audio into per-sender FIFO.
        const auto written = fifoWriteSamples(sender.pipeline, channelData, static_cast<uint32_t>(numSamples));
        if (written < static_cast<uint32_t>(numSamples))
        {
            sender.pipeline.fifoOverflows.fetch_add(1, std::memory_order_relaxed);
            if (sender.stats)
                sender.stats->fifoOverflows.store(sender.pipeline.fifoOverflows.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
    }
}

uint32_t RavennaNodeManager::fifoLevel(const SenderInstance::Pipeline& p) const
{
    const auto w = p.fifoWrite.load(std::memory_order_acquire);
    const auto r = p.fifoRead.load(std::memory_order_acquire);
    const auto avail = (w >= r) ? (w - r) : 0;
    return static_cast<uint32_t>(std::min<uint64_t>(avail, static_cast<uint64_t>(p.fifo.size())));
}

uint32_t RavennaNodeManager::fifoWriteSamples(SenderInstance::Pipeline& p, const float* data, uint32_t n)
{
    if (p.fifo.empty() || p.fifoMask == 0 || n == 0)
        return 0;

    const auto w = p.fifoWrite.load(std::memory_order_relaxed);
    const auto r = p.fifoRead.load(std::memory_order_acquire);
    const uint64_t used = (w >= r) ? (w - r) : 0;
    const uint64_t cap = static_cast<uint64_t>(p.fifo.size());
    const uint64_t free = (used >= cap) ? 0 : (cap - used);
    const uint32_t toWrite = static_cast<uint32_t>(std::min<uint64_t>(free, n));

    for (uint32_t i = 0; i < toWrite; ++i)
        p.fifo[(w + i) & p.fifoMask] = data[i];

    p.fifoWrite.store(w + toWrite, std::memory_order_release);
    return toWrite;
}

uint32_t RavennaNodeManager::fifoReadSamples(SenderInstance::Pipeline& p, float* dst, uint32_t n)
{
    if (p.fifo.empty() || p.fifoMask == 0 || n == 0)
        return 0;

    const auto r = p.fifoRead.load(std::memory_order_relaxed);
    const auto w = p.fifoWrite.load(std::memory_order_acquire);
    const uint64_t avail = (w >= r) ? (w - r) : 0;
    const uint32_t toRead = static_cast<uint32_t>(std::min<uint64_t>(avail, n));

    for (uint32_t i = 0; i < toRead; ++i)
        dst[i] = p.fifo[(r + i) & p.fifoMask];

    p.fifoRead.store(r + toRead, std::memory_order_release);
    return toRead;
}

void RavennaNodeManager::startSendThread()
{
    stopSendThread();

    buildSincTable();

    sendThreadRunning_.store(true, std::memory_order_release);
    sendThread_ = std::thread([this] { sendThreadMain(); });
}

void RavennaNodeManager::stopSendThread()
{
    const bool wasRunning = sendThreadRunning_.exchange(false, std::memory_order_acq_rel);
    if (wasRunning && sendThread_.joinable())
        sendThread_.join();
}

void RavennaNodeManager::sendThreadMain()
{
#ifdef __APPLE__
    // Best-effort: increase scheduling QoS to reduce wakeup jitter.
    // If this fails, we still function; it just increases the risk of late wakes.
    (void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif

    auto nextWake = std::chrono::steady_clock::now();

    std::array<float, kFramesPerPacket> out {};
    // Deadline monitoring: count late wakes for debugging (best-effort)
    uint32_t deadlineMisses = 0;

    while (sendThreadRunning_.load(std::memory_order_acquire))
    {
        nextWake += std::chrono::milliseconds(1);

        // Deadline monitoring: detect if we woke up late (OS scheduling hiccup).
        const auto now = std::chrono::steady_clock::now();
        if (now > nextWake + std::chrono::milliseconds(2))
        {
            deadlineMisses++;
            // Don't try to "catch up" by sending bursts; just re-align schedule.
            nextWake = now;
        }

        if (!isActive() || !ptpSubscriber_)
        {
            std::this_thread::sleep_until(nextWake);
            continue;
        }

        if (activeSenders_.empty())
        {
            std::this_thread::sleep_until(nextWake);
            continue;
        }

        for (auto& s : activeSenders_)
        {
            if (!s.enabled || !s.id.is_valid())
                continue;

            auto& p = s.pipeline;
            const uint32_t fifoLvl = fifoLevel(p);
            const uint32_t asrcLvl = asrcBufferedFrames(p);
            const uint32_t totalBuffered = fifoLvl + asrcLvl;

            if (!p.timestampInitialized)
            {
                if (!ptpSubscriber_->get_local_clock().is_calibrated())
                    continue;

                if (totalBuffered < (kTargetFifoLevel + kSincTaps))
                {
                    // Allow a larger burst fill to quickly reach target buffering on startup / with callback jitter.
                    (void)asrcFillFromFifo(p, kTargetFifoLevel + kSincTaps + kFramesPerPacket * 2, 4096);
                    continue;
                }

                const auto nowTs = ptpSubscriber_->get_local_clock().now().to_rtp_timestamp32(kSampleRate);
                p.rtpTimestamp = nowTs + kTargetFifoLevel;
                p.timestampInitialized = true;
            }

            const int32_t error = static_cast<int32_t>(totalBuffered) - static_cast<int32_t>(kTargetFifoLevel);

            constexpr double kP = 1.0 / 2'000'000.0;
            constexpr double kI = 1.0 / 50'000'000.0;
            constexpr double kMaxPpm = 2000.0;
            constexpr double kRatioSmoothing = 0.01;

            p.ratioIntegral += static_cast<double>(error) * kI;
            p.ratioIntegral = std::clamp(p.ratioIntegral, -kMaxPpm * 1e-6, kMaxPpm * 1e-6);

            p.ratio = 1.0 + static_cast<double>(error) * kP + p.ratioIntegral;
            p.ratio = std::clamp(p.ratio, 1.0 - kMaxPpm * 1e-6, 1.0 + kMaxPpm * 1e-6);
            p.ratioSmoothed += (p.ratio - p.ratioSmoothed) * kRatioSmoothing;

            constexpr uint32_t kAsrcHeadroom = kSincTaps + (kFramesPerPacket * 4);
            (void)asrcFillFromFifo(p, kTargetFifoLevel + kAsrcHeadroom, 4096);

            bool underflow = false;
            for (uint32_t i = 0; i < kFramesPerPacket; ++i)
            {
                const float y = asrcResampleOne(p, p.ratioSmoothed);
                out[i] = y;
                p.lastSample = y;
                if (!underflow && p.asrcWrite < (p.asrcRead + static_cast<uint64_t>(kSincTaps) + 4))
                    underflow = true;
            }

            const float* channels[] = { out.data() };
            rav::AudioBufferView<const float> bufferView(channels, kNumChannels, static_cast<size_t>(kFramesPerPacket));
            (void)node_->send_audio_data_realtime(s.id, bufferView, p.rtpTimestamp);
            p.rtpTimestamp += kFramesPerPacket;

            if (s.stats)
            {
                s.stats->sentPackets.fetch_add(1, std::memory_order_relaxed);
                s.stats->sentFrames.fetch_add(kFramesPerPacket, std::memory_order_relaxed);
                s.stats->lastRtpTimestamp.store(p.rtpTimestamp, std::memory_order_relaxed);
                s.stats->fifoOnly.store(fifoLvl, std::memory_order_relaxed);
                s.stats->asrcOnly.store(asrcLvl, std::memory_order_relaxed);
                s.stats->lastFifoError.store(error, std::memory_order_relaxed);

                const double ppm = (p.ratioSmoothed - 1.0) * 1e6;
                s.stats->ratioPpm.store(ppm, std::memory_order_relaxed);
                const auto prevMin = s.stats->ratioPpmMin.load(std::memory_order_relaxed);
                const auto prevMax = s.stats->ratioPpmMax.load(std::memory_order_relaxed);
                if (s.stats->sentPackets.load(std::memory_order_relaxed) <= 1)
                {
                    s.stats->ratioPpmMin.store(ppm, std::memory_order_relaxed);
                    s.stats->ratioPpmMax.store(ppm, std::memory_order_relaxed);
                }
                else
                {
                    s.stats->ratioPpmMin.store(std::min(prevMin, ppm), std::memory_order_relaxed);
                    s.stats->ratioPpmMax.store(std::max(prevMax, ppm), std::memory_order_relaxed);
                }
                if (underflow)
                    s.stats->asrcUnderflows.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // Keep the compiler happy (we may surface this later in UI)
        (void)deadlineMisses;
        std::this_thread::sleep_until(nextWake);
    }
}

void RavennaNodeManager::buildSincTable()
{
    // Windowed-sinc lowpass filter table for fractional delay interpolation.
    // We use a Kaiser window; cutoff < 1.0 to avoid imaging/aliasing near Nyquist.
    constexpr double cutoff = 0.90;      // normalized (Nyquist=1.0)
    constexpr double beta = 8.6;         // Kaiser beta (good stopband attenuation ~80dB)
    constexpr double pi = 3.14159265358979323846;
    const int taps = static_cast<int>(kSincTaps);
    const int half = taps / 2;

    auto besselI0 = [](double x) -> double {
        // Approximation of modified Bessel function I0 (sufficient for window generation)
        double sum = 1.0;
        double y = x * x / 4.0;
        double t = y;
        for (int k = 1; k < 20; ++k)
        {
            sum += t;
            t *= y / (static_cast<double>(k + 1) * static_cast<double>(k + 1));
        }
        return sum;
    };

    const double denom = besselI0(beta);
    sincTable_.assign(kSincPhases * kSincTaps, 0.0f);

    for (uint32_t p = 0; p < kSincPhases; ++p)
    {
        const double frac = static_cast<double>(p) / static_cast<double>(kSincPhases); // [0,1)
        double sum = 0.0;

        for (int i = 0; i < taps; ++i)
        {
            const int n = i - (half - 1); // symmetric around 0
            const double x = static_cast<double>(n) - frac;

            double sinc;
            const double a = pi * x;
            if (std::abs(a) < 1e-12)
                sinc = 1.0;
            else
                sinc = std::sin(cutoff * a) / (cutoff * a);

            // Kaiser window
            const double r = static_cast<double>(i) / static_cast<double>(taps - 1); // 0..1
            const double wArg = beta * std::sqrt(std::max(0.0, 1.0 - std::pow(2.0 * r - 1.0, 2.0)));
            const double win = besselI0(wArg) / denom;

            const double coeff = sinc * win * cutoff;
            sincTable_[p * kSincTaps + static_cast<uint32_t>(i)] = static_cast<float>(coeff);
            sum += coeff;
        }

        // Normalize gain at DC for each phase
        const double inv = (sum != 0.0) ? (1.0 / sum) : 1.0;
        for (uint32_t i = 0; i < kSincTaps; ++i)
            sincTable_[p * kSincTaps + i] = static_cast<float>(static_cast<double>(sincTable_[p * kSincTaps + i]) * inv);
    }
}

void RavennaNodeManager::resetPipeline(SenderInstance::Pipeline& p)
{
    p.fifo.assign(kFifoCapacityFrames, 0.0f);
    p.fifoMask = p.fifo.size() - 1;
    p.fifoWrite.store(0, std::memory_order_release);
    p.fifoRead.store(0, std::memory_order_release);
    p.fifoOverflows.store(0, std::memory_order_release);

    p.asrcRing.assign(kAsrcRingFrames, 0.0f);
    p.asrcMask = p.asrcRing.size() - 1;
    // Start read/write at kSincTaps so the interpolator never reads "before start" (prevents startup pops).
    p.asrcWrite = kSincTaps;
    p.asrcRead = kSincTaps;
    p.asrcFrac = 0.0;
    p.asrcTmp.assign(512, 0.0f);

    p.ratio = 1.0;
    p.ratioSmoothed = 1.0;
    p.ratioIntegral = 0.0;
    p.lastSample = 0.0f;
    p.rtpTimestamp = 0;
    p.timestampInitialized = false;
}

uint32_t RavennaNodeManager::asrcBufferedFrames(const SenderInstance::Pipeline& p) const
{
    const uint64_t used = (p.asrcWrite >= p.asrcRead) ? (p.asrcWrite - p.asrcRead) : 0;
    return static_cast<uint32_t>(std::min<uint64_t>(used, static_cast<uint64_t>(p.asrcRing.size())));
}

uint32_t RavennaNodeManager::asrcFillFromFifo(SenderInstance::Pipeline& p, uint32_t desiredBufferedFrames, uint32_t maxFramesPerCall)
{
    if (p.asrcRing.empty() || p.asrcMask == 0)
        return 0;

    const uint64_t buffered = (p.asrcWrite >= p.asrcRead) ? (p.asrcWrite - p.asrcRead) : 0;
    if (buffered >= desiredBufferedFrames)
        return 0;

    const uint64_t need = static_cast<uint64_t>(desiredBufferedFrames) - buffered;

    // Free space
    const uint64_t cap = static_cast<uint64_t>(p.asrcRing.size());
    const uint64_t used = (p.asrcWrite >= p.asrcRead) ? (p.asrcWrite - p.asrcRead) : 0;
    const uint64_t free = (used >= cap) ? 0 : (cap - used);
    const uint32_t toPull = static_cast<uint32_t>(
        std::min<uint64_t>(std::min<uint64_t>(std::min<uint64_t>(free, need), maxFramesPerCall), 4096)
    );
    if (toPull == 0)
        return 0;

    if (p.asrcTmp.size() < toPull)
        p.asrcTmp.resize(toPull);

    const uint32_t got = fifoReadSamples(p, p.asrcTmp.data(), toPull);

    for (uint32_t i = 0; i < got; ++i)
        p.asrcRing[(p.asrcWrite + i) & p.asrcMask] = p.asrcTmp[i];

    p.asrcWrite += got;
    return got;
}

float RavennaNodeManager::asrcGetSample(const SenderInstance::Pipeline& p, uint64_t idx) const
{
    return p.asrcRing[idx & p.asrcMask];
}

float RavennaNodeManager::asrcResampleOne(SenderInstance::Pipeline& p, double ratio)
{
    const uint64_t needAhead = p.asrcRead + static_cast<uint64_t>(kSincTaps) + 4;
    if (p.asrcWrite < needAhead)
        return p.lastSample;

    const double frac = std::clamp(p.asrcFrac, 0.0, 0.999999);
    const uint32_t phase = static_cast<uint32_t>(frac * static_cast<double>(kSincPhases));
    const uint32_t base = phase * kSincTaps;

    const int taps = static_cast<int>(kSincTaps);
    const int half = taps / 2;
    const int centerOffset = half - 1;

    double acc = 0.0;
    for (int i = 0; i < taps; ++i)
    {
        const int n = i - centerOffset;
        const uint64_t si = static_cast<uint64_t>(static_cast<int64_t>(p.asrcRead) + n);
        const float x = asrcGetSample(p, si);
        const float c = sincTable_[base + static_cast<uint32_t>(i)];
        acc += static_cast<double>(x) * static_cast<double>(c);
    }

    p.asrcFrac += ratio;
    while (p.asrcFrac >= 1.0)
    {
        p.asrcFrac -= 1.0;
        p.asrcRead += 1;
    }

    return static_cast<float>(acc);
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

std::vector<RavennaNodeManager::SenderInfo> RavennaNodeManager::listSenders() const
{
    std::vector<SenderInfo> out;
    for (const auto& s : activeSenders_)
    {
        SenderInfo info;
        info.id = s.id;
        info.sessionName = s.config.session_name;
        if (!s.config.destinations.empty())
        {
            info.destAddress = s.config.destinations[0].endpoint.address().to_string();
            info.destPort = s.config.destinations[0].endpoint.port();
        }
        info.logicalInputChannel = s.logicalInputChannel;
        info.nmosSenderId = s.nmosSenderId;
        info.nmosFlowId = s.nmosFlowId;
        out.push_back(std::move(info));
    }
    return out;
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


