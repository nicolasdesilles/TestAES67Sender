#pragma once

#include "CommonHeader.h"
#include "RavennaNodeManager.h"

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
    void onNetworkInterfaceChanged();
    void onInputChannelChanged();
    void onStartStopClicked();
    void timerCallback() override;

    juce::AudioDeviceSelectorComponent selector {
        deviceManager, 2, 64, 2, 64, false, false, true, false};
    
    // RAVENNA components
    RavennaNodeManager ravennaManager_;
    bool ravennaInitialized_{false};
    
    // UI Components
    juce::Label networkInterfaceLabel_;
    juce::ComboBox networkInterfaceCombo_;
    juce::Label inputChannelLabel_;
    juce::ComboBox inputChannelCombo_;
    juce::Label ptpStatusLabel_;
    juce::Label ptpStatusValue_;
    juce::Label ptpDiagnosticsLabel_;
    juce::TextEditor ptpDiagnosticsText_;
    juce::Label senderStatusLabel_;
    juce::Label senderStatusValue_;
    juce::TextButton startStopButton_;
    
    // State
    int selectedInputChannel_{0};
    double currentSampleRate_{48000.0};
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace AudioApp
