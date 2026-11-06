#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <cmath>
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

    A reading costs about nine milliseconds at 44.1 kHz, which is why this is
    a thread rather than a timer callback: ten of those a second on the message
    thread is a tenth of the interface's time and would be felt as a stutter in
    everything else while the tuner is open.

    THE COST GROWS AS THE SQUARE OF THE SAMPLE RATE, which is a property of the
    window rather than of this code: the window is a fixed duration, so both it
    and the longest lag are proportional to the rate, and the difference
    function's work is their product. At 192 kHz one reading is 197 ms, so a
    tenth of a second between readings is not available at any price.

    How long to wait is therefore a decision with arithmetic behind it, and it
    is `Tuner::Pacing` rather than two lines in here. The duty cycle, the
    dither that keeps a vibrato from hiding behind the reading rate, the cost
    table it was all measured from and what is still wrong with the whole
    arrangement are in that header, where a test can drive it. This class keeps
    the clock and the sleep.

    What it does keep is the measuring. The interval between readings is not a
    constant, so the gate is handed what the clock said rather than what this
    thread intended, which is the fault an audit found in the first version of
    this file: it passed the nominal tenth of a second at every rate, so above
    48 kHz, where the duty cycle takes over, it told the gate a figure 1.92 and
    then 7.88 times too small.

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

        // Low priority, and held to a quarter of a core by its own duty cycle,
        // because this shares a process with a realtime device callback. At
        // 44.1 kHz it is asleep for nine tenths of its life; at the top of the
        // studio's rate range it is asleep for three quarters of it.
        startThread (juce::Thread::Priority::low);
    }

    void stop()
    {
        // `stopThread` notifies as well as signalling, so the wait is one
        // reading rather than one interval. One reading is the 197 ms measured
        // at 192 kHz, the top of the studio's range, and the cost grows as the
        // square of the rate: four seconds is that with room for a device two
        // doublings faster on a machine several times slower. On a timeout
        // JUCE kills the thread by force, which here would be inside the
        // estimator's own buffers with the publish lock possibly held, so the
        // margin is deliberately silly rather than tight.
        stopThread (4000);
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

        /** True when there is no live audio to read: either a full window has
            not arrived yet, or none has arrived for the last few passes.

            The second half is not a nicety. The ring's counter is monotonic
            and nothing resets it, so "enough has ever arrived" latches true
            the moment it does, and the ring goes on holding the last tenth of
            a second of whatever was played. An estimator handed the same
            buffer every pass reads it as perfectly periodic, so a device that
            stops, because an interface was unplugged or the machine slept,
            would leave a bright settled note on the display for the rest of
            the session with nothing to say it was not live. */
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
        std::uint64_t lastWritten = 0;
        int quietPasses = 0;
        int readings = 0;

        // The interval that actually elapsed between the start of this pass
        // and the start of the one before it, which is what the gate needs and
        // the only thing that can be measured rather than intended. Seeded
        // with the nominal so the first full window is not divided by a figure
        // from before the thread existed.
        double previousStartedAt = 0.0;
        double measuredHop = pacing.settings().nominalSeconds;

        // The seed is the clock, so two instances started at different moments
        // dither differently. Nothing here has to reproduce a sequence: the
        // test reproduces it from its own seed.
        pacing.configure ({}, (std::uint64_t) juce::Time::getHighResolutionTicks());

        while (! threadShouldExit())
        {
            const auto startedAt = juce::Time::getMillisecondCounterHiRes();

            if (previousStartedAt > 0.0)
                measuredHop = (startedAt - previousStartedAt) / 1000.0;

            previousStartedAt = startedAt;

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

                // The nominal pacing only, which is what the gate assumes when
                // it is not handed a measured interval and the scale its stall
                // bound is expressed in. The interval the rate is actually
                // divided by is passed to `process` per reading, below.
                Tuner::GateSettings gate;
                gate.frameSeconds = pacing.settings().nominalSeconds;

                analyser.configure (spec, gate);

                window.assign ((size_t) analyser.requiredSampleCount(), 0.0f);
                configuredRate = rate;
                baseline = ring.totalWritten();
                readings = 0;
            }

            analyser.setReferenceHz (wantedReference.load());

            // So that the stall bound is expressed in this machine's real
            // pacing rather than in the nominal one, which above 48 kHz it
            // never reaches. Does not reset the window: a pacing figure that
            // drifts with load would otherwise reopen the display every time
            // the machine got busy.
            analyser.setFrameSeconds (measuredHop);

            // Whether the device is still delivering, which the ring's own
            // counter cannot say on its own: it is monotonic, so it has to be
            // compared against what it was last pass rather than against a
            // threshold. A few passes of grace because a pass where no block
            // happened to land is normal on a loaded machine, and blanking the
            // display for that would be a flicker.
            const auto writtenNow = ring.totalWritten();
            quietPasses = writtenNow == lastWritten ? quietPasses + 1 : 0;
            lastWritten = writtenNow;

            const bool enoughAudio = configuredRate > 0.0
                                  && ! window.empty()
                                  && writtenNow - baseline >= (std::uint64_t) window.size();

            const bool live = enoughAudio && quietPasses < quietPassesAllowed;

            Snapshot next;
            next.readings = readings;

            if (! live)
            {
                // Nothing to read. The history goes with it, and the next full
                // window is counted from here, so a device that stops and comes
                // back, or is swapped for another one at the same rate, cannot
                // have the two spliced into one analysis window: that splice
                // reads as a perfectly plausible wrong pitch.
                if (quietPasses >= quietPassesAllowed)
                {
                    analyser.reset();
                    baseline = writtenNow;
                }

                next.waitingForAudio = true;
            }
            else if (ring.readLatest (window))
            {
                next.reading = analyser.process (window.data(), (int) window.size(), measuredHop);
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
                next.reading = latest().reading;
                next.waitingForAudio = false;
            }

            {
                const juce::SpinLock::ScopedLockType lock (publishLock);
                published = next;
            }

            // Measured from the start of the work rather than the end of it,
            // so the reading rate does not drift with how long a reading took.
            // Everything about how long to wait, including why it is not the
            // same length twice, is in `Tuner::Pacing`.
            const double spent = (juce::Time::getMillisecondCounterHiRes() - startedAt) / 1000.0;
            const double sleepFor = pacing.nextWait (spent);

            if (threadShouldExit())
                break;

            // `wait` takes milliseconds and rounds to an int, so a sub
            // millisecond wait would round to zero and spin. One millisecond
            // is the floor rather than zero.
            wait (std::max (1, (int) std::lround (sleepFor * 1000.0)));
        }
    }

    /** How many passes with no new audio at all before the display is blanked.

        Three, which is how the figure has to be expressed: a count of passes
        and not a duration, because a pass is a tenth of a second at 44.1 and
        48 kHz and most of a second at 192, where one reading costs more than
        the nominal interval. What it has to beat is the longest gap between
        blocks the device can leave, and that scales the same way a pass does,
        which is why a count travels and a duration would not. Short enough at
        every rate that nobody reads a dead note off the screen. */
    static constexpr int quietPassesAllowed = 3;

    /** The duty cycle and the dither. Owned here, configured in `run` so the
        seed is the moment the thread started, and touched by nothing else. */
    Tuner::Pacing pacing;

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

        // The tap and the thread run for exactly as long as this component
        // exists, rather than being hung off visibility. The window owns the
        // component and is destroyed when it is closed, so that is the same
        // span and it is deterministic: nothing can leave the engine copying
        // input into a ring that nothing is reading.
        engine.setTunerListening (true);
        analysis.start();
        startTimerHz (30);
    }

    ~TunerComponent() override
    {
        // The thread first, then the tap. Stopping the tap first would leave
        // the thread reading a ring nothing is filling, which is harmless but
        // means closing the window waits for a pass that had no reason to run.
        stopTimer();
        analysis.stop();
        engine.setTunerListening (false);
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
        // was choosing from it. Compared by name rather than by count: two
        // interfaces can both have eight inputs, and a menu still naming the
        // one that was unplugged is a menu that lies about which socket it
        // means.
        if (engine.getActiveInputChannelNames() != lastInputNames)
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
        // The audio device first, because every other message below would be
        // a guess about a signal nobody is being handed. There is deliberately
        // no check on the engine's sample rate here: it holds its last value
        // when a device closes rather than going to zero, so reading it would
        // report a device that is working.
        if (snap.waitingForAudio)
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
        lastInputNames = names;

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

        // A choice the device no longer offers, which is what happens when an
        // eight input interface is swapped for a two input one, is given up
        // rather than left selected invisibly. `resolve` would clamp it onto
        // some other socket and the menu would go on reading "Follow the
        // mixer", so the engine and the menu have to be put back in step here
        // rather than one of them quietly meaning something else.
        if (wanted.assigned && selected == followTheMixerId)
            engine.setTunerSource ({});

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
    juce::StringArray lastInputNames;
    juce::Rectangle<int> readout;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TunerComponent)
};

//==============================================================================
inline void closeTunerWindow();

/** The tuner's own window, and deliberately not a dialog.

    `DialogWindow::launchAsync` enters a modal state, which is right for
    Preferences and wrong here: tuning is something done while a session is
    live, and a modal tuner takes the transport, the mixer and every input gain
    with it for as long as it is open. The mixer window is the pattern this
    follows.
*/
class TunerWindow : public juce::DocumentWindow
{
public:
    TunerWindow (AudioEngine& engine, juce::PropertiesFile& settings)
        : juce::DocumentWindow ("Tuner  -  ALLHAILPAN Studio", Ahp::black,
                                juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new TunerComponent (engine, settings), true);
        setResizable (true, false);
        setResizeLimits (320, 240, 1200, 900);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);

        // After the window is visible, which is what DarkTitleBar.h asks for:
        // there is no peer before that, so there is no native handle to set the
        // attribute on and the title bar would stay in the Windows light theme.
        // Exactly the shape of the app icon fault, where the thing was present
        // in the file and absent on screen.
        Ahp::applyDarkTitleBar (*this);
    }

    void closeButtonPressed() override
    {
        // Hidden now and deleted on the next message, because this is being
        // called from inside the window's own callback and destroying it here
        // would be destroying the object whose method is still running.
        setVisible (false);
        juce::MessageManager::callAsync ([] { closeTunerWindow(); });
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TunerWindow)
};

inline std::unique_ptr<TunerWindow>& tunerWindow()
{
    static std::unique_ptr<TunerWindow> open;
    return open;
}

/** Opens the tuner, or brings it to the front if it is already up. */
inline void showTunerWindow (AudioEngine& engine, juce::PropertiesFile& settings)
{
    auto& open = tunerWindow();

    // Visible as well as present, because a window whose close button has been
    // pressed is hidden immediately and destroyed on the next message. Asking
    // for the tuner in that gap would otherwise bring a hidden window to the
    // front and then delete it, so the tuner would appear not to open.
    if (open != nullptr && open->isVisible())
    {
        open->toFront (true);
        return;
    }

    open = std::make_unique<TunerWindow> (engine, settings);
}

inline void closeTunerWindow()
{
    tunerWindow().reset();
}
