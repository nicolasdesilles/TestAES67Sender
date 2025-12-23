#pragma once

#include "CommonHeader.h"
#include "RavennaNodeManager.h"
#include <atomic>
#include <memory>
#include <chrono>

namespace AudioApp
{
class MainComponent : public juce::AudioAppComponent, public juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;

    void paint(Graphics&) override;
    void resized() override;

private:
    void updateNetworkInterfaces();
    void updateInputChannels();
    void updatePtpStatus();
    void updateNmosStatus();
    void refreshReceivers();
    void refreshSendersUi();
    void onNetworkInterfaceChanged();
    void onInputChannelChanged();
    void onAddSender();
    void onRemoveSender();
    void onSenderSelectionChanged();
    void onSenderInputChannelChanged();
    void onConnectReceiver();
    void onDisconnectReceiver();
    void timerCallback() override;

    juce::AudioDeviceSelectorComponent selector {
        deviceManager, 2, 64, 2, 64, false, false, true, false};
    
    // RAVENNA components
    RavennaNodeManager ravennaManager_;
    bool ravennaInitialized_{false};
    
    // Tabs
    juce::TabbedComponent tabs_{juce::TabbedButtonBar::TabsAtTop};

    // Audio tab
    juce::Component audioTab_;
    juce::Label networkInterfaceLabel_;
    juce::ComboBox networkInterfaceCombo_;
    juce::Label inputChannelLabel_;
    juce::ComboBox inputChannelCombo_;
    juce::Label inputLevelValueLabel_;

    // Logs tab
    juce::Component logsTab_;
    juce::Label ptpStatusLabel_;
    juce::Label ptpStatusValue_;
    juce::Label ptpDiagnosticsLabel_;
    juce::TextEditor ptpDiagnosticsText_;
    juce::Label nmosStatusLabel_;
    juce::Label nmosStatusValue_;
    juce::Label nmosDiagnosticsLabel_;
    juce::TextEditor nmosDiagnosticsText_;
    juce::Label senderStatusLabel_;
    juce::Label senderStatusValue_;
    juce::Label senderDiagnosticsLabel_;
    juce::TextEditor senderDiagnosticsText_;

    // Senders tab
    juce::Component sendersTab_;
    juce::ComboBox senderSelector_;
    juce::TextButton addSenderButton_{"Add Sender"};
    juce::TextButton removeSenderButton_{"Remove Sender"};
    juce::Label senderInputChannelLabel_{"Sender Input", "Input Channel:"};
    juce::ComboBox senderInputChannelCombo_;
    juce::TextButton connectButton_{"Connect Receiver"};
    juce::TextButton disconnectButton_{"Disconnect"};
    juce::Label receiverSelectLabel_{"Receiver", "Receiver:"};
    juce::ComboBox receiverCombo_;

    // Receivers tab
    juce::Component receiversTab_;
    juce::TextEditor receiversListBox_;
    
    // Signal meter component
    class LevelMeter : public juce::Component, public juce::Timer
    {
    public:
        LevelMeter() : level_(0.0f), peakLevel_(0.0f) {}
        ~LevelMeter() override { stopTimer(); }
        
        void setLevel(float level)
        {
            level_ = juce::jlimit(0.0f, 1.0f, level);
            if (level_ > peakLevel_)
                peakLevel_ = level_;
            repaint();
        }
        
        void paint(juce::Graphics& g) override
        {
            auto bounds = getLocalBounds().toFloat();
            const float cornerRadius = 2.0f;
            
            // Background
            g.setColour(juce::Colours::darkgrey);
            g.fillRoundedRectangle(bounds, cornerRadius);
            
            // Level bar
            if (level_ > 0.0f)
            {
                auto levelBounds = bounds.withWidth(bounds.getWidth() * level_);
                
                // Color gradient: green -> yellow -> red
                if (level_ < 0.7f)
                {
                    g.setColour(juce::Colours::green);
                }
                else if (level_ < 0.9f)
                {
                    g.setColour(juce::Colours::yellow);
                }
                else
                {
                    g.setColour(juce::Colours::red);
                }
                
                g.fillRoundedRectangle(levelBounds, cornerRadius);
            }
            
            // Peak indicator
            if (peakLevel_ > 0.0f)
            {
                const float peakX = bounds.getWidth() * peakLevel_;
                g.setColour(juce::Colours::white);
                g.drawLine(peakX, 0, peakX, bounds.getHeight(), 1.0f);
            }
        }
        
        void timerCallback() override
        {
            // Decay peak level
            peakLevel_ *= 0.95f;
            if (peakLevel_ < 0.01f)
                peakLevel_ = 0.0f;
            repaint();
        }
        
    private:
        float level_;
        float peakLevel_;
    };
    
    LevelMeter inputLevelMeter_;
    
    std::atomic<float> currentInputLevelDb_{-100.0f};

    void handleIncomingAudio(const float* const* inputChannelData, int numInputChannels, int numSamples);
    void handleInputDeviceAboutToStart(juce::AudioIODevice* device);
    void handleInputDeviceStopped();

    struct InputMonitor final : public juce::AudioIODeviceCallback
    {
        explicit InputMonitor(MainComponent& owner) : owner_(owner) {}
        void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                              int numInputChannels,
                                              float* const* outputChannelData,
                                              int numOutputChannels,
                                              int numSamples,
                                              const juce::AudioIODeviceCallbackContext& context) override;
        void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
        void audioDeviceStopped() override;

    private:
        MainComponent& owner_;
    };

    std::unique_ptr<InputMonitor> inputMonitor_;

    // State
    int selectedInputChannel_{0};
    int selectedMeterPhysicalChannel_{0};
    std::vector<int> meterPhysicalChannelMap_;
    double currentSampleRate_{48000.0};
    std::atomic<float> currentInputLevel_{0.0f};
    std::vector<RavennaNodeManager::SenderInfo> senderInfos_;
    struct DeviceInfo
    {
        std::string id;
        std::string label;
        std::string type;
        std::string nodeId;
        std::string href;
    };
    std::vector<DeviceInfo> devices_;
    struct ReceiverInfo
    {
        std::string id;
        std::string label;
        std::string nodeId;
        std::string href;
        std::string connectionBase;
        std::string transport;
    };
    std::vector<ReceiverInfo> receivers_;

    std::atomic<bool> receiversFetchInFlight_{false};
    std::chrono::steady_clock::time_point lastReceiversFetch_{};
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace AudioApp
