#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "AhpLookAndFeel.h"
#include "AudioDefaults.h"

//==============================================================================
/** The device chooser, with the number that actually decides whether you can
    play through it printed underneath.

    Monitoring latency is the one setting a player feels in their hands rather
    than hears, and the device panel on its own does not say what the current
    choice costs: it shows a buffer size in samples, which means nothing until
    it is divided by the sample rate and added to the driver's own buffering
    on both sides. Someone tracking guitar through a 1024 sample buffer has no
    way of knowing from this window that they have chosen about fifty
    milliseconds, which is the difference between playing and fighting.

    So the round trip is shown in milliseconds, and where it comes from is
    broken out, because the fix depends on which part is large. A big buffer
    is one dropdown away. A driver that cannot go below forty milliseconds at
    any buffer size is not, and the answer there is a different driver.

    The arithmetic and the thresholds come from AudioDefaults rather than from
    here, because the same numbers decide what the studio picks on first run
    and they have tests on them there. Two copies would drift, and the window
    disagreeing with the engine about whether a setting is playable is a
    worse bug than either being wrong.
*/
class AudioSettingsPanel : public juce::Component,
                           private juce::Timer
{
public:
    explicit AudioSettingsPanel (juce::AudioDeviceManager& manager)
        : devices (manager),
          selector (manager, 0, 8, 0, 8, true, false, true, false)
    {
        addAndMakeVisible (selector);
        addAndMakeVisible (readout);

        readout.setJustificationType (juce::Justification::topLeft);
        readout.setFont (juce::FontOptions (12.0f));

        refresh();
        startTimerHz (4);          // the device can change under this window
        setSize (560, 560);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        readout.setBounds (area.removeFromBottom (78).reduced (10, 6));
        selector.setBounds (area);
    }

private:
    void timerCallback() override { refresh(); }

    void refresh()
    {
        auto* device = devices.getCurrentAudioDevice();

        if (device == nullptr)
        {
            readout.setColour (juce::Label::textColourId, Ahp::muted);
            readout.setText ("No audio device open.", juce::dontSendNotification);
            return;
        }

        const double rate = device->getCurrentSampleRate();

        if (rate <= 0.0)
            return;

        // What the driver reports, not what the buffer size implies. The two
        // differ: a driver adds its own buffering either side of ours, and on
        // some of them by a lot more than the buffer accounts for.
        const int inSamples  = device->getInputLatencyInSamples();
        const int outSamples = device->getOutputLatencyInSamples();
        const double roundTripMs = AudioDefaults::roundTripMs (inSamples, outSamples, rate);

        const juce::String driver = devices.getCurrentAudioDeviceType();

        juce::String text;
        text << juce::String (roundTripMs, 1) << " ms round trip for monitoring"
             << "   (in " << juce::String (1000.0 * inSamples / rate, 1)
             << " ms, out " << juce::String (1000.0 * outSamples / rate, 1)
             << " ms, buffer " << device->getCurrentBufferSizeSamples()
             << " at " << juce::String (rate / 1000.0, 1) << " kHz)";

        if (AudioDefaults::playable (roundTripMs))
        {
            readout.setColour (juce::Label::textColourId, Ahp::bone);
            text << "\nLow enough to play through.";
        }
        else if (! AudioDefaults::tooSlowToPlay (roundTripMs))
        {
            readout.setColour (juce::Label::textColourId, Ahp::bone);
            text << "\nPlayable, though you may feel it on fast parts. A smaller buffer will cut it further.";
        }
        else
        {
            readout.setColour (juce::Label::textColourId, Ahp::rec);
            text << "\nToo high to play through comfortably. Try a smaller buffer size first.";

           #if JUCE_WINDOWS
            // On Windows the driver matters more than the buffer. Plain
            // Windows Audio is the shared WASAPI mode, and it buffers heavily
            // no matter what buffer size is asked for, so no setting in this
            // window recovers it. Low Latency Mode is the same shared device
            // negotiated through IAudioClient3 instead: single digit
            // milliseconds, and other applications keep making sound.
            // Exclusive Mode is as fast but takes the device, which is why it
            // is suggested second.
            if (! driver.containsIgnoreCase ("ASIO")
                && ! driver.containsIgnoreCase ("Low Latency")
                && ! driver.containsIgnoreCase ("Exclusive"))
                text << "\nIf that is not enough, change Audio device type above to "
                        "Windows Audio (Low Latency Mode), or to ASIO if your interface provides it. "
                        "\"" << driver << "\" shares the device with everything else on the system and buffers for it.";
           #else
            juce::ignoreUnused (driver);
           #endif
        }

        readout.setText (text, juce::dontSendNotification);
    }

    juce::AudioDeviceManager& devices;
    juce::AudioDeviceSelectorComponent selector;
    juce::Label readout;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioSettingsPanel)
};
