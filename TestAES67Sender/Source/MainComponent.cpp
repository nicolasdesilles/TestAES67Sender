#include "MainComponent.h"
#include <juce_gui_basics/juce_gui_basics.h>

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
    , startStopButton_("Start Sending")
{
    setAudioChannels(64, 64);
    
    // Setup network interface combo
    networkInterfaceLabel_.attachToComponent(&networkInterfaceCombo_, true);
    networkInterfaceCombo_.onChange = [this] { onNetworkInterfaceChanged(); };
    addAndMakeVisible(networkInterfaceCombo_);
    
    // Setup input channel combo
    inputChannelLabel_.attachToComponent(&inputChannelCombo_, true);
    inputChannelCombo_.onChange = [this] { onInputChannelChanged(); };
    addAndMakeVisible(inputChannelCombo_);
    
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
    if (ravennaInitialized_)
    {
        ravennaManager_.stop();
    }
}

void MainComponent::prepareToPlay(int /*samplesPerBlockExpected*/, double sampleRate)
{
    currentSampleRate_ = sampleRate;
    
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
    auto* inputBuffer = bufferToFill.buffer;
    
    // Send to RAVENNA if active and we have a valid input channel
    if (ravennaInitialized_ && ravennaManager_.isActive() && inputBuffer != nullptr)
    {
        const int numInputChannels = inputBuffer->getNumChannels();
        if (selectedInputChannel_ >= 0 && selectedInputChannel_ < numInputChannels)
        {
            const float* channelData = inputBuffer->getReadPointer(selectedInputChannel_);
            const int numSamples = bufferToFill.numSamples;
            
            ravennaManager_.sendAudio(channelData, numSamples);
        }
    }
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
    
    // Audio device selector at the top
    auto selectorBounds = bounds.removeFromTop(300);
    selector.setBounds(selectorBounds);
    
    bounds.removeFromTop(margin);
    
    // Network interface selection
    auto networkBounds = bounds.removeFromTop(controlHeight);
    networkInterfaceCombo_.setBounds(networkBounds.removeFromLeft(300).withTrimmedLeft(labelWidth));
    bounds.removeFromTop(margin);
    
    // Input channel selection
    auto channelBounds = bounds.removeFromTop(controlHeight);
    inputChannelCombo_.setBounds(channelBounds.removeFromLeft(300).withTrimmedLeft(labelWidth));
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
        const int numInputChannels = device->getActiveInputChannels().countNumberOfSetBits();
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
}

} // namespace AudioApp
