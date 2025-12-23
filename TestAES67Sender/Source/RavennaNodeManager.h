#pragma once

#include "ravennakit/ravenna/ravenna_node.hpp"
#include "ravennakit/core/system.hpp"
#include "ravennakit/core/audio/audio_format.hpp"
#include "ravennakit/core/audio/audio_buffer_view.hpp"
#include "ravennakit/core/util/id.hpp"
#include "ravennakit/ptp/ptp_instance.hpp"
#include <juce_core/juce_core.h>

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

namespace AudioApp
{

/**
 * Manages the RAVENNA node, PTP synchronization, NMOS, and audio sender.
 * This class encapsulates all the complexity of setting up and using ravennakit.
 */
class RavennaNodeManager
{
public:
    RavennaNodeManager();
    ~RavennaNodeManager();

    /**
     * Initialize the RAVENNA node with system checks and basic configuration.
     * @return true if initialization was successful, false otherwise
     */
    bool initialize();

    /**
     * Start the RAVENNA node and create the audio sender.
     * @param sessionName Name for the RAVENNA session
     * @return true if start was successful, false otherwise
     */
    bool start(const std::string& sessionName = "TestAES67Sender");

    /**
     * Stop sending audio (removes all senders but keeps NMOS registration active).
     * Senders can be recreated with start() or createSender().
     */
    void stop();
    
    /**
     * Fully shutdown the RAVENNA node manager (called on app exit).
     * This removes all resources and releases network resources.
     */
    void shutdown();

    /**
     * Check if the node is currently active (started and sender created).
     * @return true if active, false otherwise
     */
    bool isActive() const;

    /**
     * Send audio for all active senders (fan-out by per-sender input channel).
     * @param inputChannelData Raw device buffers from JUCE callback
     * @param numInputChannels Number of input channels in buffer
     * @param numSamples Samples per channel
     * @param activeInputs Bitmask of active inputs (from JUCE device)
     */
    void processAudio(const float* const* inputChannelData, int numInputChannels, int numSamples, const juce::BigInteger& activeInputs);

    /**
     * Create a sender with the given session name and logical input channel index.
     * @return rav::Id of the created sender, or invalid Id on failure.
     */
    rav::Id createSender(const std::string& sessionName, int logicalInputChannel);

    /**
     * Remove a sender by id (deregisters and stops streaming).
     */
    bool removeSender(rav::Id id);

    /**
     * Change the logical input channel (index within active inputs) for a sender.
     */
    bool setSenderInputChannel(rav::Id id, int logicalInputChannel);

    /**
     * Get list of available network interface names.
     * @return Vector of interface names
     */
    std::vector<std::string> getAvailableInterfaces() const;

    /**
     * Set the network interface to use for RAVENNA streaming.
     * @param interfaceName Name of the interface (e.g., "en0", "eth0")
     * @return true if interface was set successfully, false otherwise
     */
    bool setNetworkInterface(const std::string& interfaceName);

    /**
     * Get the current network interface name.
     * @return Current interface name, or empty string if not set
     */
    std::string getCurrentInterface() const;

    /**
     * Check if PTP is synchronized.
     * @return true if PTP is synchronized, false otherwise
     */
    bool isPtpSynchronized();
    
    /**
     * Get PTP diagnostic information as a string.
     * @return String containing PTP status, grandmaster info, port count, etc.
     */
    std::string getPtpDiagnostics() const;
    
    /**
     * Set PTP domain number (default is 0).
     * @param domainNumber PTP domain number (0-127)
     * @return true if domain was set successfully
     */
    bool setPtpDomain(uint8_t domainNumber);
    
    /**
     * Get the NMOS API port number.
     * @return The port number the NMOS HTTP server is listening on, or 0 if not running
     */
    uint16_t getNmosPort() const { return nmosPort_; }

    /**
     * Get a short NMOS status string (registry connection/registration state).
     */
    std::string getNmosStatusText() const;

    /**
     * Get detailed NMOS diagnostics (mode, registry info, IDs).
     */
    std::string getNmosDiagnostics() const;

    /**
     * Best-effort registry base URL (scheme://host:port) for IS-04 Query API calls.
     * - If NMOS_REGISTRY_ADDRESS was configured, returns that.
     * - Otherwise uses discovered registry info when available.
     */
    std::string getRegistryBaseUrl() const;

    /**
     * Get sender diagnostics (active sender IDs, destinations, audio format, etc.).
     */
    std::string getSenderDiagnostics() const;
    
    struct SenderInfo
    {
        rav::Id id;
        std::string sessionName;
        std::string destAddress;
        uint16_t destPort{0};
        int logicalInputChannel{0};
        std::string nmosSenderId;
        std::string nmosFlowId;
    };

    /**
     * List active senders with basic info for UI/connection logic.
     */
    std::vector<SenderInfo> listSenders() const;
    
    /**
     * Check if PTP is stuck and needs retry.
     * Call this periodically (e.g., from a timer) to auto-recover from
     * the "first launch after build" issue on macOS.
     * @return true if a retry was attempted
     */
    bool checkAndRetryPtpIfStuck();

private:
    // Persisted NMOS node identity helpers
    boost::uuids::uuid loadOrCreateNodeId();
    void persistNodeId(const boost::uuids::uuid& id);
    juce::File getNodeIdFile() const;
    std::optional<std::pair<std::string, std::string>> resolveLocalNmosIdsForLabel(const std::string& label) const;

    // Receives RavennaNode callbacks (NMOS status/config, etc.) on the maintenance thread
    class NodeSubscriber final : public rav::RavennaNode::Subscriber
    {
    public:
        explicit NodeSubscriber(RavennaNodeManager& owner) : owner_(owner) {}

        void nmos_node_config_updated(const rav::nmos::Node::Configuration& config) override;
        void nmos_node_status_changed(const rav::nmos::Node::Status status, const rav::nmos::Node::StatusInfo& registry_info) override;

    private:
        RavennaNodeManager& owner_;
    };

    struct SenderInstance
    {
        struct RuntimeStats
        {
            std::atomic<uint64_t> sentPackets{0};
            std::atomic<uint64_t> sentFrames{0};
            std::atomic<uint32_t> lastRtpTimestamp{0};
            std::atomic<uint32_t> fifoOverflows{0};
            std::atomic<uint32_t> asrcUnderflows{0};
            std::atomic<int32_t> lastFifoError{0};
            std::atomic<double> ratioPpm{0.0};
            std::atomic<double> ratioPpmMin{0.0};
            std::atomic<double> ratioPpmMax{0.0};
            std::atomic<uint32_t> fifoOnly{0};
            std::atomic<uint32_t> asrcOnly{0};
        };

        rav::Id id;
        rav::RavennaSender::Configuration config;
        bool enabled{false};
        int logicalInputChannel{0};      // index within active input channels
        std::string nmosSenderId;
        std::string nmosFlowId;
        std::shared_ptr<RuntimeStats> stats{std::make_shared<RuntimeStats>()};

        // Per-sender drift-compensated streaming pipeline
        struct Pipeline
        {
            Pipeline() = default;
            Pipeline(const Pipeline&) = delete;
            Pipeline& operator=(const Pipeline&) = delete;

            Pipeline(Pipeline&& other) noexcept { *this = std::move(other); }

            Pipeline& operator=(Pipeline&& other) noexcept
            {
                if (this == &other) return *this;

                rtpTimestamp = other.rtpTimestamp;
                timestampInitialized = other.timestampInitialized;

                fifo = std::move(other.fifo);
                fifoMask = other.fifoMask;
                fifoWrite.store(other.fifoWrite.load(std::memory_order_relaxed), std::memory_order_relaxed);
                fifoRead.store(other.fifoRead.load(std::memory_order_relaxed), std::memory_order_relaxed);
                fifoOverflows.store(other.fifoOverflows.load(std::memory_order_relaxed), std::memory_order_relaxed);

                asrcRing = std::move(other.asrcRing);
                asrcMask = other.asrcMask;
                asrcWrite = other.asrcWrite;
                asrcRead = other.asrcRead;
                asrcFrac = other.asrcFrac;
                asrcTmp = std::move(other.asrcTmp);

                ratio = other.ratio;
                ratioSmoothed = other.ratioSmoothed;
                ratioIntegral = other.ratioIntegral;

                lastSample = other.lastSample;
                return *this;
            }

            // RTP timestamp tracking for paced send thread
            uint32_t rtpTimestamp{0};
            bool timestampInitialized{false};

            // SPSC ring buffer (audio thread producer, send thread consumer)
            std::vector<float> fifo;
            size_t fifoMask{0};
            std::atomic<uint64_t> fifoWrite{0};
            std::atomic<uint64_t> fifoRead{0};
            std::atomic<uint32_t> fifoOverflows{0};

            // ASRC ring buffer
            std::vector<float> asrcRing;
            size_t asrcMask{0};
            uint64_t asrcWrite{0};
            uint64_t asrcRead{0};
            double asrcFrac{0.0};
            std::vector<float> asrcTmp;

            // PI controller state
            double ratio{1.0};
            double ratioSmoothed{1.0};
            double ratioIntegral{0.0};

            float lastSample{0.0f};
        } pipeline;

        SenderInstance() = default;
        SenderInstance(const SenderInstance&) = delete;
        SenderInstance& operator=(const SenderInstance&) = delete;
        SenderInstance(SenderInstance&&) noexcept = default;
        SenderInstance& operator=(SenderInstance&&) noexcept = default;
    };
    
    std::unique_ptr<rav::RavennaNode> node_;
    std::unique_ptr<NodeSubscriber> nodeSubscriber_;
    std::vector<SenderInstance> activeSenders_; // Dynamically created senders
    std::atomic<bool> isActive_;                // True if at least one sender is active
    std::string currentInterface_;
    uint16_t nmosPort_{0};
    
    // PTP subscriber to monitor synchronization
    class PtpSubscriber : public rav::ptp::Instance::Subscriber
    {
    public:
        bool isSynchronized() 
        { 
            const auto& clock = get_local_clock();
            return clock.is_calibrated();
        }
        
        void ptp_parent_changed(const rav::ptp::ParentDs& parent) override
        {
            lastParent_ = parent;
            hasParent_ = true;
            
            // #region agent log
            std::ofstream logFile("/Users/nicolasdesilles/Desktop/TestAES67Sender/.cursor/debug.log", std::ios::app);
            if (logFile.is_open()) {
                std::stringstream data;
                data << "{\"grandmasterId\":\"" << parent.grandmaster_identity.to_string() << "\",\"priority1\":" << static_cast<int>(parent.grandmaster_priority1) << ",\"priority2\":" << static_cast<int>(parent.grandmaster_priority2) << "}";
                logFile << "{\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"A\",\"location\":\"RavennaNodeManager.cpp:ptp_parent_changed\",\"message\":\"PTP parent changed - grandmaster detected\",\"data\":" << data.str() << ",\"timestamp\":" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() << "}\n";
                logFile.close();
            }
            // #endregion
        }
        
        void ptp_port_changed_state(const rav::ptp::Port& port) override
        {
            auto oldState = lastPortState_;
            lastPortState_ = port.state();
            
            // #region agent log
            std::ofstream logFile("/Users/nicolasdesilles/Desktop/TestAES67Sender/.cursor/debug.log", std::ios::app);
            if (logFile.is_open()) {
                std::stringstream data;
                data << "{\"oldState\":\"" << rav::ptp::to_string(oldState) << "\",\"newState\":\"" << rav::ptp::to_string(lastPortState_) << "\",\"portId\":\"" << port.get_port_identity().clock_identity.to_string() << "\"}";
                logFile << "{\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"A\",\"location\":\"RavennaNodeManager.cpp:ptp_port_changed_state\",\"message\":\"PTP port state changed\",\"data\":" << data.str() << ",\"timestamp\":" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() << "}\n";
                logFile.close();
            }
            // #endregion
        }
        
        bool hasParent() const { return hasParent_; }
        const rav::ptp::ParentDs& getParent() const { return lastParent_; }
        rav::ptp::State getPortState() const { return lastPortState_; }
        
        void ptp_stats_updated(const rav::ptp::Stats& ptp_stats) override
        {
            lastStats_ = ptp_stats;
            
            // #region agent log
            std::ofstream logFile("/Users/nicolasdesilles/Desktop/TestAES67Sender/.cursor/debug.log", std::ios::app);
            if (logFile.is_open()) {
                std::stringstream data;
                data << "{\"ignoredOutliers\":" << ptp_stats.ignored_outliers << ",\"offsetFromMasterMean\":" << ptp_stats.offset_from_master.mean() << ",\"filteredOffsetMean\":" << ptp_stats.filtered_offset.mean() << "}";
                logFile << "{\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"E\",\"location\":\"RavennaNodeManager.cpp:ptp_stats_updated\",\"message\":\"PTP stats updated - checking clock offset\",\"data\":" << data.str() << ",\"timestamp\":" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() << "}\n";
                logFile.close();
            }
            // #endregion
        }
        
    private:
        rav::ptp::ParentDs lastParent_;
        rav::ptp::State lastPortState_{rav::ptp::State::initializing};
        std::atomic<bool> hasParent_{false};
        rav::ptp::Stats lastStats_;
    };
    
    std::unique_ptr<PtpSubscriber> ptpSubscriber_;
    
    // Sender-side buffering / pacing
    // We enqueue audio from the JUCE callback into each sender FIFO and send fixed 1ms packets from a dedicated thread.
    // Drift is handled per-sender with a PI-controlled ASRC.
    std::thread sendThread_;
    std::atomic<bool> sendThreadRunning_{false};

    // Continuous ASRC (best quality): PI-controlled resampling ratio + polyphase windowed-sinc
    static constexpr uint32_t kSincPhases = 1024;  // phase table resolution
    static constexpr uint32_t kSincTaps = 64;      // filter taps (must be even)
    std::vector<float> sincTable_;                 // [kSincPhases * kSincTaps]

    
    // NMOS status (updated from maintenance thread, read from UI thread)
    mutable std::mutex nmosMutex_;
    rav::nmos::Node::Configuration nmosConfigSnapshot_{};
    rav::nmos::Node::Status nmosStatus_{rav::nmos::Node::Status::disabled};
    rav::nmos::Node::StatusInfo nmosRegistryInfo_{};
    boost::uuids::uuid nmosNodeId_{};
    boost::uuids::uuid nmosDeviceId_{};
    std::string configuredRegistryAddress_;
    
    // PTP retry logic
    std::chrono::steady_clock::time_point ptpInitTime_;
    int ptpRetryCount_{0};
    
    // Audio format: 48kHz, 24-bit, mono
    static constexpr uint32_t kSampleRate = 48000;
    static constexpr rav::AudioEncoding kEncoding = rav::AudioEncoding::pcm_s24;
    static constexpr uint32_t kNumChannels = 1; // Mono

    // Sender pacing config
    static constexpr uint32_t kFramesPerPacket = 48;      // 1ms at 48kHz (matches PacketTime::ms_1())
    static constexpr uint32_t kSenderLatencyMs = 10;      // sender-side buffering target
    static constexpr uint32_t kTargetFifoLevel = kFramesPerPacket * kSenderLatencyMs; // in samples/frames
    static constexpr uint32_t kFifoCapacityFrames = 1u << 16; // 65536 frames (~1.36s) power-of-two
    static constexpr uint32_t kAsrcRingFrames = 1u << 15; // 32768 frames power-of-two (~0.68s)

    void startSendThread();
    void stopSendThread();
    void sendThreadMain();

    void buildSincTable();
    void resetPipeline(SenderInstance::Pipeline& p);
    uint32_t fifoLevel(const SenderInstance::Pipeline& p) const;
    uint32_t fifoWriteSamples(SenderInstance::Pipeline& p, const float* data, uint32_t n);
    uint32_t fifoReadSamples(SenderInstance::Pipeline& p, float* dst, uint32_t n);
    uint32_t asrcBufferedFrames(const SenderInstance::Pipeline& p) const;
    uint32_t asrcFillFromFifo(SenderInstance::Pipeline& p, uint32_t desiredBufferedFrames, uint32_t maxFramesPerCall);
    float asrcGetSample(const SenderInstance::Pipeline& p, uint64_t idx) const;
    float asrcResampleOne(SenderInstance::Pipeline& p, double ratio);
    
    // Helper methods
    boost::asio::ip::address_v4 generateMulticastAddress(size_t senderIndex);
    rav::RavennaSender::Configuration createSenderConfig(size_t senderIndex, bool enabled);

    void setNmosConfigSnapshot(const rav::nmos::Node::Configuration& config);
    void setNmosStatusSnapshot(rav::nmos::Node::Status status, const rav::nmos::Node::StatusInfo& info);
    static const char* nmosStatusToString(rav::nmos::Node::Status status);
};

} // namespace AudioApp

