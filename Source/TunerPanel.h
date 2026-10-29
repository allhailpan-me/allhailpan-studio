#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <vector>
#include "AhpLookAndFeel.h"
#include "AudioEngine.h"
#include "DarkTitleBar.h"
#include "Preferences.h"
#include "Tuner.h"

// ---------------------------------------------------------------------------
// The tuner's window: a readout over Tuner.h, and the thread that drives it.
//
// Everything that decides what is displayed is in Tuner.h, which carries no
// JUCE and is tested. This file is only the parts that genuinely need JUCE:
// getting samples out of the engine's ring on a thread of their own, and
// drawing the answer.
// ---------------------------------------------------------------------------

/** Runs the estimator.

    The estimator costs about nine milliseconds a reading, which is why this
    is a thread rather than a timer callback: ten of those a second on the
    message thread is a tenth of the interface's time, and it would be felt as
    a stutter in everything else while the tuner is open.

    Published through a spin lock rather than an atomic because a `Reading` is
    several numbers that have to agree with each other. Neither side of that
    lock is the audio thread: the audio thread's only part in this is writing
    samples into the engine's ring, which it does without any lock at all.
*/
class TunerAnalysis : private juce::Thread
{
public:
    TunerAnalysis (AudioEngine& e) : juce::Thread ("Tuner"), engine (e) {}

    ~TunerAnalysis() override { stop(); }

    void start()
    {
        if (isThreadRunning())
            return;

        // A reading is wanted ten times a second and takes about nine
        // milliseconds, so this thread is asleep for the great majority of its
        // life. Below normal priority so that it cannot take time from the
        // interface even if a future search range made a reading dearer.
        startThread (juce::Thread::Priority::low);
    }

    void stop()
    {
        stopThread (1000);
    }

    /** The reference pitch, in Hz. Read at the top of every pass, so moving it
        shows up on the next reading rather than on the next note. */
    void setReferenceHz (double hz) noexcept { wantedReference.store (Tuner::Scale::quantiseReference (hz)); }
    double getReferenceHz() const noexcept   { return wantedReference.load(); }

    struct Snapshot
    {
        Tuner::Analyser::Reading reading;

        /** How many readings have been taken since the window opened. A
            display needs this to tell "nothing yet" from "nothing there". */
        int  readings = 0;

        /** True when the ring has not yet handed over a full window. At 44.1
            kHz that is the first tenth of a second after the tuner opens, and
            for as long as a device is closed it never stops being true. */
        bool waitingForAudio = true;
    };

    Snapshot latest() const
    {
        const juce::SpinLock::ScopedLockType lock (publishLock);
        return published;
    }

private:
    void run() override
    {
        // Owned by this thread, so nothing here is shared and nothing here
        // needs a lock: the analyser holds the scratch buffers the estimator
        // works in, which is also why it is configured once rather than per
        // reading.
        Tuner::Analyser analyser;
        std::vector<float> window;
        double configuredRate = 0.0;
        std::uint64_t baseline = 0;
        int readings = 0;

        while (! threadShouldExit())
        {
            const auto startedAt = juce::Time::getMillisecondCounterHiRes();

            auto& ring = engine.tunerInput();
            const double rate = engine.getSampleRate();

            // A device opening at a different rate changes every lag the
            // estimator computes, so the analyser is rebuilt and the ring is
            // treated as empty again. Without the re-baseline the first
            // reading after a rate change would be taken over samples recorded
            // at two different rates, which reads as a perfectly plausible
            // wrong pitch.
            if (rate > 0.0 && rate != configuredRate)
            {
                Tuner::Spec spec;
                spec.sampleRate = rate;
                analyser.configure (spec);

                window.assign ((size_t) analyser.requiredSampleCount(), 0.0f);
                configuredRate = rate;
                baseline = ring.totalWritten();
                readings = 0;
            }

            analyser.setReferenceHz (wantedReference.load());

            Snapshot next;
            next.readings = readings;

            const bool enoughAudio = configuredRate > 0.0
                                  && ! window.empty()
                                  && ring.totalWritten() - baseline >= (std::uint64_t) window.size();

            if (enoughAudio && ring.readLatest (window))
            {
                next.reading = analyser.process (window.data(), (int) window.size());
                next.readings = ++readings;
                next.waitingForAudio = false;
            }
            else
            {
                // A torn copy is not an error worth showing: the writer
                // overtook the reader, the samples are simply not usable, and
                // the next pass is a tenth of a second away. What is kept is
                // the previous reading, so the display holds rather than
                // flickering to dashes and back.
                const auto held = latest();
                next.reading = held.reading;
                next.waitingForAudio = ! enoughAudio;
            }

            {
                const juce::SpinLock::ScopedLockType lock (publishLock);
                published = next;
            }

            // Ten readings a second, measured from the start of the work
            // rather than the end of it, so the rate does not drift with how
            // long a reading took.
            const double spent = juce::Time::getMillisecondCounterHiRes() - startedAt;
            const double sleepFor = juce::jlimit (1.0, frameMs, frameMs - spent);

            if (threadShouldExit())
                break;

            wait (sleepFor);
        }
    }

    /** A hundred milliseconds, which is the spacing the gate's stability
        figure in Tuner.h is derived at. Driving it faster would make four
        agreeing frames a shorter wait and so a weaker promise about how close
        to settled the string is. */
    static constexpr double frameMs = 100.0;

    AudioEngine& engine;
    std::atomic<double> wantedReference { Tuner::Scale::standardReferenceHz };

    mutable juce::SpinLock publishLock;
    Snapshot published;
};

//==============================================================================
/** The readout.

    Deliberately no colour for in tune against out of tune: this studio's
    palette is black, bone and red for recording only, and a tuner that read
    its answer out in green would be the one thing on screen that did not
    belong to it. The note goes bright and the needle fills its centre
    instead, which is as fast to read and does not depend on the person seeing
    colour at all.
*/
class TunerComponent : public juce::Component,
                       private juce::Timer
{
public:
    TunerComponent (AudioEngine& e, juce::PropertiesFile& s)
        : engine (e), settings (s), analysis (e)
    {
        addAndMakeVisible (sourceLabel);
        sourceLabel.setText ("Input", juce::dontSendNotification);
        sourceLabel.setFont (juce::FontOptions (12.0f));
        sourceLabel.setColour (juce::Label::textColourId, Ahp::muted);

        addAndMakeVisible (sourceBox);
        sourceBox.onChange = [this] { applySource(); };

        addAndMakeVisible (referenceLabel);
        referenceLabel.setText ("A4", juce::dontSendNotification);
        referenceLabel.setFont (juce::FontOptions (12.0f));
        referenceLabel.setColour (juce::Label::textColourId, Ahp::muted);

        addAndMakeVisible (reference);
        reference.setSliderStyle (juce::Slider::IncDecButtons);
        reference.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 72, 22);
        reference.setRange (Tuner::Scale::minReferenceHz,
                            Tuner::Scale::maxReferenceHz,
                            Tuner::Scale::referenceStepHz);
        reference.setTextValueSuffix (" Hz");
        reference.setValue (storedReference(), juce::dontSendNotification);
        reference.onValueChange = [this] { applyReference(); };

        addAndMakeVisible (resetReference);
        resetReference.setButtonText ("440");
        resetReference.onClick = [this]
        {
            reference.setValue (Tuner::Scale::standardReferenceHz);
        };

        analysis.setReferenceHz (reference.getValue());

        refreshSources();
        setSize (420, 300);
    }

    ~TunerComponent() override
    {
        // The thread first, then the tap: stopping the tap first would leave
        // the thread reading a ring nothing is filling, which is harmless but
        // means the window can only be closed a tenth of a second later than
        // it was asked to be.
        stopTimer();
        analysis.stop();
        engine.setTunerListening (false);
    }

    void visibilityChanged() override
    {
        if (isVisible())
        {
            engine.setTunerListening (true);
            analysis.start();
            startTimerHz (30);
        }
        else
        {
            stopTimer();
            analysis.stop();
            engine.setTunerListening (false);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14);

        auto top = r.removeFromTop (24);
        sourceLabel.setBounds (top.removeFromLeft (40));
        sourceBox.setBounds (top.removeFromLeft (200));

        auto bottom = r.removeFromBottom (26);
        referenceLabel.setBounds (bottom.removeFromLeft (28));
        reference.setBounds (bottom.removeFromLeft (150));
        bottom.removeFromLeft (8);
        resetReference.setBounds (bottom.removeFromLeft (50));

        readout = r;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Ahp::panel);

        const auto snap = analysis.latest();
        const auto& r   = snap.reading;

        auto area = readout;
        auto needle = area.removeFromBottom (54);
        area.removeFromBottom (6);

        drawNeedle (g, needle.toFloat(), r);

        if (! r.hasNote)
        {
            g.setColour (Ahp::muted);
            g.setFont (juce::FontOptions (62.0f));
            g.drawText ("--", area.removeFromTop (72), juce::Justification::centred);

            g.setFont (juce::FontOptions (13.0f));
            g.drawText (statusText (snap), area, juce::Justification::centredTop);
            return;
        }

        // Bright only when it is in tune, which is the whole signal this
        // display gives. One cent is the figure Boss and Korg claim for a
        // chromatic tuner, so it is what "in tune" is taken to mean here
        // rather than a number chosen to make the display feel responsive.
        const bool inTune = std::abs (r.cents) <= inTuneCents;

        g.setColour (inTune ? Ahp::bone : Ahp::muted);
        g.setFont (juce::FontOptions (62.0f));

        auto noteRow = area.removeFromTop (72);
        g.drawText (juce::String (Tuner::Scale::noteName (r.midi))
                    + juce::String (Tuner::Scale::noteOctave (r.midi)),
                    noteRow, juce::Justification::centred);

        g.setColour (Ahp::bone);
        g.setFont (juce::FontOptions (15.0f));
        g.drawText (centsText (r.cents), area.removeFromTop (20), juce::Justification::centred);

        g.setColour (Ahp::muted);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (juce::String (r.hz, 2) + " Hz   target " + juce::String (r.targetHz, 2) + " Hz",
                    area.removeFromTop (18), juce::Justification::centred);
    }

private:
    /** Plus or minus one cent, which is what every chromatic tuner on the
        market claims and what this estimator measures on a tone whose answer
        is known. */
    static constexpr double inTuneCents = 1.0;

    /** How far the needle travels. Fifty cents is a semitone either way, so
        the needle covers everything `describe` can report and never pins. */
    static constexpr double needleCents = 50.0;

    void timerCallback() override
    {
        // The source menu is rebuilt only when the device's inputs change,
        // because rebuilding it under the mouse would close it while somebody
        // was choosing from it.
        if (engine.getActiveInputChannelNames().size() != lastInputCount)
            refreshSources();

        repaint();
    }

    static juce::String centsText (double cents)
    {
        const auto rounded = juce::String (std::abs (cents), 1);

        if (cents > 0.05)
            return "+" + rounded + " cents sharp";

        if (cents < -0.05)
            return "-" + rounded + " cents flat";

        return "in tune";
    }

    juce::String statusText (const TunerAnalysis::Snapshot& snap) const
    {
        if (engine.getSampleRate() <= 0.0 || snap.waitingForAudio)
            return "waiting for the audio device";

        if (engine.getTunerInputChannel() < 0)
            return "no input to listen to";

        if (snap.reading.settling)
            return "listening";

        if (snap.reading.levelDb < -100.0)
            return "silence on " + sourceDescription();

        return "play one note on " + sourceDescription();
    }

    juce::String sourceDescription() const
    {
        const int channel = engine.getTunerInputChannel();

        if (channel < 0)
            return "no input";

        const auto names = engine.getActiveInputChannelNames();

        if (juce::isPositiveAndBelow (channel, names.size()))
            return names[channel];

        return "input " + juce::String (channel + 1);
    }

    void drawNeedle (juce::Graphics& g, juce::Rectangle<float> area,
                     const Tuner::Analyser::Reading& r) const
    {
        const float midX = area.getCentreX();

        g.setColour (Ahp::panel2);
        g.fillRect (area);

        g.setColour (Ahp::line);

        // A mark every ten cents, so the scale can be read without numbers on
        // it, and the centre drawn taller because that is the one the eye has
        // to find.
        for (int c = -50; c <= 50; c += 10)
        {
            const float x = midX + (float) (c / needleCents) * area.getWidth() * 0.5f;
            const float h = c == 0 ? area.getHeight() * 0.6f : area.getHeight() * 0.3f;
            g.fillRect (x - 0.5f, area.getCentreY() - h * 0.5f, 1.0f, h);
        }

        if (! r.hasNote)
            return;

        const double clamped = juce::jlimit (-needleCents, needleCents, r.cents);
        const float  x = midX + (float) (clamped / needleCents) * area.getWidth() * 0.5f;
        const bool   inTune = std::abs (r.cents) <= inTuneCents;

        g.setColour (inTune ? Ahp::bone : Ahp::muted);
        g.fillRect (x - 1.5f, area.getY() + 4.0f, 3.0f, area.getHeight() - 8.0f);
    }

    /** The reference the window opens at.

        `PropertiesFile` hands back zero for anything it cannot parse, and zero
        is inside no range, so an absent or damaged setting has to be told from
        a real one before it is clamped. Clamping zero would open the tuner at
        410 Hz, which is a tuner that is wrong in a way nobody would think to
        look for. */
    double storedReference() const
    {
        const auto text = settings.getValue (referenceKey);

        if (! Prefs::looksNumeric (text.toRawUTF8()))
            return Tuner::Scale::standardReferenceHz;

        return Tuner::Scale::quantiseReference (text.getDoubleValue());
    }

    void applyReference()
    {
        const double hz = Tuner::Scale::quantiseReference (reference.getValue());

        analysis.setReferenceHz (hz);
        settings.setValue (referenceKey, juce::String (hz, 1));
        repaint();
    }

    void refreshSources()
    {
        const auto names = engine.getActiveInputChannelNames();
        lastInputCount = names.size();

        options = InputSource::options (names.size());

        sourceBox.clear (juce::dontSendNotification);
        sourceBox.addItem ("Follow the mixer", followTheMixerId);

        for (size_t i = 0; i < options.size(); ++i)
        {
            const auto& choice = options[i];
            auto text = juce::String (InputSource::describe (choice));

            if (juce::isPositiveAndBelow (choice.first, names.size()))
                text += "  " + names[choice.first];

            sourceBox.addItem (text, firstSourceId + (int) i);
        }

        const auto wanted = engine.getTunerSource();
        int selected = followTheMixerId;

        if (wanted.assigned)
            for (size_t i = 0; i < options.size(); ++i)
                if (options[i].first == wanted.choice.first
                    && options[i].stereo == wanted.choice.stereo)
                    selected = firstSourceId + (int) i;

        sourceBox.setSelectedId (selected, juce::dontSendNotification);
    }

    void applySource()
    {
        const int id = sourceBox.getSelectedId();
        const int index = id - firstSourceId;

        if (id == followTheMixerId || ! juce::isPositiveAndBelow (index, (int) options.size()))
        {
            engine.setTunerSource ({});
            return;
        }

        engine.setTunerSource ({ options[(size_t) index], true });
    }

    static constexpr int followTheMixerId = 1;
    static constexpr int firstSourceId    = 2;
    static constexpr const char* referenceKey = "tunerReferenceHz";

    AudioEngine&          engine;
    juce::PropertiesFile& settings;
    TunerAnalysis         analysis;

    juce::Label    sourceLabel, referenceLabel;
    juce::ComboBox sourceBox;
    juce::Slider   reference;
    juce::TextButton resetReference;

    std::vector<InputSource::Choice> options;
    int lastInputCount = -1;
    juce::Rectangle<int> readout;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TunerComponent)
};

//==============================================================================
inline juce::Component::SafePointer<juce::DialogWindow>& tunerWindow()
{
    static juce::Component::SafePointer<juce::DialogWindow> open;
    return open;
}

/** Opens the tuner, or brings it to the front if it is already up. */
inline void showTunerWindow (AudioEngine& engine, juce::PropertiesFile& settings)
{
    auto& open = tunerWindow();

    if (open != nullptr)
    {
        open->toFront (true);
        return;
    }

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new TunerComponent (engine, settings));
    o.dialogTitle                  = "Tuner";
    o.dialogBackgroundColour       = Ahp::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar            = true;
    o.resizable                    = true;

    if (auto* w = o.launchAsync())
    {
        Ahp::applyDarkTitleBar (*w);
        open = w;
    }
}

inline void closeTunerWindow()
{
    tunerWindow().deleteAndZero();
}
