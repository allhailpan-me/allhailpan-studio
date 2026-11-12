#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "AhpLookAndFeel.h"
#include "AudioDefaults.h"
#include "LatencyCalibration.h"
#include "Preferences.h"

#include <cmath>
#include <functional>
#include <utility>

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

    The same two figures also decide where a recorded take lands, so the
    offset being applied to takes is printed here and the correction to it
    lives here too. Both belong next to the latency they are derived from: a
    guitarist who thinks their overdubs are landing late wants to see what the
    studio believes before they start nudging clips by hand, and a build that
    is quietly applying nothing looks exactly like one that is applying the
    wrong amount. Three days went into that distinction once already, over
    whether a Windows build had ASIO compiled in.
*/
class AudioSettingsPanel : public juce::Component,
                           private juce::Timer
{
public:
    /** The trim arrives as a number and leaves through a callback rather than
        this panel reaching into the settings file itself, because the file
        lives behind the preferences window that owns this panel and reading it
        from both would be two paths to one setting. */
    AudioSettingsPanel (juce::AudioDeviceManager& manager,
                        std::function<juce::String()> findFastest,
                        std::function<int()> recordOffset,
                        double initialTrimMs,
                        std::function<void (double)> setTrimMs)
        : devices (manager),
          selector (manager, 0, 8, 0, 8, true, false, true, false),
          findFastestDevice (std::move (findFastest)),
          recordOffsetSamples (std::move (recordOffset)),
          onTrimChanged (std::move (setTrimMs))
    {
        addAndMakeVisible (selector);
        addAndMakeVisible (readout);
        addAndMakeVisible (trimLabel);
        addAndMakeVisible (trimSlider);

        trimLabel.setJustificationType (juce::Justification::centredRight);
        trimLabel.setColour (juce::Label::textColourId, Ahp::muted);
        trimLabel.setFont (juce::FontOptions (12.0f));

        trimSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        // Continuous rather than stepped in tenths of a millisecond, which it
        // was until the measurement below existed. A tenth of a millisecond is
        // nearly five samples at 48 kHz and nineteen at 192, so a snap that
        // coarse would throw away most of what a measurement good to a
        // hundredth of a sample just bought. The measurement sets this slider
        // rather than writing to the settings behind it, so there is one path
        // from a correction to its effect whether it was measured or dialled,
        // and a snap on the way through would be a quiet loss on that path.
        trimSlider.setRange (Prefs::recordTrimMs.minimum, Prefs::recordTrimMs.maximum, 0.0);
        trimSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 76, 22);
        trimSlider.setDoubleClickReturnValue (true, Prefs::recordTrimMs.fallback);
        trimSlider.textFromValueFunction = [] (double ms)
        {
            // Two places, because a measured correction is not a round number
            // and showing it as one would make the studio look like it had
            // rounded the measurement rather than the display.
            return (ms > 0.0 ? juce::String ("+") : juce::String()) + juce::String (ms, 2) + " ms";
        };
        trimSlider.setTooltip ("Only for a driver that reports its latency wrongly. Positive pulls "
                               "recorded takes earlier.\n"
                               "Measure it rather than guessing: patch a cable from an output back "
                               "into an input and press Measure the round trip below.\n"
                               "Takes already recorded do not move, but nothing is lost either: "
                               "everything captured is in the file, so a take's left edge can "
                               "always be dragged back over it.");
        trimSlider.setValue (Prefs::number (initialTrimMs, Prefs::recordTrimMs),
                             juce::dontSendNotification);
        trimSlider.onValueChange = [this]
        {
            if (onTrimChanged != nullptr)
                onTrimChanged (trimSlider.getValue());

            refresh();
        };

        readout.setJustificationType (juce::Justification::topLeft);
        readout.setFont (juce::FontOptions (12.0f));

        // A TextEditor rather than a Label, because the report is as long as
        // the machine makes it and a Label silently squeezes or drops the
        // lines that do not fit. What it drops first is the end, which is the
        // list of what was tried, which is the whole reason this exists.
        addAndMakeVisible (outcome);
        outcome.setMultiLine (true, false);
        outcome.setReadOnly (true);
        outcome.setScrollbarsShown (true);
        outcome.setCaretVisible (false);
        outcome.setFont (juce::FontOptions (11.0f));
        outcome.setColour (juce::TextEditor::textColourId, Ahp::muted);
        outcome.setColour (juce::TextEditor::backgroundColourId, Ahp::panel);
        outcome.setColour (juce::TextEditor::outlineColourId, Ahp::line);
        outcome.setColour (juce::TextEditor::focusedOutlineColourId, Ahp::line);

        addAndMakeVisible (findButton);
        findButton.setTooltip ("Tries every driver this machine has, keeps the fastest one that "
                               "monitoring can be played through, and remembers it.\n"
                               "Drivers that share the sound card are tried first. One that takes "
                               "it is used only if nothing that shares it was fast enough, and the "
                               "studio says so when that happens.\n"
                               "Takes a few seconds, because changing driver closes the device "
                               "and waits for the system to let go of it.");
        findButton.setEnabled (findFastestDevice != nullptr);
        findButton.onClick = [this] { lookForSomethingFaster(); };

        addAndMakeVisible (loopLabel);
        loopLabel.setJustificationType (juce::Justification::centredRight);
        loopLabel.setColour (juce::Label::textColourId, Ahp::muted);
        loopLabel.setFont (juce::FontOptions (12.0f));

        addAndMakeVisible (outputBox);
        addAndMakeVisible (toLabel);
        addAndMakeVisible (inputBox);

        // Repointed while a measurement is running, the probe stays on the
        // channels it was started with and the boxes show the ones it is not
        // on, so the report then names a cable nobody patched. That is the
        // exact fault LatencyCalibration::activeChannels exists to prevent,
        // arrived at from the other side. Restarting is the honest answer: the
        // demodulator has to be reset for new channels anyway.
        outputBox.onChange = [this] { channelChoiceChanged(); };
        inputBox .onChange = [this] { channelChoiceChanged(); };

        toLabel.setJustificationType (juce::Justification::centred);
        toLabel.setColour (juce::Label::textColourId, Ahp::muted);
        toLabel.setFont (juce::FontOptions (12.0f));

        addAndMakeVisible (measureButton);
        measureButton.setTooltip ("Plays a signal out of the chosen output and listens for it on "
                                  "the chosen input, so patch a cable between the two first.\n"
                                  "It measures the whole way round through both converters and "
                                  "the driver, which is what the driver is only guessing at, and "
                                  "it is accurate to a fraction of a sample.\n"
                                  "Disarm anything you are monitoring before you start: an input "
                                  "you are listening to sends the signal back out again, which is "
                                  "a loop rather than a round trip.");
        measureButton.onClick = [this] { toggleMeasurement(); };

        addAndMakeVisible (applyButton);
        applyButton.setEnabled (false);
        applyButton.setTooltip ("Sets the correction above to the difference between what was "
                                "measured and what the driver claims, so takes land where they "
                                "were played.\n"
                                "Measure again if you change the buffer size or the driver.");
        applyButton.onClick = [this] { applyMeasurement(); };

        refreshChannelLists();
        refresh();
        startTimerHz (4);          // the device can change under this window
        setSize (560, 600);
    }

    void resized() override
    {
        auto area = getLocalBounds();

        // Bottom upwards, which reads top down as: the chooser, then what you
        // have in milliseconds and what is being taken off a take, then the
        // correction to that, then the button, then what it did. Taken in
        // that order because the button is only worth pressing once the
        // number above it has told you that you want to, and the correction
        // is only worth reaching for once the line above it has said what is
        // already being applied.
        outcome.setBounds (area.removeFromBottom (112).reduced (10, 2));

        auto buttonRow = area.removeFromBottom (34).reduced (10, 4);
        findButton   .setBounds (buttonRow.removeFromLeft (210));
        buttonRow.removeFromLeft (8);
        measureButton.setBounds (buttonRow.removeFromLeft (200));
        buttonRow.removeFromLeft (8);
        applyButton  .setBounds (buttonRow.removeFromLeft (200));

        auto loopRow = area.removeFromBottom (28).reduced (10, 2);
        loopLabel .setBounds (loopRow.removeFromLeft (190));
        loopRow.removeFromLeft (6);
        outputBox .setBounds (loopRow.removeFromLeft (140));
        toLabel   .setBounds (loopRow.removeFromLeft (26));
        inputBox  .setBounds (loopRow.removeFromLeft (140));

        auto trimRow = area.removeFromBottom (28).reduced (10, 2);
        trimLabel .setBounds (trimRow.removeFromLeft (190));
        trimRow.removeFromLeft (6);
        trimSlider.setBounds (trimRow.removeFromLeft (300));

        readout.setBounds (area.removeFromBottom (96).reduced (10, 6));
        selector.setBounds (area);
    }

private:
    /** The studio does this by itself on a first run. This is the same thing
        asked for on purpose, which is what somebody upgrading needs: their
        settings file already names a device, and the engine will not override
        a choice it has to assume was theirs.

        The work blocks the message thread, so the waiting text is put up and
        the search left until the next trip through the event loop. Otherwise
        the first thing anyone sees is a window that has stopped repainting. */
    void lookForSomethingFaster()
    {
        if (findFastestDevice == nullptr)
            return;

        // The search closes the device and opens others, so anything holding a
        // callback on it has to let go first. A measurement left running
        // across a device change would be demodulating one device's input
        // against another device's clock.
        stopMeasurement();

        findButton.setEnabled (false);
        outcome.setText ("Trying every driver on this machine, which takes a few seconds.", false);

        juce::Component::SafePointer<AudioSettingsPanel> safeThis (this);

        juce::Timer::callAfterDelay (60, [safeThis]
        {
            if (safeThis == nullptr)
                return;

            const auto result = safeThis->findFastestDevice();

            if (safeThis == nullptr)
                return;

            safeThis->outcome.setText (result, false);
            safeThis->findButton.setEnabled (true);
            safeThis->refresh();
        });
    }

    //==========================================================================
    /** The channels the two boxes offer, which are the device's active ones.

        Rebuilt whenever the device changes, and the pairing of a name to the
        index a callback would find it at is LatencyCalibration's job rather
        than this panel's: see the comment there on why offering the name of
        one channel and measuring another is the fault worth guarding.
    */
    void refreshChannelLists()
    {
        auto* device = devices.getCurrentAudioDevice();

        const auto fill = [] (juce::ComboBox& box, const juce::Array<LatencyCalibration::Channel>& channels)
        {
            const int wanted = box.getSelectedId();

            box.clear (juce::dontSendNotification);

            for (const auto& c : channels)
                box.addItem (c.name, c.index + 1);      // ids are one based

            if (channels.isEmpty())
                box.setEnabled (false);
            else
            {
                box.setEnabled (true);

                // Keep what was chosen if it is still there, so opening and
                // closing the window does not quietly re-point the cable.
                box.setSelectedId (box.indexOfItemId (wanted) >= 0 ? wanted : channels[0].index + 1,
                                   juce::dontSendNotification);
            }
        };

        if (device == nullptr)
        {
            fill (outputBox, {});
            fill (inputBox, {});
            knownChannelSignature.clear();
            return;
        }

        const auto outputs = LatencyCalibration::activeChannels (*device, false);
        const auto inputs  = LatencyCalibration::activeChannels (*device, true);

        fill (outputBox, outputs);
        fill (inputBox, inputs);

        knownChannelSignature = deviceSignature();
    }

    /** What has to stay the same for the channel boxes and a kept measurement
        to still be about this device.

        The names rather than the counts, because a count cannot see a channel
        set being changed for another of the same size: deselect input 1 and
        select input 5 in the chooser above and the box still says "Input 1"
        while callback index 0 is now input 5. An audit found that, and on
        JACK it can happen without anybody touching the chooser, since
        getActiveInputChannels there follows the port connections.

        The rate and the buffer are in it because they are what a measurement
        is of, and because the pickers are not the only thing that goes stale
        when they move.
    */
    juce::String deviceSignature() const
    {
        auto* device = devices.getCurrentAudioDevice();

        if (device == nullptr)
            return {};

        juce::String signature = device->getName()
                                 + "/" + juce::String (device->getCurrentSampleRate(), 1)
                                 + "/" + juce::String (device->getCurrentBufferSizeSamples());

        for (bool input : { false, true })
        {
            signature << (input ? "/in" : "/out");

            for (const auto& c : LatencyCalibration::activeChannels (*device, input))
                signature << ":" << c.name;
        }

        return signature;
    }

    /** Drops the kept reading and with it the offer to apply it.

        Called whenever anything the measurement was of might have moved. The
        reading is kept across stopping on purpose, so that somebody can watch
        it settle, stop, and then decide, and that is exactly what makes it
        possible to decide about a device that has gone. See
        LatencyProbe::stillApplicable for what it would have cost.
    */
    void forgetMeasurement()
    {
        lastMeasured  = {};
        measuredUnder = {};
        applyButton.setEnabled (false);
    }

    /** Whether the kept reading is still about the device that is open.

        Two things, because neither covers the other. The figures are checked
        by LatencyProbe::stillApplicable, which is where that decision is
        testable, and it catches a change of rate, of buffer size or of what
        the driver claims. It cannot see which device this is, because a device
        is not in Figures, so the signature below drops the reading outright
        whenever the device changes at all.
    */
    bool measurementStillApplies() const
    {
        return lastMeasured.status == LatencyProbe::Status::ok
               && LatencyProbe::stillApplicable (measuredUnder, claimFigures());
    }

    void channelChoiceChanged()
    {
        // A cable on different channels is a different measurement, so the
        // previous one is not an answer about it.
        forgetMeasurement();

        if (calibration.isRunning())
            startMeasurement();
        else
            refresh();
    }

    void toggleMeasurement()
    {
        if (calibration.isRunning())
        {
            stopMeasurement();
            refresh();
        }
        else
        {
            startMeasurement();
        }
    }

    void startMeasurement()
    {
        if (devices.getCurrentAudioDevice() == nullptr)
        {
            outcome.setText ("No audio device is open, so there is nothing to measure.", false);
            return;
        }

        if (! outputBox.isEnabled() || ! inputBox.isEnabled())
        {
            // A device with only one direction cannot be measured at all, and
            // Ardour refuses the same case in the same words: latency
            // calibration needs playback and capture.
            outcome.setText ("This device does not have both an input and an output enabled, "
                             "so a round trip cannot be measured through it.", false);
            return;
        }

        calibration.start (outputBox.getSelectedId() - 1, inputBox.getSelectedId() - 1);

        if (! calibration.isRunning())
        {
            outcome.setText ("Could not start the measurement.", false);
            return;
        }

        measureButton.setButtonText ("Stop measuring");
        findButton.setEnabled (false);
        forgetMeasurement();
        refresh();
    }

    void stopMeasurement()
    {
        if (! calibration.isRunning())
            return;

        calibration.stop();
        measureButton.setButtonText (measureButtonText);
        findButton.setEnabled (findFastestDevice != nullptr);

        // Deliberately does not call refresh(). refresh() is what notices a
        // device change and calls this, so calling it back from here is a
        // recursion that happens to terminate, which is not a thing to leave
        // in a file. The callers that need the panel redrawn ask for it.
        //
        // The last good reading is kept and stays applicable after stopping,
        // because the useful order of operations is to watch it settle, stop,
        // and then decide. A result that vanished with the probe would have to
        // be taken again to be used.
    }

    /** The claim the measurement is a correction to, built from the device
        itself.

        Only the three figures claimedRoundTripSamples reads are filled in.
        The engine's own latency is deliberately left at nothing: the probe is
        a device callback and never went through the mixer, so there is none of
        it in what was measured, and captureOffsetSamples adds that part
        separately.

        This is a second place that builds a Figures, which an audit raised and
        which is worth writing down rather than leaving to be discovered. The
        engine builds its own in AudioEngine::getRecordFigures, and the two
        already differ in one respect: the engine falls back to its own stored
        sample rate when the device reports none, and this does not, so this
        reports no device open where the engine would still compensate. That
        difference only changes which sentence the panel prints, because
        nothing here is applied when there is no device.

        The reason it is duplicated rather than asked for is that the window
        reaches the engine only through PreferencesActions, and adding a member
        to that means editing the file that builds it, which another branch
        holds open. It belongs in PreferencesActions next to
        recordOffsetSamples, which is already a std::function<int()> from the
        engine for exactly this kind of question, and it is recorded in
        Research/BACKLOG.md as the next thing to do here.
    */
    RecordAlign::Figures claimFigures() const
    {
        RecordAlign::Figures f;

        if (auto* device = devices.getCurrentAudioDevice())
        {
            f.inputLatency  = device->getInputLatencyInSamples();
            f.outputLatency = device->getOutputLatencyInSamples();
            f.bufferSize    = device->getCurrentBufferSizeSamples();
            f.sampleRate    = device->getCurrentSampleRate();
        }

        return f;
    }

    void applyMeasurement()
    {
        // Asked again here rather than trusted from the button's enabled
        // state. The button is a picture of what was true when it was last
        // drawn, and the device can change in the quarter second before
        // somebody's finger arrives.
        if (! measurementStillApplies())
        {
            forgetMeasurement();
            outcome.setText ("That measurement was taken on a different setup, so it is not "
                             "being applied. Measure again.", false);
            return;
        }

        const double trim = LatencyProbe::trimMsFor (lastMeasured.roundTripSamples, claimFigures());

        if (! LatencyProbe::trimFits (trim))
            return;

        // Through the slider, which is the one path from a correction to its
        // effect: it notifies, which stores the value and applies it to the
        // engine. Writing the setting directly would be a second path, and
        // the two would be free to disagree about what is in force.
        trimSlider.setValue (trim, juce::sendNotificationSync);
    }

    /** What the measurement currently says, as something somebody can act on.

        Every refusal names the thing to go and do, because a measurement that
        only says it failed leaves somebody staring at a cable. The two that
        are easy to confuse are kept apart deliberately: nothing coming back is
        a patching problem and a reading that will not settle is a signal
        problem.
    */
    juce::String measurementText (const RecordAlign::Figures& figures)
    {
        const auto reading = calibration.latest();
        const double seconds = calibration.elapsedSeconds();
        const double rate = figures.sampleRate;

        if (rate <= 0.0)
            return "No audio device is open.";

        auto ms = [rate] (double samples) { return juce::String (1000.0 * samples / rate, 2); };

        switch (reading.status)
        {
            case LatencyProbe::Status::noSignal:
                if (seconds < 1.0)
                    return "Listening for the signal coming back.";

                return "Nothing is coming back. Check that the cable really joins \""
                       + outputBox.getText() + "\" to \"" + inputBox.getText()
                       + "\", and that the input's gain is up far enough to hear a quiet tone.";

            case LatencyProbe::Status::tooHot:
                return "What is coming back is too loud to measure through: it peaks at "
                       + juce::String (reading.inputPeak, 2)
                       + " where it has to stay under " + juce::String (LatencyProbe::peakCeiling, 2)
                       + ". Turn the input gain down, or the output level if it is an output you "
                         "can set, and it will settle again in a second.";

            case LatencyProbe::Status::unreliable:
                if (seconds < 1.0)
                    return "Settling.";

                return "The signal is coming back but the measurement cannot agree with itself, "
                       "which usually means something else is in the loop: a second path back, "
                       "an effect on the way out, or too much noise on the input. Nothing is "
                       "monitoring that input, is it?";

            case LatencyProbe::Status::implausible:
                return "What came back cannot be a round trip, so nothing is being offered from "
                       "it. If there is a delay or a reverb anywhere in the path, take it out.";

            case LatencyProbe::Status::ok:
                break;
        }

        const double claimed = (double) RecordAlign::claimedRoundTripSamples (figures);
        const double trim    = LatencyProbe::trimMsFor (reading.roundTripSamples, figures);

        juce::String text;
        text << "Measured " << ms (reading.roundTripSamples) << " ms round trip ("
             << juce::String (reading.roundTripSamples, 1) << " samples).";

        // The comparison is the whole point of showing the number: the driver
        // has already been believed, and this says by how much it was wrong.
        text << "\nThe driver claims " << ms (claimed) << " ms (" << (int) claimed << " samples), ";

        if (std::abs (trim) < 0.01)
            text << "which agrees with the measurement. There is nothing to correct.";
        else if (trim > 0.0)
            text << "so it is understating by " << juce::String (trim, 2)
                 << " ms and takes are landing that much late.";
        else
            text << "so it is overstating by " << juce::String (-trim, 2)
                 << " ms and takes are being pulled that much too early.";

        if (! LatencyProbe::trimFits (trim))
            text << "\nThat is more than the correction can hold, which means this is not a "
                    "misreporting driver but something else in the path. It is not being offered.";
        else if (std::abs (trim) >= 0.01)
            text << "\nPress Use this measurement to apply it.";

        if (reading.inverted)
            text << "\nThe cable has its polarity reversed somewhere. The measurement is still "
                    "right, but that cable will cause trouble elsewhere.";

        return text;
    }

    void timerCallback() override { refresh(); }

    void refresh()
    {
        auto* device = devices.getCurrentAudioDevice();

        // The device can be changed from the chooser above without anything
        // telling this panel, and the channel boxes are indices into whatever
        // is open now. Pointing them at a device that has gone is how a
        // measurement ends up being taken on a channel nobody patched, and
        // keeping a reading across it is how a correction for one device gets
        // applied to another.
        if (deviceSignature() != knownChannelSignature)
        {
            stopMeasurement();
            forgetMeasurement();
            refreshChannelLists();
        }

        // Outside the branch below, and that is the point: the offer to apply
        // a kept reading has to be withdrawn by whatever makes it
        // inapplicable, not only refreshed while a measurement happens to be
        // running. It was inside, which is how a measurement from one device
        // stayed on offer for the next one.
        applyButton.setEnabled (measurementStillApplies()
                                && LatencyProbe::trimFits (
                                       LatencyProbe::trimMsFor (lastMeasured.roundTripSamples,
                                                                claimFigures())));

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
                text << "\n\"" << driver << "\" shares the device with everything else on the system "
                        "and buffers for it. Press Find the fastest device below. If you have an "
                        "audio interface, its own ASIO driver is the one to be on.";
           #else
            juce::ignoreUnused (driver);
           #endif
        }

        // What is actually being taken off a take, which is the round trip
        // above plus whatever the mixer is holding back for a lookahead
        // plugin plus the correction below. Said in milliseconds and in
        // samples: milliseconds is what anybody feels, and samples is what
        // has to be compared against another studio's record offset.
        if (recordOffsetSamples != nullptr)
        {
            const int offset = recordOffsetSamples();
            const juce::String ms = juce::String (std::abs (1000.0 * offset / rate), 1);

            text << "\nRecorded takes are ";

            if (offset > 0)
                text << "pulled " << ms << " ms earlier (" << offset
                     << " samples), so a part lands where it was played.";
            else if (offset < 0)
                text << "pushed " << ms << " ms later (" << -offset
                     << " samples), which only the correction below asks for.";
            else
                text << "left exactly where they arrive, which takes a correction below "
                        "that cancels the latency being reported.";
        }

        readout.setText (text, juce::dontSendNotification);

        // The measurement's own report, which is only written while a
        // measurement is live or has just produced something. Otherwise this
        // box belongs to whatever was pressed last.
        if (calibration.isRunning())
        {
            const auto figures = claimFigures();
            const auto reading = calibration.latest();

            if (reading.status == LatencyProbe::Status::ok)
            {
                // Kept together, because a reading without the figures it was
                // taken under cannot be checked against anything later.
                lastMeasured  = reading;
                measuredUnder = figures;
            }

            outcome.setText (measurementText (figures), false);

            applyButton.setEnabled (measurementStillApplies()
                                    && LatencyProbe::trimFits (
                                           LatencyProbe::trimMsFor (lastMeasured.roundTripSamples,
                                                                    figures)));
        }
    }

    juce::AudioDeviceManager& devices;

    // Immediately after the reference it is built from, and before every
    // control, so that it is destroyed after all of them. findButton's
    // onClick, trimSlider's onValueChange and the device chooser all reach it,
    // and a member declared before it would be destroyed after it. Nothing
    // reachable actually calls into it from a destructor, but an audit pointed
    // out that the comment claiming this used to sit under a declaration that
    // did not have the property, which is worse than no comment.
    LatencyCalibration calibration { devices };

    juce::AudioDeviceSelectorComponent selector;
    juce::Label      readout;
    juce::TextEditor outcome;
    juce::TextButton findButton { "Find the fastest device" };

    std::function<juce::String()> findFastestDevice;
    std::function<int()>          recordOffsetSamples;
    std::function<void (double)>  onTrimChanged;

    juce::Label  trimLabel { {}, "Record offset correction" };
    juce::Slider trimSlider;

    static constexpr const char* measureButtonText = "Measure the round trip";

    juce::Label      loopLabel { {}, "Loopback cable, from" };
    juce::Label      toLabel   { {}, "to" };
    juce::ComboBox   outputBox, inputBox;
    juce::TextButton measureButton { measureButtonText };
    juce::TextButton applyButton   { "Use this measurement" };

    /** The last reading good enough to be used, kept so that stopping the
        measurement does not throw away the answer it was taken for, and the
        figures it was taken under, so that it can be refused once they have
        moved. */
    LatencyProbe::Reading lastMeasured;
    RecordAlign::Figures  measuredUnder;

    /** What the channel boxes were built from, so a device change is noticed
        without asking the device manager for a callback this panel does not
        otherwise need. */
    juce::String knownChannelSignature;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioSettingsPanel)
};
