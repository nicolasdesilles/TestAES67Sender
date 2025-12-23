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
    , inputChannelLabel_("Meter Channel:", "Meter Channel:")
    , ptpStatusLabel_("PTP Status:", "PTP Status:")
    , ptpStatusValue_("ptpStatus", "Not Synchronized")
    , ptpDiagnosticsLabel_("PTP Diagnostics:", "PTP Diagnostics:")
    , ptpDiagnosticsText_()
    , nmosStatusLabel_("NMOS Status:", "NMOS Status:")
    , nmosStatusValue_("nmosStatus", "Unknown")
    , nmosDiagnosticsLabel_("NMOS Diagnostics:", "NMOS Diagnostics:")
    , nmosDiagnosticsText_()
    , senderStatusLabel_("Sender Status:", "Sender Status:")
    , senderStatusValue_("senderStatus", "Stopped")
    , senderDiagnosticsLabel_("Sender Diagnostics:", "Sender Diagnostics:")
    , senderDiagnosticsText_()
{
    setAudioChannels(64, 64);
    
    inputMonitor_ = std::make_unique<InputMonitor>(*this);
    deviceManager.addAudioCallback(inputMonitor_.get());

    addAndMakeVisible(tabs_);

    // Audio tab
    networkInterfaceLabel_.attachToComponent(&networkInterfaceCombo_, true);
    networkInterfaceCombo_.onChange = [this] { onNetworkInterfaceChanged(); };
    audioTab_.addAndMakeVisible(networkInterfaceCombo_);

    inputChannelLabel_.attachToComponent(&inputChannelCombo_, true);
    inputChannelCombo_.onChange = [this] { onInputChannelChanged(); };
    audioTab_.addAndMakeVisible(inputChannelCombo_);

    inputLevelMeter_.startTimer(50);
    audioTab_.addAndMakeVisible(inputLevelMeter_);

    inputLevelValueLabel_.setText("-inf dB", juce::dontSendNotification);
    inputLevelValueLabel_.setJustificationType(juce::Justification::centredLeft);
    inputLevelValueLabel_.setColour(juce::Label::textColourId, juce::Colours::white);
    inputLevelValueLabel_.setFont(juce::Font(14.0f, juce::Font::plain));
    audioTab_.addAndMakeVisible(inputLevelValueLabel_);

    audioTab_.addAndMakeVisible(selector);

    // Logs tab
    ptpStatusLabel_.attachToComponent(&ptpStatusValue_, true);
    ptpStatusValue_.setColour(juce::Label::textColourId, juce::Colours::red);
    logsTab_.addAndMakeVisible(ptpStatusValue_);

    ptpDiagnosticsLabel_.attachToComponent(&ptpDiagnosticsText_, true);
    ptpDiagnosticsText_.setMultiLine(true);
    ptpDiagnosticsText_.setReadOnly(true);
    ptpDiagnosticsText_.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 10.0f, 0));
    logsTab_.addAndMakeVisible(ptpDiagnosticsText_);

    nmosStatusLabel_.attachToComponent(&nmosStatusValue_, true);
    nmosStatusValue_.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    logsTab_.addAndMakeVisible(nmosStatusValue_);

    nmosDiagnosticsLabel_.attachToComponent(&nmosDiagnosticsText_, true);
    nmosDiagnosticsText_.setMultiLine(true);
    nmosDiagnosticsText_.setReadOnly(true);
    nmosDiagnosticsText_.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 10.0f, 0));
    logsTab_.addAndMakeVisible(nmosDiagnosticsText_);

    senderStatusLabel_.attachToComponent(&senderStatusValue_, true);
    senderStatusValue_.setColour(juce::Label::textColourId, juce::Colours::orange);
    logsTab_.addAndMakeVisible(senderStatusValue_);

    senderDiagnosticsLabel_.attachToComponent(&senderDiagnosticsText_, true);
    senderDiagnosticsText_.setMultiLine(true);
    senderDiagnosticsText_.setReadOnly(true);
    senderDiagnosticsText_.setScrollbarsShown(true);
    senderDiagnosticsText_.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 10.0f, 0));
    logsTab_.addAndMakeVisible(senderDiagnosticsText_);

    // Senders tab
    senderSelector_.onChange = [this] { onSenderSelectionChanged(); };
    sendersTab_.addAndMakeVisible(senderSelector_);

    addSenderButton_.onClick = [this] { onAddSender(); };
    sendersTab_.addAndMakeVisible(addSenderButton_);
    removeSenderButton_.onClick = [this] { onRemoveSender(); };
    sendersTab_.addAndMakeVisible(removeSenderButton_);

    senderInputChannelLabel_.attachToComponent(&senderInputChannelCombo_, true);
    senderInputChannelCombo_.onChange = [this] { onSenderInputChannelChanged(); };
    sendersTab_.addAndMakeVisible(senderInputChannelCombo_);

    receiverSelectLabel_.attachToComponent(&receiverCombo_, true);
    sendersTab_.addAndMakeVisible(receiverCombo_);

    connectButton_.onClick = [this] { onConnectReceiver(); };
    sendersTab_.addAndMakeVisible(connectButton_);
    disconnectButton_.onClick = [this] { onDisconnectReceiver(); };
    sendersTab_.addAndMakeVisible(disconnectButton_);

    // Receivers tab
    receiversListBox_.setMultiLine(true);
    receiversListBox_.setReadOnly(true);
    receiversTab_.addAndMakeVisible(receiversListBox_);

    tabs_.addTab("Audio", juce::Colours::darkgrey, &audioTab_, false);
    tabs_.addTab("Logs", juce::Colours::darkgrey, &logsTab_, false);
    tabs_.addTab("Senders", juce::Colours::darkgrey, &sendersTab_, false);
    tabs_.addTab("Devices", juce::Colours::darkgrey, &receiversTab_, false);
    
    // Initialize RAVENNA (but don't start yet)
    ravennaInitialized_ = ravennaManager_.initialize();
    if (ravennaInitialized_)
    {
        updateNetworkInterfaces();
    }
    
    // Start timer to update status displays
    startTimer(500); // Update every 500ms
    
    setSize(1200, 900);
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

    int actualChannelIndex = selectedMeterPhysicalChannel_;
    if (actualChannelIndex < 0 || actualChannelIndex >= numInputChannels || !activeInputChannels[actualChannelIndex])
        actualChannelIndex = -1;

    static int logCounter = 0;
    ++logCounter;
    if (logCounter % 100 == 0)
    {
        std::ofstream logFile("/Users/nicolasdesilles/Desktop/TestAES67Sender/.cursor/debug.log", std::ios::app);
        if (logFile.is_open())
        {
            logFile << "{\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"H6\",\"location\":\"MainComponent.cpp:155\",\"message\":\"Active input channels check\",\"data\":{\"numActiveInputs\":" << activeInputChannels.countNumberOfSetBits() << ",\"activeChannels\":\"" << activeInputChannels.toString(2).toStdString() << "\",\"selectedChannel\":" << selectedInputChannel_ << ",\"meterPhysicalChannel\":" << selectedMeterPhysicalChannel_ << ",\"actualChannelIndex\":" << actualChannelIndex << "},\"timestamp\":" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() << "}\n";
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
        ravennaManager_.processAudio(inputChannelData, numInputChannels, numSamples, activeInputChannels);
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
    auto bounds = getLocalBounds().reduced(8);
    tabs_.setBounds(bounds);

    const int margin = 10;
    const int labelWidth = 150;
    const int controlHeight = 28;

    // Audio tab layout
    {
        auto b = audioTab_.getLocalBounds().reduced(10);
        auto selectorBounds = b.removeFromTop(320);
        selector.setBounds(selectorBounds);
        b.removeFromTop(margin);

        auto net = b.removeFromTop(controlHeight);
        networkInterfaceCombo_.setBounds(net.removeFromLeft(350).withTrimmedLeft(labelWidth));
        b.removeFromTop(margin);

        auto channel = b.removeFromTop(controlHeight);
        inputChannelCombo_.setBounds(channel.removeFromLeft(300).withTrimmedLeft(labelWidth));
        auto meter = channel.removeFromLeft(220).reduced(5);
        inputLevelMeter_.setBounds(meter);
        auto val = channel.removeFromLeft(140).reduced(5);
        inputLevelValueLabel_.setBounds(val);
    }

    // Logs tab layout
    {
        auto b = logsTab_.getLocalBounds().reduced(10);
        auto ptp = b.removeFromTop(controlHeight);
        ptpStatusValue_.setBounds(ptp.removeFromLeft(300).withTrimmedLeft(labelWidth));
        b.removeFromTop(margin);

        auto ptpDiag = b.removeFromTop(140);
        ptpDiagnosticsText_.setBounds(ptpDiag.removeFromLeft(b.getWidth() - labelWidth).withTrimmedLeft(labelWidth));
        b.removeFromTop(margin);

        auto nmos = b.removeFromTop(controlHeight);
        nmosStatusValue_.setBounds(nmos.removeFromLeft(300).withTrimmedLeft(labelWidth));
        b.removeFromTop(margin);

        auto nmosDiag = b.removeFromTop(140);
        nmosDiagnosticsText_.setBounds(nmosDiag.removeFromLeft(b.getWidth() - labelWidth).withTrimmedLeft(labelWidth));
        b.removeFromTop(margin);

        auto sender = b.removeFromTop(controlHeight);
        senderStatusValue_.setBounds(sender.removeFromLeft(300).withTrimmedLeft(labelWidth));
        b.removeFromTop(margin);

        // Let sender diagnostics take remaining space (more senders => more lines)
        senderDiagnosticsText_.setBounds(b.withTrimmedLeft(labelWidth));
    }

    // Senders tab layout
    {
        auto b = sendersTab_.getLocalBounds().reduced(10);
        auto topRow = b.removeFromTop(controlHeight);
        senderSelector_.setBounds(topRow.removeFromLeft(320));
        addSenderButton_.setBounds(topRow.removeFromLeft(140).reduced(5, 0));
        removeSenderButton_.setBounds(topRow.removeFromLeft(140).reduced(5, 0));
        b.removeFromTop(margin);

        auto inputRow = b.removeFromTop(controlHeight);
        senderInputChannelCombo_.setBounds(inputRow.removeFromLeft(250).withTrimmedLeft(labelWidth));
        b.removeFromTop(margin);

        auto receiverRow = b.removeFromTop(controlHeight);
        receiverCombo_.setBounds(receiverRow.removeFromLeft(350).withTrimmedLeft(labelWidth));
        connectButton_.setBounds(receiverRow.removeFromLeft(180).reduced(5, 0));
        disconnectButton_.setBounds(receiverRow.removeFromLeft(160).reduced(5, 0));
    }

    // Receivers tab layout
    {
        receiversListBox_.setBounds(receiversTab_.getLocalBounds().reduced(10));
    }
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
    meterPhysicalChannelMap_.clear();
    
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device != nullptr)
    {
        juce::BigInteger activeInputs = device->getActiveInputChannels();
        const int numActiveInputs = activeInputs.countNumberOfSetBits();
        const int numPhysicalInputs = device->getInputChannelNames().size();
        
        // #region agent log
        {
            std::ofstream logFile("/Users/nicolasdesilles/Desktop/TestAES67Sender/.cursor/debug.log", std::ios::app);
            if (logFile.is_open()) {
                logFile << "{\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"H5\",\"location\":\"MainComponent.cpp:235\",\"message\":\"updateInputChannels\",\"data\":{\"deviceName\":\"" << device->getName().toStdString() << "\",\"numActiveInputChannels\":" << numActiveInputs << ",\"activeInputChannels\":\"" << activeInputs.toString(2).toStdString() << "\",\"totalInputChannels\":" << device->getInputChannelNames().size() << "},\"timestamp\":" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() << "}\n";
            }
        }
        // #endregion
        
        int logicalIndex = 0;
        for (int ch = 0; ch < numPhysicalInputs; ++ch)
        {
            if (!activeInputs[ch])
                continue;
            const auto name = device->getInputChannelNames()[ch];
            inputChannelCombo_.addItem("In " + juce::String(ch + 1) + " - " + name, logicalIndex + 1);
            meterPhysicalChannelMap_.push_back(ch);
            ++logicalIndex;
        }
        
        if (numActiveInputs > 0)
        {
            // Select first channel by default, or keep current selection if valid
            if (selectedInputChannel_ >= numActiveInputs)
            {
                selectedInputChannel_ = 0;
            }
            inputChannelCombo_.setSelectedId(selectedInputChannel_ + 1);
            if (selectedInputChannel_ >= 0 && selectedInputChannel_ < static_cast<int>(meterPhysicalChannelMap_.size()))
                selectedMeterPhysicalChannel_ = meterPhysicalChannelMap_[static_cast<size_t>(selectedInputChannel_)];
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

void MainComponent::updateNmosStatus()
{
    if (!ravennaInitialized_)
    {
        nmosStatusValue_.setText("Not Initialized", juce::dontSendNotification);
        nmosStatusValue_.setColour(juce::Label::textColourId, juce::Colours::red);
        nmosDiagnosticsText_.setText("RAVENNA not initialized", juce::dontSendNotification);
        return;
    }

    const auto statusText = ravennaManager_.getNmosStatusText();
    nmosStatusValue_.setText(juce::String(statusText), juce::dontSendNotification);
    nmosDiagnosticsText_.setText(juce::String(ravennaManager_.getNmosDiagnostics()), juce::dontSendNotification);

    // Colour hint
    if (statusText.find("registered") != std::string::npos)
        nmosStatusValue_.setColour(juce::Label::textColourId, juce::Colours::green);
    else if (statusText.find("p2p") != std::string::npos)
        nmosStatusValue_.setColour(juce::Label::textColourId, juce::Colours::orange);
    else if (statusText.find("error") != std::string::npos)
        nmosStatusValue_.setColour(juce::Label::textColourId, juce::Colours::red);
    else
        nmosStatusValue_.setColour(juce::Label::textColourId, juce::Colours::lightblue);
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
        if (selectedInputChannel_ >= 0 && selectedInputChannel_ < static_cast<int>(meterPhysicalChannelMap_.size()))
            selectedMeterPhysicalChannel_ = meterPhysicalChannelMap_[static_cast<size_t>(selectedInputChannel_)];
    }
}

void MainComponent::timerCallback()
{
    updatePtpStatus();
    updateNmosStatus();
    refreshSendersUi();
    refreshReceivers();
    
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
    
    // Update sender diagnostics, but don't destroy user scroll position while they interact with the view.
    if (!senderDiagnosticsText_.hasKeyboardFocus(true) && !senderDiagnosticsText_.isMouseOverOrDragging())
    {
        const auto newText = juce::String(ravennaManager_.getSenderDiagnostics());
        if (senderDiagnosticsText_.getText() != newText)
            senderDiagnosticsText_.setText(newText, juce::dontSendNotification);
    }
}

void MainComponent::refreshSendersUi()
{
    senderInfos_ = ravennaManager_.listSenders();

    const int selectedId = senderSelector_.getSelectedId();
    senderSelector_.clear(juce::dontSendNotification);
    int idx = 1;
    for (const auto& s : senderInfos_)
    {
        juce::String label = juce::String(s.sessionName.empty() ? "Sender" : s.sessionName) + " (" + juce::String(s.id.to_string()) + ")";
        senderSelector_.addItem(label, idx++);
    }
    if (selectedId > 0 && selectedId <= senderSelector_.getNumItems())
        senderSelector_.setSelectedId(selectedId, juce::dontSendNotification);
    else if (senderSelector_.getNumItems() > 0)
        senderSelector_.setSelectedId(1, juce::dontSendNotification);

    onSenderSelectionChanged();
}

void MainComponent::refreshReceivers()
{
    // Periodic background refresh (avoid blocking message thread)
    const auto now = std::chrono::steady_clock::now();
    if (receiversFetchInFlight_.load(std::memory_order_acquire))
        return;
    if (lastReceiversFetch_ != std::chrono::steady_clock::time_point{} &&
        now - lastReceiversFetch_ < std::chrono::seconds(2))
        return;

    receiversFetchInFlight_.store(true, std::memory_order_release);
    lastReceiversFetch_ = now;

    std::thread([this] {
        auto baseUrl = juce::String(ravennaManager_.getRegistryBaseUrl());
        if (baseUrl.isEmpty())
        {
            if (const char* env = std::getenv("NMOS_REGISTRY_ADDRESS"); env && std::strlen(env) > 0)
                baseUrl = env;
            else
                baseUrl = "http://127.0.0.1:8010";
        }

        auto fetchJson = [](const juce::String& fullUrl) -> juce::var {
            juce::URL url(fullUrl);
            auto stream = url.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                                                    .withConnectionTimeoutMs(1200)
                                                    .withNumRedirectsToFollow(1));
            if (!stream)
                return {};
            return juce::JSON::parse(stream->readEntireStreamAsString());
        };

        // Request a high paging limit to avoid missing href/node mappings on larger registries.
        const auto receiversJson = fetchJson(baseUrl + "/x-nmos/query/v1.3/receivers?paging.limit=1000");
        const auto devicesJson = fetchJson(baseUrl + "/x-nmos/query/v1.3/devices?paging.limit=1000");
        const auto nodesJson = fetchJson(baseUrl + "/x-nmos/query/v1.3/nodes?paging.limit=1000");
        if (!receiversJson.isArray() || !devicesJson.isArray() || !nodesJson.isArray())
        {
            receiversFetchInFlight_.store(false, std::memory_order_release);
            return;
        }

        std::unordered_map<std::string, std::string> nodeIdToHref;
        for (const auto& item : *nodesJson.getArray())
        {
            auto* obj = item.getDynamicObject();
            if (!obj)
                continue;
            const auto id = obj->getProperty("id").toString().toStdString();
            auto href = obj->getProperty("href").toString().toStdString();
            if (!id.empty() && !href.empty())
            {
                // Normalize:
                // - remove trailing slash
                // - if registry returns href including /x-nmos/node/<ver>, strip it back to base URL
                while (!href.empty() && href.back() == '/')
                    href.pop_back();
                const auto pos = href.find("/x-nmos/node/");
                if (pos != std::string::npos)
                    href = href.substr(0, pos);
                nodeIdToHref[id] = href;
            }
        }

        std::unordered_map<std::string, std::string> deviceIdToNodeId;

        std::vector<DeviceInfo> devicesList;
        for (const auto& item : *devicesJson.getArray())
        {
            auto* obj = item.getDynamicObject();
            if (!obj)
                continue;
            DeviceInfo info;
            info.id = obj->getProperty("id").toString().toStdString();
            info.label = obj->getProperty("label").toString().toStdString();
            info.type = obj->getProperty("type").toString().toStdString();
            info.nodeId = obj->getProperty("node_id").toString().toStdString();
            auto it = nodeIdToHref.find(info.nodeId);
            if (it != nodeIdToHref.end())
                info.href = it->second;
            if (!info.id.empty() && !info.nodeId.empty())
                deviceIdToNodeId[info.id] = info.nodeId;
            devicesList.push_back(std::move(info));
        }

        std::vector<ReceiverInfo> receiversList;
        for (const auto& item : *receiversJson.getArray())
        {
            auto* obj = item.getDynamicObject();
            if (!obj)
                continue;
            ReceiverInfo info;
            info.id = obj->getProperty("id").toString().toStdString();
            info.label = obj->getProperty("label").toString().toStdString();
            info.transport = obj->getProperty("transport").toString().toStdString();
            // IS-04 receiver resources always have device_id; node_id may not be present in query results.
            const auto deviceId = obj->getProperty("device_id").toString().toStdString();
            auto nodeIdIt = deviceIdToNodeId.find(deviceId);
            if (nodeIdIt != deviceIdToNodeId.end())
                info.nodeId = nodeIdIt->second;

            auto it = nodeIdToHref.find(info.nodeId);
            if (it != nodeIdToHref.end())
            {
                info.href = it->second;
                // node href is like http://host:port, Connection API base sits on the node
                info.connectionBase = info.href + "/x-nmos/connection/v1.1";
            }
            receiversList.push_back(std::move(info));
        }

        juce::MessageManager::callAsync([this, movedReceivers = std::move(receiversList), movedDevices = std::move(devicesList)]() mutable {
            receivers_ = std::move(movedReceivers);
            devices_ = std::move(movedDevices);

            const auto selected = receiverCombo_.getSelectedId();
            receiverCombo_.clear(juce::dontSendNotification);
            int idx = 1;
            for (const auto& r : receivers_)
            {
                juce::String label = juce::String(r.label.empty() ? r.id : r.label);
                if (r.connectionBase.empty())
                    label += " (no href)";
                receiverCombo_.addItem(label, idx++);
            }
            if (selected > 0 && selected <= receiverCombo_.getNumItems())
                receiverCombo_.setSelectedId(selected, juce::dontSendNotification);
            else if (receiverCombo_.getNumItems() > 0)
                receiverCombo_.setSelectedId(1, juce::dontSendNotification);

            juce::String summary;
            summary << "Registry: " << juce::String(ravennaManager_.getRegistryBaseUrl()) << "\n";
            summary << "Devices: " << devices_.size() << "\n";
            summary << "Receivers (for connection dropdown): " << receivers_.size() << "\n\n";

            for (const auto& d : devices_)
            {
                summary << juce::String(d.id) << " - " << juce::String(d.label) << "\n";
                summary << "  type=" << juce::String(d.type) << "\n";
                summary << "  node_id=" << juce::String(d.nodeId) << "\n";
                summary << "  href=" << juce::String(d.href) << "\n\n";
            }
            receiversListBox_.setText(summary, juce::dontSendNotification);

            receiversFetchInFlight_.store(false, std::memory_order_release);
        });
    }).detach();
}

void MainComponent::onAddSender()
{
    const auto name = "Sender" + std::to_string(senderInfos_.size() + 1);
    auto id = ravennaManager_.createSender(name, 0);
    if (id.is_valid())
    {
        senderStatusValue_.setText("Sender Created", juce::dontSendNotification);
        senderStatusValue_.setColour(juce::Label::textColourId, juce::Colours::green);
    }
    else
    {
        senderStatusValue_.setText("Failed to create sender", juce::dontSendNotification);
        senderStatusValue_.setColour(juce::Label::textColourId, juce::Colours::red);
    }
    refreshSendersUi();
}

void MainComponent::onRemoveSender()
{
    const auto idx = senderSelector_.getSelectedId();
    if (idx <= 0 || idx > static_cast<int>(senderInfos_.size()))
        return;
    const auto id = senderInfos_[static_cast<size_t>(idx - 1)].id;
    ravennaManager_.removeSender(id);
    refreshSendersUi();
}

void MainComponent::onSenderSelectionChanged()
{
    const auto idx = senderSelector_.getSelectedId();
    senderInputChannelCombo_.clear(juce::dontSendNotification);
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device != nullptr)
    {
        juce::BigInteger activeInputs = device->getActiveInputChannels();
        const int numInputChannels = activeInputs.countNumberOfSetBits();
        for (int i = 0; i < numInputChannels; ++i)
            senderInputChannelCombo_.addItem("Channel " + juce::String(i + 1), i + 1);
    }
    if (idx > 0 && idx <= static_cast<int>(senderInfos_.size()))
    {
        int logical = senderInfos_[static_cast<size_t>(idx - 1)].logicalInputChannel;
        senderInputChannelCombo_.setSelectedId(logical + 1, juce::dontSendNotification);
    }
}

void MainComponent::onSenderInputChannelChanged()
{
    const auto idx = senderSelector_.getSelectedId();
    if (idx <= 0 || idx > static_cast<int>(senderInfos_.size()))
        return;
    const auto id = senderInfos_[static_cast<size_t>(idx - 1)].id;
    const int logical = senderInputChannelCombo_.getSelectedId() - 1;
    ravennaManager_.setSenderInputChannel(id, logical);
}

static std::unique_ptr<juce::WebInputStream> sendJsonRequest(const juce::String& urlString,
                                                             const juce::String& method,
                                                             const juce::String& body)
{
    juce::URL url(urlString);
    // WebInputStream uses URL's internal POST data, even for custom methods like PATCH.
    url = url.withPOSTData(body);

    auto stream = std::make_unique<juce::WebInputStream>(url, true);
    stream->withCustomRequestCommand(method)
        .withExtraHeaders("Content-Type: application/json\r\n")
        .withConnectionTimeout(1000)
        .withNumRedirectsToFollow(1);

    if (!stream->connect(nullptr))
        return nullptr;

    const int status = stream->getStatusCode();
    if (status >= 400 || status == 0)
        return nullptr;

    return stream;
}

void MainComponent::onConnectReceiver()
{
    const auto sIdx = senderSelector_.getSelectedId();
    const auto rIdx = receiverCombo_.getSelectedId();
    if (sIdx <= 0 || rIdx <= 0)
        return;

    if (sIdx > static_cast<int>(senderInfos_.size()) || rIdx > static_cast<int>(receivers_.size()))
        return;

    const auto sender = senderInfos_[static_cast<size_t>(sIdx - 1)];
    const auto receiver = receivers_[static_cast<size_t>(rIdx - 1)];

    if (sender.nmosSenderId.empty())
    {
        senderStatusValue_.setText("Sender NMOS ID missing", juce::dontSendNotification);
        senderStatusValue_.setColour(juce::Label::textColourId, juce::Colours::red);
        return;
    }

    if (receiver.connectionBase.empty())
    {
        senderStatusValue_.setText("Receiver has no connection API href", juce::dontSendNotification);
        senderStatusValue_.setColour(juce::Label::textColourId, juce::Colours::red);
        return;
    }

    senderStatusValue_.setText("Connecting...", juce::dontSendNotification);
    senderStatusValue_.setColour(juce::Label::textColourId, juce::Colours::orange);

    juce::Component::SafePointer<MainComponent> safeThis(this);
    std::thread([safeThis, sender, receiver] {
        bool ok = false;
        juce::String error;
#if JUCE_MAC
        juce::ScopedAutoReleasePool pool;
#endif
        try
        {
            auto staged = new juce::DynamicObject();
            staged->setProperty("sender_id", juce::String(sender.nmosSenderId));
            staged->setProperty("master_enable", true);

            juce::Array<juce::var> tpArray;
            juce::DynamicObject* tp = new juce::DynamicObject();
            tp->setProperty("destination_ip", juce::String(sender.destAddress));
            tp->setProperty("destination_port", static_cast<int>(sender.destPort));
            tp->setProperty("rtp_enabled", true);
            tpArray.add(juce::var(tp));
            staged->setProperty("transport_params", tpArray);

            juce::String stagedBody = juce::JSON::toString(juce::var(staged));
            auto stagedUrl = juce::String(receiver.connectionBase) + "/single/receivers/" + receiver.id + "/staged";
            auto stream = sendJsonRequest(stagedUrl, "PATCH", stagedBody);
            if (!stream)
                throw std::runtime_error("PATCH staged failed");
            const int stagedStatus = stream->getStatusCode();
            if (stagedStatus >= 400 || stagedStatus == 0)
                throw std::runtime_error(("PATCH staged HTTP " + std::to_string(stagedStatus)).c_str());

            auto activateObj = new juce::DynamicObject();
            activateObj->setProperty("mode", "activate_immediate");
            juce::String activateBody = juce::JSON::toString(juce::var(activateObj));
            auto activateUrl = juce::String(receiver.connectionBase) + "/single/receivers/" + receiver.id + "/staged/activation";
            auto actStream = sendJsonRequest(activateUrl, "POST", activateBody);
            if (!actStream)
                throw std::runtime_error("activate failed");
            const int actStatus = actStream->getStatusCode();
            if (actStatus >= 400 || actStatus == 0)
                throw std::runtime_error(("activate HTTP " + std::to_string(actStatus)).c_str());

            ok = true;
        }
        catch (const std::exception& e)
        {
            error = e.what();
        }
        catch (...)
        {
            error = "unknown error";
        }

        juce::MessageManager::callAsync([safeThis, ok, error] {
            if (!safeThis)
                return;
            safeThis->senderStatusValue_.setText(ok ? "Receiver connected" : ("Connect failed: " + error), juce::dontSendNotification);
            safeThis->senderStatusValue_.setColour(juce::Label::textColourId, ok ? juce::Colours::green : juce::Colours::red);
        });
    }).detach();
    return;
}

void MainComponent::onDisconnectReceiver()
{
    const auto rIdx = receiverCombo_.getSelectedId();
    if (rIdx <= 0)
        return;
    if (rIdx > static_cast<int>(receivers_.size()))
        return;
    const auto receiver = receivers_[static_cast<size_t>(rIdx - 1)];

    if (receiver.connectionBase.empty())
        return;

    senderStatusValue_.setText("Disconnecting...", juce::dontSendNotification);
    senderStatusValue_.setColour(juce::Label::textColourId, juce::Colours::orange);

    juce::Component::SafePointer<MainComponent> safeThis(this);
    std::thread([safeThis, receiver] {
        bool ok = false;
        juce::String error;
#if JUCE_MAC
        juce::ScopedAutoReleasePool pool;
#endif
        try
        {
            auto staged = new juce::DynamicObject();
            staged->setProperty("sender_id", juce::String());
            staged->setProperty("master_enable", false);
            juce::Array<juce::var> tpArray;
            staged->setProperty("transport_params", tpArray);
            juce::String stagedBody = juce::JSON::toString(juce::var(staged));
            auto stagedUrl = juce::String(receiver.connectionBase) + "/single/receivers/" + receiver.id + "/staged";
            auto stream = sendJsonRequest(stagedUrl, "PATCH", stagedBody);
            if (!stream)
                throw std::runtime_error("PATCH staged failed");
            const int stagedStatus = stream->getStatusCode();
            if (stagedStatus >= 400 || stagedStatus == 0)
                throw std::runtime_error(("PATCH staged HTTP " + std::to_string(stagedStatus)).c_str());

            auto activateObj = new juce::DynamicObject();
            activateObj->setProperty("mode", "activate_immediate");
            juce::String activateBody = juce::JSON::toString(juce::var(activateObj));
            auto activateUrl = juce::String(receiver.connectionBase) + "/single/receivers/" + receiver.id + "/staged/activation";
            auto act = sendJsonRequest(activateUrl, "POST", activateBody);
            if (!act)
                throw std::runtime_error("activate failed");
            const int actStatus = act->getStatusCode();
            if (actStatus >= 400 || actStatus == 0)
                throw std::runtime_error(("activate HTTP " + std::to_string(actStatus)).c_str());

            ok = true;
        }
        catch (const std::exception& e)
        {
            error = e.what();
        }
        catch (...)
        {
            error = "unknown error";
        }

        juce::MessageManager::callAsync([safeThis, ok, error] {
            if (!safeThis)
                return;
            safeThis->senderStatusValue_.setText(ok ? "Receiver disconnected" : ("Disconnect failed: " + error), juce::dontSendNotification);
            safeThis->senderStatusValue_.setColour(juce::Label::textColourId, ok ? juce::Colours::orange : juce::Colours::red);
        });
    }).detach();
    return;
}

} // namespace AudioApp
