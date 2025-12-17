#include "MainComponent.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>

namespace AudioApp
{
MainComponent::MainComponent()
    : networkInterfaceLabel_("Network Interface:", "Network Interface:")
    , inputChannelLabel_("Input Channel:", "Input Channel:")
    , ptpStatusLabel_("PTP Status:", "PTP Status:")
    , ptpStatusValue_("ptpStatus", "Not Synchronized")
    , ptpDiagnosticsLabel_("PTP Diagnostics:", "PTP Diagnostics:")
    , ptpDiagnosticsText_()
    , senderStatusLabel_("Sender Status:", "Sender Status:")
    , senderStatusValue_("senderStatus", "Stopped")
    , senderDiagnosticsLabel_("Sender Diagnostics:", "Sender Diagnostics:")
    , senderDiagnosticsText_()
    , startStopButton_("Start Sending")
{
    setAudioChannels(64, 64);
    
    inputMonitor_ = std::make_unique<InputMonitor>(*this);
    deviceManager.addAudioCallback(inputMonitor_.get());
    
    // Setup network interface combo
    networkInterfaceLabel_.attachToComponent(&networkInterfaceCombo_, true);
    networkInterfaceCombo_.onChange = [this] { onNetworkInterfaceChanged(); };
    addAndMakeVisible(networkInterfaceCombo_);
    
    // Setup input channel combo
    inputChannelLabel_.attachToComponent(&inputChannelCombo_, true);
    inputChannelCombo_.onChange = [this] { onInputChannelChanged(); };
    addAndMakeVisible(inputChannelCombo_);
    
    // Setup input level meter
    inputLevelMeter_.startTimer(50); // Update peak decay every 50ms
    addAndMakeVisible(inputLevelMeter_);
    
    inputLevelValueLabel_.setText("-inf dB", juce::dontSendNotification);
    inputLevelValueLabel_.setJustificationType(juce::Justification::centredLeft);
    inputLevelValueLabel_.setColour(juce::Label::textColourId, juce::Colours::white);
    inputLevelValueLabel_.setFont(juce::Font(14.0f, juce::Font::plain));
    addAndMakeVisible(inputLevelValueLabel_);
    
    // Setup PTP status
    ptpStatusLabel_.attachToComponent(&ptpStatusValue_, true);
    ptpStatusValue_.setColour(juce::Label::textColourId, juce::Colours::red);
    addAndMakeVisible(ptpStatusValue_);
    
    // Setup PTP diagnostics
    ptpDiagnosticsLabel_.attachToComponent(&ptpDiagnosticsText_, true);
    ptpDiagnosticsText_.setMultiLine(true);
    ptpDiagnosticsText_.setReadOnly(true);
    ptpDiagnosticsText_.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 10.0f, 0));
    addAndMakeVisible(ptpDiagnosticsText_);
    
    // Setup sender status
    senderStatusLabel_.attachToComponent(&senderStatusValue_, true);
    senderStatusValue_.setColour(juce::Label::textColourId, juce::Colours::orange);
    addAndMakeVisible(senderStatusValue_);

    // Setup sender diagnostics
    senderDiagnosticsLabel_.attachToComponent(&senderDiagnosticsText_, true);
    senderDiagnosticsText_.setMultiLine(true);
    senderDiagnosticsText_.setReadOnly(true);
    senderDiagnosticsText_.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 10.0f, 0));
    addAndMakeVisible(senderDiagnosticsText_);
    
    // Setup start/stop button
    startStopButton_.onClick = [this] { onStartStopClicked(); };
    addAndMakeVisible(startStopButton_);
    
    // Add audio device selector
    addAndMakeVisible(selector);
    
    // Initialize RAVENNA (but don't start yet)
    ravennaInitialized_ = ravennaManager_.initialize();
    if (ravennaInitialized_)
    {
        updateNetworkInterfaces();
    }
    
    // Start timer to update status displays
    startTimer(500); // Update every 500ms
    
    setSize(900, 1000);
}

MainComponent::~MainComponent()
{
    stopTimer();
    if (inputMonitor_ != nullptr)
    {
        deviceManager.removeAudioCallback(inputMonitor_.get());
    }
    if (ravennaInitialized_)
    {
        ravennaManager_.stop();
    }
}

void MainComponent::prepareToPlay(int /*samplesPerBlockExpected*/, double sampleRate)
{
    currentSampleRate_ = sampleRate;
    
    // #region agent log
    {
        std::ofstream logFile("/Users/nicolasdesilles/Desktop/TestAES67Sender/.cursor/debug.log", std::ios::app);
        if (logFile.is_open()) {
            auto* device = deviceManager.getCurrentAudioDevice();
            juce::BigInteger activeInputs = device ? device->getActiveInputChannels() : juce::BigInteger();
            int numActiveInputs = activeInputs.countNumberOfSetBits();
            logFile << "{\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"H5\",\"location\":\"MainComponent.cpp:80\",\"message\":\"prepareToPlay called\",\"data\":{\"sampleRate\":" << sampleRate << ",\"deviceName\":\"" << (device ? device->getName().toStdString() : "null") << "\",\"numActiveInputChannels\":" << numActiveInputs << ",\"activeInputChannels\":\"" << activeInputs.toString(2).toStdString() << "\"},\"timestamp\":" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() << "}\n";
        }
    }
    // #endregion
    
    // Check if sample rate matches 48kHz (required for RAVENNA)
    if (std::abs(sampleRate - 48000.0) > 1.0)
    {
        // Update status to show warning
        senderStatusValue_.setText("Sample Rate Error: " + juce::String(sampleRate) + " Hz (need 48kHz)", juce::dontSendNotification);
        senderStatusValue_.setColour(juce::Label::textColourId, juce::Colours::red);
    }
    
    // Update input channels list
    updateInputChannels();
    
    // If RAVENNA is already started, we might need to restart it
    // (In a production app, you'd handle sample rate changes more gracefully)
}

void MainComponent::releaseResources()
{
    // Stop RAVENNA sending when audio stops
    if (ravennaInitialized_ && ravennaManager_.isActive())
    {
        ravennaManager_.stop();
        senderStatusValue_.setText("Stopped", juce::dontSendNotification);
        senderStatusValue_.setColour(juce::Label::textColourId, juce::Colours::orange);
        startStopButton_.setButtonText("Start Sending");
    }
}

void MainComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    bufferToFill.clearActiveBufferRegion();
}

void MainComponent::handleIncomingAudio(const float* const* inputChannelData, int numInputChannels, int numSamples)
{
    if (inputChannelData == nullptr || numInputChannels <= 0 || numSamples <= 0)
    {
        currentInputLevel_.store(0.0f);
        currentInputLevelDb_.store(-100.0f);
        return;
    }

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
    {
        currentInputLevel_.store(0.0f);
        currentInputLevelDb_.store(-100.0f);
        return;
    }

    juce::BigInteger activeInputChannels = device->getActiveInputChannels();

    int actualChannelIndex = -1;
    int activeChannelCount = 0;
    for (int channel = 0; channel < numInputChannels; ++channel)
    {
        if (activeInputChannels[channel])
        {
            if (activeChannelCount == selectedInputChannel_)
            {
                actualChannelIndex = channel;
                break;
            }
            ++activeChannelCount;
        }
    }

    static int logCounter = 0;
    ++logCounter;
    if (logCounter % 100 == 0)
    {
        std::ofstream logFile("/Users/nicolasdesilles/Desktop/TestAES67Sender/.cursor/debug.log", std::ios::app);
        if (logFile.is_open())
        {
            logFile << "{\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"H6\",\"location\":\"MainComponent.cpp:155\",\"message\":\"Active input channels check\",\"data\":{\"numActiveInputs\":" << activeInputChannels.countNumberOfSetBits() << ",\"activeChannels\":\"" << activeInputChannels.toString(2).toStdString() << "\",\"selectedChannel\":" << selectedInputChannel_ << ",\"actualChannelIndex\":" << actualChannelIndex << "},\"timestamp\":" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() << "}\n";
        }
    }

    if (actualChannelIndex < 0 || actualChannelIndex >= numInputChannels)
    {
        if (logCounter % 100 == 0)
        {
            std::ofstream logFile("/Users/nicolasdesilles/Desktop/TestAES67Sender/.cursor/debug.log", std::ios::app);
            if (logFile.is_open())
            {
                logFile << "{\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"H6\",\"location\":\"MainComponent.cpp:205\",\"message\":\"Invalid mapped channel index\",\"data\":{\"selectedChannel\":" << selectedInputChannel_ << ",\"actualChannelIndex\":" << actualChannelIndex << "},\"timestamp\":" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() << "}\n";
            }
        }

        currentInputLevel_.store(0.0f);
        currentInputLevelDb_.store(-100.0f);
        return;
    }

    const float* channelData = inputChannelData[actualChannelIndex];
    if (channelData == nullptr)
    {
        currentInputLevel_.store(0.0f);
        currentInputLevelDb_.store(-100.0f);
        return;
    }

    float sumSquared = 0.0f;
    float maxSample = 0.0f;
    float minSample = 0.0f;
    int nonZeroSamples = 0;
    const float firstSample = channelData[0];
    for (int i = 0; i < numSamples; ++i)
    {
        const float sample = channelData[i];
        sumSquared += sample * sample;
        if (std::abs(sample) > 0.0001f)
            ++nonZeroSamples;
        maxSample = juce::jmax(maxSample, sample);
        minSample = juce::jmin(minSample, sample);
    }

    const float rms = std::sqrt(sumSquared / static_cast<float>(juce::jmax(1, numSamples)));
    const float normalizedLevel = juce::jlimit(0.0f, 1.0f, rms * 1.414f);
    const float levelDb = (normalizedLevel > 0.0f) ? 20.0f * std::log10(normalizedLevel) : -100.0f;

    currentInputLevel_.store(normalizedLevel);
    currentInputLevelDb_.store(levelDb);

    if (logCounter % 100 == 0)
    {
        std::ofstream logFile("/Users/nicolasdesilles/Desktop/TestAES67Sender/.cursor/debug.log", std::ios::app);
        if (logFile.is_open())
        {
            logFile << "{\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"H4\",\"location\":\"MainComponent.cpp:185\",\"message\":\"Audio samples analysis (input monitor)\",\"data\":{\"rms\":" << rms << ",\"maxSample\":" << maxSample << ",\"minSample\":" << minSample << ",\"nonZeroSamples\":" << nonZeroSamples << ",\"totalSamples\":" << numSamples << ",\"firstSample\":" << firstSample << ",\"actualChannelIndex\":" << actualChannelIndex << ",\"levelDb\":" << levelDb << "},\"timestamp\":" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() << "}\n";
        }
    }

    if (ravennaInitialized_ && ravennaManager_.isActive())
    {
        ravennaManager_.sendAudio(channelData, numSamples);
    }
}

void MainComponent::handleInputDeviceAboutToStart(juce::AudioIODevice* device)
{
    {
        std::ofstream logFile("/Users/nicolasdesilles/Desktop/TestAES67Sender/.cursor/debug.log", std::ios::app);
        if (logFile.is_open())
        {
            logFile << "{\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"H7\",\"location\":\"MainComponent.cpp:360\",\"message\":\"audioDeviceAboutToStart\",\"data\":{\"deviceName\":\"" << (device ? device->getName().toStdString() : "null") << "\"},\"timestamp\":" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() << "}\n";
        }
    }

    juce::MessageManager::callAsync([this] { updateInputChannels(); });
}

void MainComponent::handleInputDeviceStopped()
{
    {
        std::ofstream logFile("/Users/nicolasdesilles/Desktop/TestAES67Sender/.cursor/debug.log", std::ios::app);
        if (logFile.is_open())
        {
            logFile << "{\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"H7\",\"location\":\"MainComponent.cpp:375\",\"message\":\"audioDeviceStopped\",\"data\":{},\"timestamp\":" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() << "}\n";
        }
    }

    currentInputLevel_.store(0.0f);
    currentInputLevelDb_.store(-100.0f);
}

void MainComponent::InputMonitor::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                                   int numInputChannels,
                                                                   float* const* outputChannelData,
                                                                   int numOutputChannels,
                                                                   int numSamples,
                                                                   const juce::AudioIODeviceCallbackContext& context)
{
    juce::ignoreUnused(outputChannelData, numOutputChannels, context);
    owner_.handleIncomingAudio(inputChannelData, numInputChannels, numSamples);
}

void MainComponent::InputMonitor::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    owner_.handleInputDeviceAboutToStart(device);
}

void MainComponent::InputMonitor::audioDeviceStopped()
{
    owner_.handleInputDeviceStopped();
}


void MainComponent::paint(Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void MainComponent::resized()
{
    auto bounds = getLocalBounds();
    const int margin = 10;
    const int labelWidth = 150;
    const int controlHeight = 30;
    
    // Audio device selector at the top (reduced height to make room for other controls)
    auto selectorBounds = bounds.removeFromTop(400);
    selector.setBounds(selectorBounds);
    
    bounds.removeFromTop(margin);
    
    // Network interface selection
    auto networkBounds = bounds.removeFromTop(controlHeight);
    networkInterfaceCombo_.setBounds(networkBounds.removeFromLeft(300).withTrimmedLeft(labelWidth));
    bounds.removeFromTop(margin);
    
    // Input channel selection with level meter
    auto channelBounds = bounds.removeFromTop(controlHeight);
    auto comboBounds = channelBounds.removeFromLeft(300).withTrimmedLeft(labelWidth);
    inputChannelCombo_.setBounds(comboBounds);
    
    // Place level meter next to the combo box
    auto meterBounds = channelBounds.removeFromLeft(200).reduced(5, 5);
    inputLevelMeter_.setBounds(meterBounds);
    
    auto valueBounds = channelBounds.removeFromLeft(120).reduced(5, 2);
    inputLevelValueLabel_.setBounds(valueBounds);
    bounds.removeFromTop(margin);
    
    // PTP status
    auto ptpBounds = bounds.removeFromTop(controlHeight);
    ptpStatusValue_.setBounds(ptpBounds.removeFromLeft(300).withTrimmedLeft(labelWidth));
    bounds.removeFromTop(margin);
    
    // PTP diagnostics
    auto ptpDiagBounds = bounds.removeFromTop(120);
    ptpDiagnosticsText_.setBounds(ptpDiagBounds.removeFromLeft(bounds.getWidth() - labelWidth).withTrimmedLeft(labelWidth));
    bounds.removeFromTop(margin);
    
    // Sender status
    auto senderBounds = bounds.removeFromTop(controlHeight);
    senderStatusValue_.setBounds(senderBounds.removeFromLeft(300).withTrimmedLeft(labelWidth));
    bounds.removeFromTop(margin);

    // Sender diagnostics
    auto senderDiagBounds = bounds.removeFromTop(180);
    senderDiagnosticsText_.setBounds(senderDiagBounds.removeFromLeft(bounds.getWidth() - labelWidth).withTrimmedLeft(labelWidth));
    bounds.removeFromTop(margin);
    
    // Start/Stop button
    startStopButton_.setBounds(bounds.removeFromTop(controlHeight + 10).removeFromLeft(200));
}

void MainComponent::updateNetworkInterfaces()
{
    networkInterfaceCombo_.clear();
    
    auto interfaces = ravennaManager_.getAvailableInterfaces();
    for (size_t i = 0; i < interfaces.size(); ++i)
    {
        networkInterfaceCombo_.addItem(interfaces[i], static_cast<int>(i + 1));
    }
    
    // Select current interface if available
    auto currentInterface = ravennaManager_.getCurrentInterface();
    if (!currentInterface.empty())
    {
        for (int i = 0; i < networkInterfaceCombo_.getNumItems(); ++i)
        {
            if (networkInterfaceCombo_.getItemText(i) == juce::String(currentInterface))
            {
                networkInterfaceCombo_.setSelectedId(i + 1);
                break;
            }
        }
    }
    else if (networkInterfaceCombo_.getNumItems() > 0)
    {
        networkInterfaceCombo_.setSelectedId(1);
        onNetworkInterfaceChanged();
    }
}

void MainComponent::updateInputChannels()
{
    inputChannelCombo_.clear();
    
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device != nullptr)
    {
        juce::BigInteger activeInputs = device->getActiveInputChannels();
        const int numInputChannels = activeInputs.countNumberOfSetBits();
        
        // #region agent log
        {
            std::ofstream logFile("/Users/nicolasdesilles/Desktop/TestAES67Sender/.cursor/debug.log", std::ios::app);
            if (logFile.is_open()) {
                logFile << "{\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"H5\",\"location\":\"MainComponent.cpp:235\",\"message\":\"updateInputChannels\",\"data\":{\"deviceName\":\"" << device->getName().toStdString() << "\",\"numActiveInputChannels\":" << numInputChannels << ",\"activeInputChannels\":\"" << activeInputs.toString(2).toStdString() << "\",\"totalInputChannels\":" << device->getInputChannelNames().size() << "},\"timestamp\":" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() << "}\n";
            }
        }
        // #endregion
        
        for (int i = 0; i < numInputChannels; ++i)
        {
            inputChannelCombo_.addItem("Channel " + juce::String(i + 1), i + 1);
        }
        
        if (numInputChannels > 0)
        {
            // Select first channel by default, or keep current selection if valid
            if (selectedInputChannel_ >= numInputChannels)
            {
                selectedInputChannel_ = 0;
            }
            inputChannelCombo_.setSelectedId(selectedInputChannel_ + 1);
        }
    }
}

void MainComponent::updatePtpStatus()
{
    if (!ravennaInitialized_)
    {
        ptpStatusValue_.setText("Not Initialized", juce::dontSendNotification);
        ptpStatusValue_.setColour(juce::Label::textColourId, juce::Colours::red);
        ptpDiagnosticsText_.setText("RAVENNA not initialized", juce::dontSendNotification);
        return;
    }
    
    if (ravennaManager_.isPtpSynchronized())
    {
        ptpStatusValue_.setText("Synchronized", juce::dontSendNotification);
        ptpStatusValue_.setColour(juce::Label::textColourId, juce::Colours::green);
    }
    else
    {
        ptpStatusValue_.setText("Not Synchronized", juce::dontSendNotification);
        ptpStatusValue_.setColour(juce::Label::textColourId, juce::Colours::red);
    }
    
    // Update diagnostics
    auto diagnostics = ravennaManager_.getPtpDiagnostics();
    ptpDiagnosticsText_.setText(juce::String(diagnostics), juce::dontSendNotification);
}

void MainComponent::onNetworkInterfaceChanged()
{
    if (networkInterfaceCombo_.getSelectedId() > 0)
    {
        const int index = networkInterfaceCombo_.getSelectedId() - 1;
        auto interfaces = ravennaManager_.getAvailableInterfaces();
        if (index >= 0 && index < static_cast<int>(interfaces.size()))
        {
            ravennaManager_.setNetworkInterface(interfaces[static_cast<size_t>(index)]);
        }
    }
}

void MainComponent::onInputChannelChanged()
{
    if (inputChannelCombo_.getSelectedId() > 0)
    {
        selectedInputChannel_ = inputChannelCombo_.getSelectedId() - 1;
    }
}

void MainComponent::onStartStopClicked()
{
    if (!ravennaInitialized_)
    {
        senderStatusValue_.setText("RAVENNA Not Initialized", juce::dontSendNotification);
        senderStatusValue_.setColour(juce::Label::textColourId, juce::Colours::red);
        return;
    }
    
    if (ravennaManager_.isActive())
    {
        // Stop sending
        ravennaManager_.stop();
        senderStatusValue_.setText("Stopped", juce::dontSendNotification);
        senderStatusValue_.setColour(juce::Label::textColourId, juce::Colours::orange);
        startStopButton_.setButtonText("Start Sending");
    }
    else
    {
        // Check PTP synchronization (warn but allow starting)
        if (!ravennaManager_.isPtpSynchronized())
        {
            senderStatusValue_.setText("Starting (PTP not synced)", juce::dontSendNotification);
            senderStatusValue_.setColour(juce::Label::textColourId, juce::Colours::orange);
        }
        
        // Start sending
        if (ravennaManager_.start("TestAES67Sender"))
        {
            senderStatusValue_.setText("Active", juce::dontSendNotification);
            senderStatusValue_.setColour(juce::Label::textColourId, juce::Colours::green);
            startStopButton_.setButtonText("Stop Sending");
        }
        else
        {
            senderStatusValue_.setText("Failed to Start", juce::dontSendNotification);
            senderStatusValue_.setColour(juce::Label::textColourId, juce::Colours::red);
        }
    }
}

void MainComponent::timerCallback()
{
    updatePtpStatus();
    
    // Check if PTP is stuck and needs retry (helps with "first launch after build" issue on macOS)
    if (ravennaInitialized_)
    {
        ravennaManager_.checkAndRetryPtpIfStuck();
    }
    
    // Update input level meter
    inputLevelMeter_.setLevel(currentInputLevel_.load());
    const float levelDb = currentInputLevelDb_.load();
    const juce::String levelText = (levelDb <= -90.0f) ? "-inf dB" : juce::String(levelDb, 1) + " dB";
    inputLevelValueLabel_.setText(levelText, juce::dontSendNotification);
    
    // Update sender status
    if (ravennaManager_.isActive())
    {
        if (senderStatusValue_.getText() != "Active")
        {
            senderStatusValue_.setText("Active", juce::dontSendNotification);
            senderStatusValue_.setColour(juce::Label::textColourId, juce::Colours::green);
            startStopButton_.setButtonText("Stop Sending");
        }
    }
    else
    {
        if (senderStatusValue_.getText() != "Stopped")
        {
            senderStatusValue_.setText("Stopped", juce::dontSendNotification);
            senderStatusValue_.setColour(juce::Label::textColourId, juce::Colours::orange);
            startStopButton_.setButtonText("Start Sending");
        }
    }

    // Update sender diagnostics
    senderDiagnosticsText_.setText(juce::String(ravennaManager_.getSenderDiagnostics()), juce::dontSendNotification);
}

} // namespace AudioApp
