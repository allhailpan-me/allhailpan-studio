#include "MainComponent.h"
#include "AudioDefaults.h"
#include "MidiFileIO.h"
#include "AhpLookAndFeel.h"
#include "DarkTitleBar.h"
#include "PanOne.h"
#include "Recorder.h"
#include <cmath>

namespace
{
    // bar : beat : tick, where a beat is the denominator unit rather than a
    // quarter note, so 6/8 reads 1:1 to 1:6. The tick is `Meter`'s, which is
    // what makes this the same readout as the playlist's rather than a second
    // one that looks like it: the two copies of this line had drifted, and
    // this one truncated where the other rounded.
    juce::String formatPosition (const Meter::Map& meter, double beats)
    {
        beats = std::max (0.0, beats);
        const auto at = meter.barBeatAt (beats);
        return juce::String ((int) at.bar) + ":" + juce::String (at.beat) + ":"
             + juce::String (Meter::tickOfFraction (at.fraction)).paddedLeft ('0', 2);
    }

    /** A bar number for a message, counting from one and carrying the fraction
        of a bar, which is what "replace bars 5.00 to 9.00" wants. */
    juce::String barNumber (const Meter::Map& meter, double beats)
    {
        return juce::String (beats / std::max (1.0e-9, meter.barLengthQuartersAt (beats)) + 1.0, 2);
    }

    enum RecordModeId { recAuto = 1, recAudio, recMidi, recBoth, recInstrument, recInstrumentMidi };

}

MainComponent::MainComponent()
{
    // The built in instrument's wavetables, built here rather than the first
    // time somebody loads it. It is a few hundred transforms and forty
    // megabytes, which is a third of a second at launch where nobody is
    // listening, against a third of a second in the middle of somebody's
    // work. Nothing in the instrument is built lazily, and this is where
    // that promise is actually kept.
    PanOne::prepareSharedResources();

    auto savedDevice = plugins.settings().getXmlValue ("audioDevice");
    startupError = engine.start (savedDevice.get());

    // ---- transport ----
    playButton.onClick   = [this] { togglePlay(); };
    stopButton.onClick   = [this] { stopAll(); };
    stopButton.setTooltip ("Stop (Space). Press again to silence any note that's still ringing.");
    recordButton.onClick = [this] { toggleRecord(); };
    recordButton.setColour (juce::TextButton::buttonOnColourId, Ahp::rec);
    recordButton.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    recordButton.setTooltip ("Record (Ctrl+R)");

    monitorBox.addItem ("Monitor: off", 1);
    monitorBox.addItem ("Monitor: armed", 2);
    monitorBox.addItem ("Monitor: on", 3);
    monitorBox.setSelectedId (1, juce::dontSendNotification);
    monitorBox.setTooltip ("Hear your inputs through this studio's own effects.\n"
                           "armed = only the mixer strips that are armed to record, on = every "
                           "strip that has an input.\n"
                           "Pick which input feeds which strip in the mixer.\n"
                           "Use headphones: monitoring through speakers near a microphone feeds back.");
    monitorBox.onChange = [this]
    {
        const auto mode = (AudioEngine::Monitor) (monitorBox.getSelectedId() - 1);
        engine.setMonitorMode (mode);
        plugins.settings().setValue (Prefs::Key::monitorMode, monitorBox.getSelectedId());

        if (mode != AudioEngine::Monitor::off)
        {
            const double roundTrip = engine.monitoringRoundTripMs();

            juce::String message;
            const int wired = engine.countWiredInputs();

            if (wired == 0)
                message << "No inputs are wired to a mixer insert yet. Open the mixer and "
                           "pick one on the strip you want to record through.";
            else
                message << "Monitoring " << wired << (wired == 1 ? " input" : " inputs")
                        << " at " << juce::String (roundTrip, 1) << " ms round trip. "
                           "Use headphones: speakers near a microphone will feed back.";

            // Said at the moment it matters, which is when somebody turns
            // monitoring on with an instrument in their hands rather than
            // when they go looking in a settings window. The number alone is
            // only useful to somebody who already knows what a good one is,
            // so when it is bad it says so and says where to go. The studio
            // picks the fastest driver it can on first run, so getting here
            // means either the machine will not go faster or the device has
            // been chosen by hand since.
            if (AudioDefaults::tooSlowToPlay (roundTrip))
                message << " That is too slow to play through: open Audio settings and try another device type.";

            setStatus (message);
        }
    };
    // Through choiceId, because an id the box does not have selects nothing,
    // which reads back as zero, and zero minus one is not a monitoring mode the
    // engine has. See Preferences.h.
    monitorBox.setSelectedId (Prefs::choiceId (plugins.settings().getIntValue (Prefs::Key::monitorMode,
                                                                              Prefs::monitorMode.fallback),
                                              Prefs::monitorMode),
                             juce::dontSendNotification);
    engine.setMonitorMode ((AudioEngine::Monitor) (monitorBox.getSelectedId() - 1));

    recordMode.addItem ("Rec: Auto", recAuto);
    recordMode.addItem ("Rec: Input audio", recAudio);
    recordMode.addItem ("Rec: MIDI + automation", recMidi);
    recordMode.addItem ("Rec: Input audio + MIDI", recBoth);
    recordMode.addItem ("Rec: Instrument sound", recInstrument);
    recordMode.addItem ("Rec: Instrument sound + MIDI", recInstrumentMidi);
    static_assert (Prefs::recordMode.count == (int) recInstrumentMidi,
                   "a record mode was added without telling Preferences.h, so a stored id for it "
                   "would be rejected as out of range");

    recordMode.setSelectedId (Prefs::choiceId (plugins.settings().getIntValue (Prefs::Key::recordMode,
                                                                              Prefs::recordMode.fallback),
                                              Prefs::recordMode),
                              juce::dontSendNotification);
    recordMode.onChange = [this] { plugins.settings().setValue (Prefs::Key::recordMode, recordMode.getSelectedId()); };
    recordMode.setTooltip ("Auto records the selected channel's sound and its MIDI when an instrument is loaded,\n"
                           "otherwise audio from your interface.\n"
                           "\"Instrument sound\" captures whatever the plugin makes, including notes you play with the\n"
                           "mouse inside its own window, which never reach the app as MIDI.");

    countInBox.addItem ("Count: off", 1);
    countInBox.addItem ("Count: 1 bar", 2);
    countInBox.addItem ("Count: 2 bars", 3);
    countInBox.setSelectedId (Prefs::choiceId (plugins.settings().getIntValue (Prefs::Key::countIn,
                                                                              Prefs::countIn.fallback),
                                              Prefs::countIn),
                              juce::dontSendNotification);
    countInBox.onChange = [this] { plugins.settings().setValue (Prefs::Key::countIn, countInBox.getSelectedId()); };
    countInBox.setTooltip ("Counts you in with the metronome before recording starts. The song stays silent during the count.");

    clickButton.setToggleState (engine.isMetronomeOn(), juce::dontSendNotification);
    clickButton.onClick = [this]
    {
        engine.setMetronome (! engine.isMetronomeOn());
        clickButton.setToggleState (engine.isMetronomeOn(), juce::dontSendNotification);
    };

    punchButton.setColour (juce::TextButton::buttonOnColourId, Ahp::rec);
    punchButton.setTooltip ("Replaces what is inside the loop range when you record, and leaves "
                            "the rest of the take alone. Drag a loop range in the ruler first. "
                            "Recording rolls in a bar early so the join has something to fade from.");
    punchButton.onClick = [this]
    {
        project.punchEnabled = ! project.punchEnabled;
        refreshPunchButton();

        // Said once, here, rather than left for the record button to refuse:
        // the punch is set on the loop locators, so turning it on with no
        // range set is a reasonable thing to do in the wrong order and the
        // only unhelpful answer is silence.
        if (project.punchEnabled && ! project.hasLoopRange())
            setStatus ("Punch is on, but there is no loop range to punch. Drag one in the "
                       "playlist ruler and that becomes the punch in and punch out.");
        else if (project.punchEnabled)
            setStatus ("Punch is on. Recording will replace bars "
                         + barNumber (project.meter, project.loopStart) + " to "
                         + barNumber (project.meter, project.loopEnd)
                         + " on the armed track and leave the rest of the take as it is.");
        else
            setStatus ("Punch is off. Recording lays down a new take as it always has.");

        project.changed();
    };
    refreshPunchButton();

    soloAlertButton.setClickingTogglesState (false);
    soloAlertButton.setColour (juce::TextButton::buttonOnColourId, Ahp::rec);
    soloAlertButton.setTooltip ("Lights when something in the mixer is soloed. Click to clear every solo.");
    soloAlertButton.onClick = [this]
    {
        engine.clearAllSolos();
        mixer.refresh();
        refreshSoloAlert();
    };

    playlistTab.onClick = [this] { setView (View::playlist); };
    pianoTab.onClick    = [this] { setView (View::pianoRoll); };
    rackTab.onClick     = [this] { setView (View::rack); };
    modTab.onClick      = [this] { setView (View::modulators); };
    reportTab.onClick   = [this] { setView (View::mixReport); };
    mixerTab.onClick    = [this] { toggleMixerWindow(); };
    playlistTab.setTooltip ("F5");
    pianoTab.setTooltip ("F7");
    rackTab.setTooltip ("Step sequencer for all 16 channels (F6)");
    modTab.setTooltip ("Move any plugin parameter in time, locked to the tempo (F8)");
    reportTab.setTooltip ("What the finished mix measures, and what that means");
    mixerTab.setTooltip ("Opens the mixer window (F9)");

    fileButton.onClick = [this] { showFileMenu(); };
    fileButton.setTooltip ("New, open, save (Ctrl+N, Ctrl+O, Ctrl+S)");
    recent.setMaxNumberOfItems (12);
    recent.restoreFromString (plugins.settings().getValue ("recentProjects"));

    undoButton.onClick = [this] { undoRedo (false); };
    redoButton.onClick = [this] { undoRedo (true); };
    undoButton.setTooltip ("Undo (Ctrl+Z). Unlimited steps.");
    redoButton.setTooltip ("Redo (Ctrl+Y or Ctrl+Shift+Z)");

    // Both of these open the same window, on the tab they are named after. They
    // stay as separate buttons because picking a device and scanning for plugins
    // are the two things someone does on a fresh install, and burying either one
    // behind a tab would cost a first run more than the tidiness is worth.
    audioButton.onClick   = [this] { showPreferences (PreferencesComponent::audio); };
    pluginsButton.onClick = [this] { showPreferences (PreferencesComponent::plugins); };
    audioButton.setTooltip ("Audio device and the monitoring latency it costs (part of Preferences, Ctrl+,)");
    pluginsButton.setTooltip ("Scan for VST3 and other plugins (part of Preferences, Ctrl+,)");

    tempo.setSliderStyle (juce::Slider::LinearBar);
    tempo.setRange (20.0, 400.0, 0.01);
    tempo.setNumDecimalPlacesToDisplay (2);
    tempo.setValue (storedPreference (plugins.settings(), "bpm", Prefs::defaultTempo),
                    juce::dontSendNotification);
    tempo.onValueChange = [this]
    {
        engine.setBpm (tempo.getValue());
        project.bpm = engine.getBpm();

        // Clips set to follow the tempo are re-stretched before the snapshot
        // is pushed, so matched audio stays on the grid instead of drifting
        // when the tempo moves.
        project.retuneTempoFollowers();

        pushArrangement (true);
        playlist.refresh();
        markDirty();
    };
    engine.setBpm (tempo.getValue());

    timeSigLabel.setColour (juce::Label::textColourId, Ahp::muted);
    timeSigField.setEditable (false, true, false);
    timeSigField.setJustificationType (juce::Justification::centred);
    timeSigField.setColour (juce::Label::outlineColourId, Ahp::line);
    timeSigField.setTooltip ("The time signature, as beats per bar over the note that is a beat: "
                             "4/4, 3/4, 6/8, 7/8, 12/8. The tempo is quarter notes per minute "
                             "whatever this says, so changing it moves the bar lines, the click "
                             "and the grid and does not move a note.");
    timeSigField.onTextChange = [this]
    {
        Meter::Signature typed;
        if (Meter::parse (timeSigField.getText().toStdString(), typed))
        {
            applyTimeSignature (typed);
        }
        else
        {
            // Put back what was there rather than guessing at what was meant.
            // "4/3" read as 4/4 would leave somebody looking at a signature
            // they did not type.
            showTimeSignature();
            setStatus ("A time signature is beats per bar over the note that is a beat, like 4/4 "
                       "or 6/8. The beat has to be a whole, half, quarter, eighth, sixteenth or "
                       "thirty second note, because those are the only ones a MIDI file can say.");
        }
    };

    // The same thing the tempo slider gets from storedPreference above: a
    // control that has never been written to is empty, and juce::Label fires
    // onTextChange only on a change, so clicking into the empty box and out of
    // it again leaves it empty. Without this the studio launches showing a
    // blank signature and keeps it until a project is opened.
    showTimeSignature();

    tempoLabel.setColour (juce::Label::textColourId, Ahp::muted);
    clock.setJustificationType (juce::Justification::centred);
    clock.setFont (juce::FontOptions (20.0f));
    clock.setColour (juce::Label::outlineColourId, Ahp::line);

    // ---- channel bar ----
    channelLabel.setColour (juce::Label::textColourId, Ahp::muted);
    insertLabel.setColour (juce::Label::textColourId, Ahp::muted);
    typingLabel.setColour (juce::Label::textColourId, Ahp::muted);
    typingLabel.setJustificationType (juce::Justification::centredRight);

    channelBox.onChange = [this] { selectChannel (channelBox.getSelectedId() - 1); };
    channelBox.setTooltip ("16 instrument channels. Live playing and MIDI recording use the selected one.");

    instrumentBox.setTextWhenNothingSelected ("Load an instrument");
    instrumentBox.setTextWhenNoChoicesAvailable ("No instruments yet. Scan in Plugins.");
    instrumentBox.onChange = [this]
    {
        const int index = instrumentBox.getSelectedId() - 1;
        if (juce::isPositiveAndBelow (index, instrumentTypes.size()))
            loadInstrument (instrumentTypes.getReference (index));
    };
    insertBox.onChange = [this]
    {
        const int ch = engine.getSelectedChannel();
        const int ins = insertBox.getSelectedId() - 1;
        project.channels[(size_t) ch].insert = ins;
        engine.setChannelInsert (ch, ins);
        mixer.refresh();
        markDirty();
    };
    insertBox.setTooltip ("Which mixer insert this channel plays through");

    browseButton.setTooltip ("Search your instruments by name");
    browseButton.onClick = [this]
    {
        juce::Component::SafePointer<MainComponent> safeThis (this);
        PluginPicker::show ("Load an instrument", instrumentTypes, [safeThis] (const juce::PluginDescription& d)
        {
            if (safeThis != nullptr)
                safeThis->loadInstrument (d);
        });
    };

    showButton.onClick   = [this] { if (auto* p = engine.getChannelPlugin (engine.getSelectedChannel())) openPluginWindow (*p); };
    sumOutsButton.setClickingTogglesState (true);
    sumOutsButton.setTooltip ("For instruments with several output buses (Microtonic Multi, multi-out drum plugins):\n"
                              "on = every output is mixed into this channel, off = only the first stereo output.");
    sumOutsButton.onClick = [this]
    {
        engine.setChannelSumsOutputs (engine.getSelectedChannel(), sumOutsButton.getToggleState());
        markDirty();
    };
    routeOutsButton.setTooltip ("Give each of this instrument's outputs its own mixer insert, "
                                "so every drum gets its own strip, fader and effects.");
    routeOutsButton.onClick = [this] { showBusRouting (engine.getSelectedChannel()); };

    unloadButton.onClick = [this] { unloadInstrument (engine.getSelectedChannel()); };

    typing.onOctaveChanged = [this] { updateTypingLabel(); };
    addKeyListener (&typing);

    // ---- piano ----
    piano.setAvailableRange (24, 108);
    piano.setOctaveForMiddleC (4);
    piano.clearKeyMappings();
    piano.setScrollButtonsVisible (false);
    piano.setVelocity (0.8f, true);
    piano.setColour (juce::MidiKeyboardComponent::whiteNoteColourId,           Ahp::bone);
    piano.setColour (juce::MidiKeyboardComponent::blackNoteColourId,           Ahp::panel2);
    piano.setColour (juce::MidiKeyboardComponent::keySeparatorLineColourId,    Ahp::muted);
    piano.setColour (juce::MidiKeyboardComponent::keyDownOverlayColourId,      Ahp::muted.withAlpha (0.85f));
    piano.setColour (juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, Ahp::muted.withAlpha (0.25f));
    piano.setColour (juce::MidiKeyboardComponent::textLabelColourId,           Ahp::black);
    piano.setColour (juce::MidiKeyboardComponent::shadowColourId,              juce::Colours::transparentBlack);

    // ---- views ----
    playlist.onInteraction   = [this] { grabKeyboardFocus(); };
    playlist.onPlayFrom      = [this] (double b) { playFrom (b); };
    playlist.onSetPosition   = [this] (double b) { setPosition (b); };
    playlist.onOpenPianoRoll = [this] (int id) { pianoRoll.setClip (id); if (auto* c = project.find (id)) selectChannel (c->channel); setView (View::pianoRoll); };
    playlist.isRendering     = [this] (const Clip& c)
    {
        return c.sample != nullptr && ! c.isTakeFolder() && StretchCache::needsRender (c.stretch, c.pitch)
            && stretchCache.isPending (*c.sample, c.stretch, c.pitch);
    };
    playlist.onRouteTrack    = [this] (int) { mixer.refresh(); };
    playlist.onStatus        = [this] (const juce::String& m) { setStatus (m); };

    // A dropped MIDI file adopts its own tempo only when the project is still
    // at the default, and it goes through the tempo control rather than
    // writing project.bpm, so the engine, the displayed number and every
    // tempo following clip all move together.
    playlist.onSetTempo      = [this] (double bpm)
    {
        tempo.setValue (bpm, juce::sendNotificationSync);
    };

    // And its signature, through the one path in, for the same reason.
    playlist.onSetTimeSignature = [this] (Meter::Signature s) { applyTimeSignature (s); };

    pianoRoll.onInteraction = [this] { grabKeyboardFocus(); };
    pianoRoll.onSetPosition = [this] (double b) { setPosition (b); };

    rack.onEdited         = [this] { markDirty(); };

    modPanel.onEdited           = [this] { markDirty(); };
    modPanel.getSelectedChannel = [this] { return engine.getSelectedChannel(); };
    rack.onSelectChannel  = [this] (int channel) { selectChannel (channel); grabKeyboardFocus(); };

    mixer.onInteraction = [this] { grabKeyboardFocus(); };
    mixer.onEdited      = [this] { markDirty(); };
    mixer.onSlotClicked = [this] (int insert, int slot) { showFxMenu (insert, slot); };
    mixer.removeFxCallback = [this] (int insert, int slot)
    {
        if (auto* p = engine.getFx (insert, slot))
            closePluginWindow (p);
        engine.setFx (insert, slot, nullptr);
        project.missingFx.erase (insert * kNumFxSlots + slot);
    };

    addAndMakeVisible (browser);
    addAndMakeVisible (playlist);
    addChildComponent (pianoRoll);
    addChildComponent (rack);
    addChildComponent (modPanel);
    addChildComponent (mixReport);
    addAndMakeVisible (logo);

    for (auto* c : { static_cast<juce::Component*> (&playButton), static_cast<juce::Component*> (&stopButton),
                     static_cast<juce::Component*> (&recordButton), static_cast<juce::Component*> (&recordMode), static_cast<juce::Component*> (&monitorBox), static_cast<juce::Component*> (&countInBox),
                     static_cast<juce::Component*> (&clickButton), static_cast<juce::Component*> (&punchButton),
                     static_cast<juce::Component*> (&soloAlertButton),
                     static_cast<juce::Component*> (&playlistTab),
                     static_cast<juce::Component*> (&pianoTab), static_cast<juce::Component*> (&rackTab),
                     static_cast<juce::Component*> (&modTab), static_cast<juce::Component*> (&reportTab),
                     static_cast<juce::Component*> (&mixerTab),
                     static_cast<juce::Component*> (&audioButton), static_cast<juce::Component*> (&pluginsButton),
                     static_cast<juce::Component*> (&undoButton), static_cast<juce::Component*> (&redoButton),
                     static_cast<juce::Component*> (&fileButton),
                     static_cast<juce::Component*> (&tempo), static_cast<juce::Component*> (&tempoLabel),
                     static_cast<juce::Component*> (&timeSigField), static_cast<juce::Component*> (&timeSigLabel),
                     static_cast<juce::Component*> (&clock), static_cast<juce::Component*> (&channelLabel),
                     static_cast<juce::Component*> (&channelBox), static_cast<juce::Component*> (&instrumentBox),
                     static_cast<juce::Component*> (&insertLabel), static_cast<juce::Component*> (&insertBox),
                     static_cast<juce::Component*> (&browseButton), static_cast<juce::Component*> (&showButton), static_cast<juce::Component*> (&unloadButton), static_cast<juce::Component*> (&sumOutsButton), static_cast<juce::Component*> (&routeOutsButton),
                     static_cast<juce::Component*> (&typingLabel), static_cast<juce::Component*> (&piano) })
    {
        addAndMakeVisible (c);
        c->setWantsKeyboardFocus (false);
        c->setMouseClickGrabsKeyboardFocus (false);
    }

    project.addChangeListener (this);
    plugins.knownPlugins.addChangeListener (this);

    refreshPluginLists();
    refreshChannelControls();
    pushArrangement (true);
    updateTypingLabel();
    setView (View::playlist);
    updateUndoButtons();

    // Everything the preferences window owns, applied the same way it applies a
    // change: one path from a stored setting to its effect.
    applyPreferences (plugins.settings(), preferenceActions());

    if (startupError.isNotEmpty())
        setStatus ("Audio device problem: " + startupError + ". Open Preferences (Ctrl+,) to pick another device.");

    // Crash recovery: if the last session didn't close cleanly, offer the autosave
    const bool crashedLastTime = plugins.settings().getBoolValue ("sessionOpen", false)
                              && ProjectIO::autosaveFile().existsAsFile();
    plugins.settings().setValue ("sessionOpen", true);
    plugins.settings().saveIfNeeded();
    if (crashedLastTime)
    {
        juce::Component::SafePointer<MainComponent> safeThis (this);
        juce::Timer::callAfterDelay (900, [safeThis] { if (safeThis != nullptr) safeThis->offerRecovery(); });
    }

    setWantsKeyboardFocus (true);
    setSize (1500, 920);
    // The same rate the autosave interval is counted in, so the two cannot
    // drift. See Prefs::mainTimerHz.
    startTimerHz ((int) Prefs::mainTimerHz);
}

MainComponent::~MainComponent()
{
    stopTimer();

    // Before anything else goes: it holds references to the plugin manager and
    // the device manager, which are members here.
    closeTunerWindow();
    closePreferencesWindow();

    if (recordingAudio || recordingMidi)
        finishRecording();

    removeKeyListener (&typing);
    project.removeChangeListener (this);
    plugins.knownPlugins.removeChangeListener (this);

    if (mixerWindow != nullptr)
    {
        plugins.settings().setValue ("mixerBounds", mixerWindow->getWindowStateAsString());
        mixerWindow.reset();
    }
    pluginWindows.clear();

    // Audio must stop reading clips before the samples go away
    engine.setSnapshot ({});
    engine.stopPreview();
    for (int c = 0; c < kNumChannels; ++c)
        engine.setChannelPlugin (c, nullptr);
    for (int i = 0; i < kNumInserts; ++i)
        for (int k = 0; k < kNumFxSlots; ++k)
            engine.setFx (i, k, nullptr);

    if (auto xml = engine.devices().createStateXml())
        plugins.settings().setValue ("audioDevice", xml.get());
    plugins.settings().setValue ("bpm", tempo.getValue());
    plugins.settings().setValue ("sessionOpen", false);
    plugins.settings().setValue ("recentProjects", recent.toString());
    plugins.settings().saveIfNeeded();
}

// ---------------------------------------------------------------------------
// Transport

void MainComponent::togglePlay()
{
    if (recordingAudio || recordingMidi)
    {
        stopAll();
        return;
    }
    engine.setPlaying (! engine.isPlaying());
}

void MainComponent::stopAll()
{
    const bool wasIdle = ! engine.isPlaying() && ! recordingAudio && ! recordingMidi;

    if (recordingAudio || recordingMidi)
        finishRecording();

    engine.stopAndRewind();

    if (wasIdle)
    {
        // Pressing Stop again is a panic button: silence anything still ringing,
        // reset the instruments themselves (for plugins that drone on regardless
        // of note-offs), and send the start marker back to the beginning.
        engine.panic();
        engine.resetInstruments();
        engine.setSongStart (0.0);
        engine.stopAndRewind();
        playlist.refresh();
        setStatus ("Stopped every note, reset the instruments, and moved the start back to the beginning.");
    }
    engine.keyboard().allNotesOff (0);
    engine.stopPreview();
    playlist.repaint();
}

void MainComponent::playFrom (double beat)
{
    if (recordingAudio || recordingMidi)
        return;
    engine.setSongStart (beat);
    if (engine.isPlaying())
    {
        engine.locate (beat);
    }
    else
    {
        engine.stopAndRewind();
        engine.setPlaying (true);
    }
    playlist.repaint();
}

void MainComponent::showTimeSignature()
{
    // dontSendNotification, or setting the text would re-enter onTextChange
    // and apply the signature a second time.
    timeSigField.setText (juce::String (Meter::toString (project.meter.at (0.0))),
                          juce::dontSendNotification);
}

void MainComponent::applyTimeSignature (const Meter::Signature& signature)
{
    // Typing the signature the project is already in is not an edit. The
    // field is still put back, so "04/4" normalises to "4/4", but nothing
    // else moves: without this, re-typing 4/4 in a 4/4 project re-pushed the
    // whole arrangement and marked the project dirty, so closing it offered
    // to save a project nothing had happened to.
    if (signature == project.meter.at (0.0))
    {
        showTimeSignature();
        return;
    }

    if (! project.meter.setFirst (signature))
        return;

    // The engine holds its own copy for the click and the play head, read
    // once a block on the audio thread. Both halves of this have to move
    // together or the click is in one signature and the grid in another.
    engine.setTimeSignature (project.meter.at (0.0));
    showTimeSignature();

    // The song end is where the last clip's bar finishes, so it moves with the
    // signature: pushArrangement is what hands the new figure to the engine,
    // and without it the transport would keep looping on the old bar line.
    pushArrangement (true);

    playlist.refresh();
    pianoRoll.repaint();

    // refresh rather than repaint, because the modulator panel's rate menu is
    // built there and a label in it reads "1 bar" only in a signature whose
    // bar is that many quarters. The rack is deliberately not refreshed: it
    // does not follow the signature at all, and the note on rackLengthBeats
    // in Project.h says why that is tonight's answer.
    modPanel.refresh();
    markDirty();
}

void MainComponent::setPosition (double beat)
{
    if (recordingAudio || recordingMidi)
        return;
    if (engine.isPlaying())
    {
        engine.locate (beat);
    }
    else
    {
        engine.setSongStart (beat);
        engine.stopAndRewind();
    }
    playlist.repaint();
}

// ---------------------------------------------------------------------------
// Recording

void MainComponent::toggleRecord()
{
    if (recordingAudio || recordingMidi)
    {
        stopAll();
        return;
    }

    const int channel = engine.getSelectedChannel();
    auto* instrument  = engine.getChannelPlugin (channel);
    auto* device      = engine.devices().getCurrentAudioDevice();
    const bool hasInput = device != nullptr && ! device->getActiveInputChannels().isZero();

    bool wantInput = false, wantInstrument = false, wantMidi = false;
    switch (recordMode.getSelectedId())
    {
        case recAudio:          wantInput = true; break;
        case recMidi:           wantMidi = true; break;
        case recBoth:           wantInput = true; wantMidi = true; break;
        case recInstrument:     wantInstrument = true; break;
        case recInstrumentMidi: wantInstrument = true; wantMidi = true; break;
        default:
            // Auto: an instrument records as MIDI, so it stays editable and the
            // sound can still be changed afterwards. Notes played inside the
            // plugin's own window are captured too, if it sends them out.
            if (instrument != nullptr) wantMidi = true;
            else                       wantInput = true;
            break;
    }

    if ((wantMidi || wantInstrument) && instrument == nullptr)
    {
        if (! wantInput)
        {
            setStatus ("Load an instrument on channel " + juce::String (channel + 1) + " first, or record your input instead.");
            return;
        }
        wantMidi = wantInstrument = false;
    }
    if (wantInput && ! hasInput)
    {
        if (! wantMidi && ! wantInstrument)
        {
            setStatus ("No input is switched on. Open Audio settings and enable an input channel.");
            return;
        }
        wantInput = false;
    }

    // Recording an input means recording the armed strips, so with none armed
    // there is nothing to capture and the record button would roll the
    // transport and produce nothing. Saying which button to press is more use
    // than a take that silently did not happen.
    if (wantInput && engine.countArmedInputs() == 0)
    {
        if (! wantMidi && ! wantInstrument)
        {
            setStatus ("No mixer strip is armed. Open the mixer, pick an input on a strip, "
                       "or right click a strip and choose Map inputs from here.");
            return;
        }
        wantInput = false;
    }

    int armed = project.firstArmedTrack();
    if (armed < 0)
    {
        armed = project.firstEmptyTrackFrom (0);
        project.tracks[(size_t) armed].armed = true;
    }
    const bool recordingSomeAudio = wantInput || wantInstrument;

    // Input recording takes one track per armed strip, so MIDI has to start
    // looking past all of them rather than past the first. Otherwise the MIDI
    // clip lands on the track the second microphone is about to use.
    const int audioTracksNeeded = wantInput ? std::max (1, engine.countArmedInputs()) : 1;

    audioTrack  = armed;
    midiTrack   = recordingSomeAudio ? project.firstEmptyTrackFrom (armed + audioTracksNeeded)
                                     : armed;
    midiChannel = channel;

    takeNotes.clear();
    heldNotes.clear();
    takeLanes.clear();

    // Bars of the signature, not of four beats: a two bar count-in in 3/4 is
    // six beats, which is what the player hears counted.
    const double countBeats = (countInBox.getSelectedId() - 1)
                                * project.meter.barLengthQuartersAt (project.loopStart);

    // Recording over a loop starts at the loop, not wherever the playhead
    // happened to be. Otherwise the first pass would be the odd one out: it
    // would start somewhere else and be a different length from the nine
    // after it, and nothing in the folder would line up.
    //
    // A punch is the exception, and it has to be. The crossfade at the punch
    // in sits in front of it and is read from the new pass, so starting the
    // transport exactly at the punch in leaves the join nothing to fade from
    // and the punch arrives as a butt join every time. Rolling in before it is
    // also what the player needs: Nuendo's own reason for punch recording is
    // that you "want to listen to what is already recorded up to the recording
    // start position".
    //
    // One bar, which is Logic's published figure: "Logic Pro will begin
    // recording approximately one bar before the punch-in locator", and what
    // it buys there is the same thing it buys here, that "the start point of
    // the region [can] be adjusted later". A bar rather than a number of
    // seconds because every other range in this transport is in beats and a
    // musician counts themselves in in bars, and a bar of the signature rather
    // than four quarters, which are the same thing only in 4/4.
    //
    // The range is latched here and read nowhere else for the rest of the
    // take, which is the discipline `CLAUDE.md` already states for the record
    // alignment figure, "the way Ardour refuses an alignment change while a
    // DiskWriter is running". The playlist ruler is live while the transport
    // rolls: drag the loop during a take and an unlatched punch would land the
    // performance over a stretch the player was not playing to, and switching
    // Punch off mid take would lay the pass over the old one instead and sum
    // with it, which is the fault this whole thing exists to remove.
    punchArmed = project.punchRange();

    if (Punch::isUsable (punchArmed))
        engine.setSongStart (std::max (0.0, project.loopStart - punchLeadInBeats (project.loopStart)));
    else if (project.hasLoopRange())
        engine.setSongStart (project.loopStart);

    engine.stopAndRewind();
    engine.setRecordSource (wantInstrument ? channel : -1);
    if (recordingSomeAudio)
        engine.startAudioRecording();
    if (wantMidi)
    {
        paramRecorder.reset();
        engine.startMidiRecording (channel);
        recordingPlugin = instrument;
        recordingPluginProducesMidi = instrument->producesMidi();
        recordingPlugin->addListener (&paramRecorder);
    }

    recordingAudio      = recordingSomeAudio;
    recordingInstrument = wantInstrument;
    recordingMidi       = wantMidi;

    if (countBeats > 0.0)
        engine.startWithCountIn (engine.getSongStart(), countBeats);
    else
        engine.setPlaying (true);
    project.changed();

    juce::String what = wantInstrument ? (wantMidi ? "this channel's sound, its notes and its knob moves" : "this channel's sound")
                      : wantInput      ? (wantMidi ? "your input and MIDI" : "your input")
                                       : "MIDI and automation";
    juce::String msg = countBeats > 0.0 ? "Counting in, then recording " + what + "..."
                                        : "Recording " + what + ". Press Space or Stop to finish.";
    if (Punch::isUsable (punchArmed) && recordingSomeAudio)
        msg << "  Punching in over the loop range: everything outside it is left alone. "
               "The transport will not loop, so this is one pass.";
    else if (project.hasLoopRange() && recordingSomeAudio)
        msg << "  Every time round the loop is kept as its own take.";
    setStatus (msg);
}

void MainComponent::refreshPunchButton()
{
    // Lit only when it is actually going to do something. Punch left on with
    // no loop range is not punching, and a button that says it is would be
    // lying about what the record button is about to do.
    punchButton.setToggleState (project.isPunching(), juce::dontSendNotification);
}

void MainComponent::collectRecordedMidi()
{
    std::vector<AudioEngine::RecordedMidi> events;
    engine.drainRecordedMidi (events);
    for (auto& ev : events)
    {
        const juce::MidiMessage m (ev.bytes, ev.size, 0.0);
        if (m.isNoteOn())
        {
            heldNotes[m.getNoteNumber()] = { ev.beat, m.getFloatVelocity() };
        }
        else if (m.isNoteOff())
        {
            auto it = heldNotes.find (m.getNoteNumber());
            if (it != heldNotes.end())
            {
                takeNotes.push_back ({ it->second.first, std::max (1.0 / 64.0, ev.beat - it->second.first),
                                       m.getNoteNumber(), it->second.second });
                heldNotes.erase (it);
            }
        }
    }

    std::vector<AudioEngine::RecordedParam> params;
    engine.drainRecordedParams (params);
    for (auto& p : params)
    {
        auto& lane = takeLanes[p.index];
        lane.paramIndex = p.index;
        if (! lane.points.empty() && std::abs (lane.points.back().beat - p.beat) < 1.0 / 192.0)
            lane.points.back().value = p.value;
        else
            lane.points.push_back ({ p.beat, p.value });
    }
}

void MainComponent::finishRecording()
{
    const double stopBeat = engine.getBeatPosition();

    // ---- MIDI + automation ----
    if (recordingMidi)
    {
        engine.stopMidiRecording();
        if (recordingPlugin != nullptr)
            recordingPlugin->removeListener (&paramRecorder);
        collectRecordedMidi();

        for (auto& [note, held] : heldNotes)
            takeNotes.push_back ({ held.first, std::max (1.0 / 64.0, stopBeat - held.first), note, held.second });
        heldNotes.clear();

        const double start = engine.getMidiRecordStartBeat();
        if (start >= 0.0 && (! takeNotes.empty() || ! takeLanes.empty()))
        {
            auto pattern = std::make_shared<MidiPattern>();
            for (auto n : takeNotes)
            {
                n.start -= start;
                if (n.start >= 0.0)
                    pattern->notes.push_back (n);
            }

            const auto* plugin = recordingPlugin;
            for (auto& [index, lane] : takeLanes)
            {
                AutoLane l;
                l.paramIndex = index;
                l.name = (plugin != nullptr && juce::isPositiveAndBelow (index, plugin->getParameters().size()))
                             ? plugin->getParameters()[index]->getName (40)
                             : "Parameter " + juce::String (index);
                for (auto p : lane.points)
                {
                    p.beat = std::max (0.0, p.beat - start);
                    l.points.push_back (p);
                }
                l.sort();
                pattern->lanes.push_back (std::move (l));
            }

            Clip clip;
            clip.type    = ClipType::midi;
            clip.pattern = pattern;
            clip.channel = midiChannel;
            clip.track   = midiTrack;
            clip.start   = start;
            // At least one bar, and a whole number of bars past the last
            // note, so a part recorded live sits on the grid.
            {
                const double needed = std::max (stopBeat - start, pattern->lastBeat());
                const double bar    = project.meter.barLengthQuartersAt (start);
                clip.length = std::max (bar, project.meter.roundUpToBarAt (needed, 1.0e-9));
            }
            const int id = project.addClip (clip);
            project.selection = { id };
            pianoRoll.setClip (id);

            juce::String message = "Recorded " + juce::String ((int) pattern->notes.size()) + " notes and "
                                 + juce::String ((int) pattern->lanes.size()) + " automation lanes onto "
                                 + project.tracks[(size_t) midiTrack].name + ". Double-click the clip to edit it.";
            if (pattern->notes.empty())
            {
                const bool announces = recordingPluginProducesMidi;
                message << (announces
                    ? "  (The plugin offers a MIDI output but sent nothing. Check its own MIDI settings.)"
                    : "  (This plugin has no MIDI output, so notes played inside its window can't be recorded. "
                      "Use Rec: Instrument sound to capture them as audio.)");
            }
            else if (engine.getPluginMidiCaptured() > 0)
            {
                message << "  (" + juce::String (engine.getPluginMidiCaptured()) + " of those came from the plugin itself.)";
            }
            if (pattern->lanes.empty() && ! paramRecorder.sawGesture.load())
                message << "  (This plugin doesn't tell the app when you grab a knob, so its moves weren't recorded.)";
            setStatus (message);
        }
        else
        {
            setStatus ("No MIDI was played, so no clip was made.");
        }

        takeNotes.clear();
        takeLanes.clear();
        recordingPlugin = nullptr;
        recordingMidi = false;
    }

    // ---- audio ----
    if (recordingAudio)
    {
        // Read before they are cleared. Reading wasInstrument after clearing
        // it is how the instrument branch below became unreachable once,
        // which lost the take and left the recorder running for the rest of
        // the session.
        const bool wasInstrument = recordingInstrument;
        recordingAudio = false;
        recordingInstrument = false;
        engine.setRecordSource (-1);

        if (wasInstrument)
        {
            finishAudioRecording (engine.stopInstrumentRecording(), audioTrack,
                                  project.channels[(size_t) midiChannel].name);
        }
        else
        {
            // One take per armed insert, each on its own playlist track, so
            // recording four players at once gives four tracks rather than
            // four clips in a heap.
            //
            // Each track is found empty rather than counted off from the
            // first. Stepping blindly would drop a second microphone onto a
            // track that already has clips, and worse, running past the last
            // track and clamping would append one player's passes into
            // another player's take folder, which is a comp: comping would
            // then crossfade between two different instruments.
            auto captured = engine.stopAudioRecording();
            int  track = audioTrack;

            // A capture with nothing left in it is reachable now in a way it
            // was not before: the alignment takes the round trip off the
            // front, so a stab at the record button that lasted less than the
            // round trip has no audio in it once the offset is removed.
            // Saying so beats a record button that appears to do nothing.
            if (captured.empty())
                appendStatus ("Nothing to keep: that take was shorter than the latency being "
                              "compensated for, so there was no audio left in it once the "
                              "offset came off the front.");

            for (auto& capture : captured)
            {
                if (! juce::isPositiveAndBelow (track, Project::numTracks))
                {
                    setStatus ("Ran out of playlist tracks, so "
                                 + juce::String ((int) captured.size()) + " inputs could not all "
                                   "be kept. The ones that fit were.");
                    break;
                }

                finishAudioRecording (std::move (capture.passes), track,
                                      engine.insert (capture.insert).name);

                // Asked again each time, because the clip just added makes
                // the track it landed on no longer empty.
                track = project.firstEmptyTrackFrom (track + 1);
            }
        }

        // The latch is released only now, after every take is down, so that
        // dragging the loop range the moment the transport stops cannot change
        // where the performance just played lands. It also puts the engine's
        // loop range back, which pushArrangement leaves alone while a punch is
        // armed.
        punchArmed = { 0.0, 0.0, false, false };
        pushArrangement (false);
    }

    playlist.refresh();
}

// Turns what the recorder captured into clips. One pass makes one audio clip,
// exactly as it always did. Several passes, which is what recording round a
// loop produces, make one take folder: they are alternatives for the same
// stretch of the song rather than a stack of clips on top of each other, and
// keeping them together is what makes comping possible at all.
//
// Each pass still gets its own file in Recordings, so nothing about this is a
// new way of storing audio, and a pass can be dragged out of the project and
// used elsewhere.
void MainComponent::finishAudioRecording (std::vector<Recorder::Take> passes,
                                          int track, const juce::String& sourceName)
{
    if (passes.empty())
        return;

    const double sampleRate = engine.getSampleRate();

    // How far into a pass its own start sits is the recorder's answer, not
    // one worked out here. It used to be worked out here, from the
    // interface's reported round trip, which is the right idea in the wrong
    // place: it left out the studio's own compensation, so a lookahead plugin
    // on the master pushed every take late again; it was clamped to half a
    // pass, so a short one was quietly half corrected; in a loop recording it
    // read past the head of each pass without moving the split points, so the
    // last of every performance but the final one was handed to the next pass
    // and skipped there, which threw it away; and sitting up here in the
    // window it had no test over it at all. See RecordAlign.h.

    // Named after whatever it came from, which with several inputs at once is
    // the difference between four files called Take and four that say which
    // microphone they were.
    const auto stem = sourceName.isNotEmpty() ? sourceName + " take " : juce::String ("Take ");
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S");

    int  dropped   = 0;
    int  unsaved   = 0;
    bool anyToDisk = false;

    std::vector<Take> takes;
    double folderStart = -1.0;

    // The clip a punch would land in, found before the passes are built rather
    // than after, because a punch needs the passes measured from somewhere
    // else and that cannot be worked out afterwards.
    //
    // Where a pass sits inside a folder is seconds from the folder's start, and
    // a folder's start is its clip's start beat. For an ordinary recording the
    // two are the same thing, because the clip is created at the first pass's
    // own position. A punch lands in a clip that is already somewhere, and the
    // transport rolled in a bar before the punch rather than at the clip, so
    // the pass's position inside that folder is measured from the clip and not
    // from the roll in. Measured from the wrong one, the audio is offset inside
    // the folder by however far apart the two are, and what the punch plays is
    // either silence or the wrong part of the performance, at full level, with
    // a clean crossfade either side of it. Nothing errors.
    double punchClipStart = 0.0;
    int    punchClipId    = -1;

    // Only the track this call is for, which with several inputs at once means
    // only the first player's. Each later input is given a track found empty,
    // so there is nothing on it to punch into and its pass lands as an
    // ordinary take. That is the right way round for the case somebody is
    // actually in, which is fixing one line of one part, and it is written
    // down because punching while tracking a band would otherwise be
    // surprising in a way nothing explains.
    if (Punch::isUsable (punchArmed))
    {
        if (auto* target = project.punchTargetOn (track, punchArmed))
        {
            punchClipId    = target->id;
            punchClipStart = target->start;
        }
    }

    std::vector<Take> punchPasses;

    for (size_t i = 0; i < passes.size(); ++i)
    {
        auto& pass = passes[i];
        dropped = std::max (dropped, pass.droppedFrames);

        const auto suffix = passes.size() > 1 ? " " + juce::String ((int) i + 1) : juce::String();
        const auto file   = BrowserPanel::recordingsFolder().getChildFile (stem + stamp + suffix + ".wav");

        if (writeWavFile (file, pass.audio, sampleRate))
            anyToDisk = true;
        else
            ++unsaved;

        auto sample = cache.adopt (std::move (pass.audio), sampleRate, file,
                                   file.getFileNameWithoutExtension());
        const double duration = sample->durationSeconds();

        // The pass's own start, which is after whatever of it was played
        // before the record point. That audio stays in the file so the left
        // edge has somewhere to be dragged back to.
        const double head = sampleRate > 0.0
                              ? std::min (std::max (0.0, (double) pass.preRollFrames / sampleRate), duration)
                              : 0.0;

        if (duration - head <= 0.01)
            continue;                      // a pass too short to be one

        if (folderStart < 0.0)
            folderStart = pass.startBeat;

        Take take;
        take.name   = "Take " + juce::String ((int) takes.size() + 1);
        take.sample = sample;
        take.offset = head;
        take.length = duration - head;

        // Where this pass sits inside the folder. Loop recording puts every
        // pass at the same place, and this is zero for all of them; a pass
        // that began later than the first is held back by the difference, so
        // a punch in does not slide to the front of the folder.
        take.start = std::max (0.0, (pass.startBeat - folderStart) * 60.0 / std::max (1.0, project.bpm));

        if (punchClipId >= 0)
        {
            // The same pass, placed from the clip's start instead of from the
            // roll in. The arithmetic is in Punch.h, where it is tested,
            // because getting it wrong here is silent: see Punch::placePass.
            const auto placed = Punch::placePass (pass.startBeat, punchClipStart,
                                                  std::max (1.0, project.bpm), head, duration);

            if (placed.valid && placed.length > 0.01)
            {
                Take p;
                p.name   = "Punch";
                p.sample = sample;
                p.start  = placed.start;
                p.offset = placed.offset;
                p.length = placed.length;
                punchPasses.push_back (std::move (p));
            }
        }

        takes.push_back (std::move (take));
    }

    if (takes.empty())
    {
        // Silent until now, and reachable: every pass can be shorter than the
        // offset being taken off its front, which is what a stab at the
        // record button produces. A record button that appears to do nothing
        // is worse than one that says why.
        appendStatus ("Nothing to keep from " + (sourceName.isNotEmpty() ? sourceName
                                                                        : juce::String ("that take"))
                        + ": it was shorter than the latency being compensated for.");
        return;
    }

    juce::String msg;

    // ---- the punch ----
    // Recording over something already on the track replaces what is inside
    // the punch and leaves the rest of the performance as it was, which is
    // what every studio's default does and what this one would not do before:
    // the take would have been laid over the old one and both would have
    // played at once. See Punch.h for the geometry and the sources.
    //
    // The passes are passed by copy rather than moved, because a refusal has
    // to leave the ordinary path below something to put down. Losing a
    // performance because a punch could not be made is not a trade worth
    // taking, and copying a take copies a name and a pointer rather than any
    // audio.
    if (punchClipId >= 0 && ! punchPasses.empty())
    {
        const auto refusal = project.punchTake (punchClipId, punchPasses, punchArmed);

        switch (refusal)
        {
            case Project::PunchRefusal::none:
            {
                msg = "Punched in over bars "
                        + barNumber (project.meter, punchArmed.inBeat) + " to "
                        + barNumber (project.meter, punchArmed.outBeat) + " on "
                        + project.tracks[(size_t) track].name
                        + ". The rest of the take is untouched: drag across a lane to move the "
                          "join, or back over the whole clip to put the original performance "
                          "back.";

                if (unsaved > 0)
                    msg << (anyToDisk ? "  (" + juce::String (unsaved) + " passes couldn't be written to disk.)"
                                      : "  (Kept in this session, but nothing could be written to disk.)");
                if (dropped > 0)
                    msg << "  (" << dropped << " samples dropped. Try a larger buffer size.)";

                browser.refresh();
                setStatus (msg);
                return;
            }

            case Project::PunchRefusal::stretched:
                // Said rather than worked around. A take folder reads its
                // passes directly, so it cannot carry warp markers or a
                // stretch ratio, and punching into a clip that has either
                // would change how the part nobody punched sounds.
                msg = "That clip is warped or stretched, so it can't be punched into without "
                      "changing the rest of it. The take is on the track as it is, above the "
                      "old one. ";
                break;

            case Project::PunchRefusal::alreadyComped:
                msg = "That clip is already comped, and a folder has one crossfade length for "
                      "all of its joins, so punching into it would change the joins you made. "
                      "The take is on the track as it is. ";
                break;

            case Project::PunchRefusal::tooManyTakes:
                msg = "That clip already holds as many passes as a folder can. The take is on "
                      "the track as it is. ";
                break;

            case Project::PunchRefusal::missedTheClip:
                msg = "The punch range and that clip do not overlap by enough to punch, or the "
                      "pass stopped before reaching it. The take is on the track as it is. ";
                break;

            case Project::PunchRefusal::noClip:
            case Project::PunchRefusal::notAudio:
                break;      // nothing to punch into: an ordinary take, below
        }
    }

    if (takes.size() == 1)
    {
        // One pass is one clip, the way it has always been. Wrapping a single
        // take in a folder would mean every ordinary recording came back as
        // something that needs comping.
        Clip clip;
        clip.type   = ClipType::audio;
        clip.sample = takes[0].sample;
        clip.track  = track;
        clip.start  = folderStart;
        clip.offset = takes[0].offset;
        clip.length = takes[0].length;
        project.selection.insert (project.addClip (clip));
        msg << (unsaved == 0 ? "Take saved to " + takes[0].sample->file.getFullPathName()
                             : juce::String ("Take kept in this session, but it couldn't be saved to disk."));
    }
    else
    {
        // Another round of passes over the same loop joins the folder already
        // there rather than burying it, which is what makes "keep going until
        // one of them is right" work.
        const double totalSeconds = [&takes]
        {
            double longest = 0.0;
            for (const auto& t : takes)
                longest = std::max (longest, t.start + t.length);
            return longest;
        }();

        int id = 0;
        if (auto* existing = project.takeFolderAt (track, folderStart, totalSeconds))
        {
            id = existing->id;
            for (auto& t : takes)
                project.addTake (id, std::move (t));
        }
        else
        {
            id = project.addTakeFolder (track, folderStart, std::move (takes));
        }

        project.selection = { id };
        const auto* folder = project.folderFor (id);
        const int   count  = folder != nullptr ? folder->size() : 0;

        msg << juce::String (count) + " takes in a folder on "
            + project.tracks[(size_t) track].name
            + ". Drag across a lane to comp from that take.";
    }

    if (unsaved > 0 && takes.size() > 1)
        msg << (anyToDisk ? "  (" + juce::String (unsaved) + " of them couldn't be written to disk.)"
                          : "  (Kept in this session, but nothing could be written to disk.)");
    if (dropped > 0)
        msg << "  (" << dropped << " samples dropped. Try a larger buffer size.)";

    browser.refresh();
    setStatus (msg);
}

// ---------------------------------------------------------------------------
// Arrangement

void MainComponent::pushArrangement (bool allowRenders)
{
    project.bpm = engine.getBpm();

    AudioEngine::Snapshot snap;
    std::set<juce::String> keysInUse;

    for (const auto& c : project.clips)
    {
        const auto& track = project.tracks[(size_t) c.track];
        if (c.muted || track.muted)
            continue;

        // The clip's own two edges, worked out once here and handed to every
        // entry the clip produces. It is built from the clip's span on the
        // arrangement rather than from the audio behind it, because a fade is
        // a gesture on the timeline: a stretched clip's fade lasts as long as
        // it was drawn to, and a warped clip's de-click fade does not get
        // bent by the markers. Only the ramps become beats, the same
        // conversion and the same reason as the comp's.
        const ClipFade::Window edgeFade
            = ClipFade::windowFor (c.start, c.endBeat (project.bpm),
                                   project.resolvedFadeFor (c),
                                   project.bpm / 60.0);

        if (c.isTakeFolder())
        {
            // One comped stretch becomes one entry, so the renderer plays a
            // comp with the code it already has for playing audio, and the
            // offline export inherits it without a second implementation:
            // renderOffline calls renderAudioClips over this same snapshot.
            // That is the only way to be sure a bounce sounds like what was
            // heard, which for a comp matters more than for anything else,
            // because the comp is the performance.
            const double secondsPerBeat = 60.0 / std::max (1.0, project.bpm);
            const float  gain = juce::Decibels::decibelsToGain (c.gainDb);

            for (const auto& span : c.compSpans())
            {
                const auto* take = c.folder->take (span.take);
                if (take == nullptr || take->sample == nullptr || ! span.isAudible()
                    || take->sample->audio.getNumSamples() == 0
                    || take->sample->audio.getNumChannels() == 0)
                    continue;

                AudioEngine::AudioClipRT rt;
                rt.insert     = track.insert;
                rt.data       = take->sample.get();
                rt.gain       = gain;
                rt.rate       = 1.0;
                rt.start      = c.start + span.readFrom / secondsPerBeat;
                rt.readOffset = take->offset + (span.readFrom - take->start);
                rt.playLength = span.readTo - span.readFrom;

                // The ramps are the only thing converted into beats, because
                // beats are what the renderer has per sample. The comp itself
                // stays in seconds so that changing the tempo moves the
                // folder on the grid without moving the joins through the
                // performance.
                const auto f = span.fade();
                rt.fade = { c.start + f.inFrom   / secondsPerBeat,
                            c.start + f.inTo     / secondsPerBeat,
                            c.start + f.outFrom  / secondsPerBeat,
                            c.start + f.outTo    / secondsPerBeat };
                // Every comped stretch carries the same window, so the two
                // outer edges of the folder ramp and the stretches between
                // them are outside both ramps. One caveat worth knowing: the
                // comp spans are clamped to each take's own audio, so if the
                // first span starts after the folder's own start, or its take
                // has gone missing, the fade in ramps across nothing and the
                // first audible sample gets none of it. The folder's edges
                // fade where the comp reaches them, which is almost always.
                rt.edgeFade = edgeFade;

                snap.audio.push_back (rt);
            }
        }
        else if (c.isAudio())
        {
            if (c.sample == nullptr || c.sample->audio.getNumSamples() == 0 || c.sample->audio.getNumChannels() == 0)
                continue;   // missing file

            AudioEngine::AudioClipRT rt;
            rt.insert   = track.insert;
            rt.start    = c.start;
            rt.gain     = juce::Decibels::decibelsToGain (c.gainDb);
            rt.edgeFade = edgeFade;

            // A warped clip reads along the piecewise line its markers
            // describe. Which audio that line is measured against depends on
            // whether the stretched copy is ready, so it is worked out below
            // and the segments are built from it.
            double     warpScale = 1.0;
            const bool warped    = c.isWarped();

            if (StretchCache::needsRender (c.stretch, c.pitch))
            {
                keysInUse.insert (StretchCache::keyFor (*c.sample, c.stretch, c.pitch));
                if (auto rendered = stretchCache.get (c.sample, c.stretch, c.pitch, allowRenders))
                {
                    rt.data       = rendered.get();
                    rt.readOffset = c.offset * c.stretch;
                    rt.playLength = c.length * c.stretch;
                    rt.rate       = 1.0;

                    // The markers describe positions in the file, and this is a
                    // copy already stretched by the clip's ratio, so the line
                    // has to be measured in the copy's own seconds.
                    warpScale = c.stretch;
                }
                else
                {
                    // quick preview until the proper render is ready
                    rt.data       = c.sample.get();
                    rt.readOffset = c.offset;
                    rt.playLength = c.length * c.stretch;
                    rt.rate       = 1.0 / c.stretch;
                }
            }
            else
            {
                rt.data       = c.sample.get();
                rt.readOffset = c.offset;
                rt.playLength = c.length;
                rt.rate       = 1.0;
            }

            if (warped)
            {
                const double beats = c.lengthBeats (project.bpm);
                rt.playLength = beats * 60.0 / project.bpm;
                rt.warpFirst  = (int) snap.warp.size();
                buildWarpSegments (c.warp, c.offset, c.fallbackSlope (project.bpm), c.start,
                                   snap.warp, warpScale);
                rt.warpCount  = (int) snap.warp.size() - rt.warpFirst;
            }

            snap.audio.push_back (rt);
        }
        else if (c.pattern != nullptr)
        {
            AudioEngine::MidiClipRT m;
            m.channel = juce::jlimit (0, kNumChannels - 1, c.channel);
            m.start   = c.start;
            m.end     = c.start + c.length;
            const double origin = c.start - c.offset;

            for (const auto& n : c.pattern->notes)
            {
                // Groove is applied here rather than to the stored notes, so
                // it can be changed or turned off without having lost the
                // original timing, and rather than in the audio callback, so
                // it costs nothing per sample and an export inherits it
                // without a second code path.
                const double placed = project.groove.place (n.start, n.note);
                const double on = origin + placed;

                if (on < m.start || on >= m.end)
                    continue;

                const float shaded = project.groove.shade (n.start, n.velocity);

                AudioEngine::NoteRT note;
                note.on       = on;
                note.off      = std::min (on + n.length, m.end);
                note.note     = n.note;
                note.velocity = (juce::uint8) juce::jlimit (1, 127, juce::roundToInt (shaded * 127.0f));
                m.notes.push_back (note);
            }

            for (const auto& lane : c.pattern->lanes)
            {
                AudioEngine::LaneRT l;
                l.paramIndex = lane.paramIndex;
                l.lane = lane;
                for (auto& p : l.lane.points)
                    p.beat += origin;
                m.lanes.push_back (std::move (l));
            }
            snap.midi.push_back (std::move (m));
        }
        else if (c.isAutomation() && c.curve != nullptr)
        {
            AudioEngine::AutoCurveRT a;
            a.target = c.curve->target;
            a.start  = c.start;
            a.end    = c.start + c.length;

            // Shifted into absolute beats here, as the in-clip lanes above
            // are, so the audio thread never has to know where a clip starts.
            // Adding the same number to every beat cannot reorder them, so the
            // list stays in the form the lookup needs without re-sorting.
            const double origin = c.start - c.offset;
            a.points = c.curve->points;
            for (auto& p : a.points)
                p.beat += origin;

            snap.automation.push_back (std::move (a));
        }
    }

    snap.songEnd    = project.songLoopEndBeats();
    snap.modulators = project.modulators;      // small, so copied whole
    engine.setSnapshot (std::move (snap));

    // A punch does not loop, and that is a decision with a published precedent
    // rather than a limitation nobody thought about. Every pass after the first
    // begins exactly at the punch in, because that is where the transport wraps
    // to, so it has no lead in and the crossfade in front of the punch has
    // nothing of the new pass to fade from: the join can only be a butt join,
    // which is the one thing `CompModel.h` says a comp boundary must not be.
    // Pro Tools makes the same call the same way round, listing "Loop playback
    // not available" as a cost of its punch modes.
    //
    // Cubase and Logic do allow the pair, and each pass punching the same span
    // into a folder of takes is the better end state. It needs a lead in for
    // every pass rather than only the first, which is a change to the recorder
    // rather than to the punch, and it is written into the backlog as the next
    // step rather than half done here.
    if (project.hasLoopRange() && ! Punch::isUsable (punchArmed))
        engine.setLoopRange (project.loopStart, project.loopEnd);
    else
        engine.clearLoopRange();
    stretchCache.keepOnly (keysInUse);
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &project)
    {
        const bool editing = playlist.isEditing() || pianoRoll.isDragging() || rack.isDragging();
        pushArrangement (! editing);
        if (! editing)
        {
            if (project.commit())    // a finished edit becomes one undo step
                markDirty();
            updateUndoButtons();
        }
    
        // The loop range is saved with the project but is deliberately not
        // part of the undo history, so commit() above never notices it. It
        // still has to make the project worth saving, or moving the loop and
        // closing the window would quietly lose it.
        if (project.loopStart != lastLoopStart || project.loopEnd != lastLoopEnd
            || project.punchEnabled != lastPunchEnabled)
        {
            lastLoopStart    = project.loopStart;
            lastLoopEnd      = project.loopEnd;
            lastPunchEnabled = project.punchEnabled;
            markDirty();

            // The punch is the loop range, so clearing the range with a click
            // in the ruler turns the punch off in effect. The button has to
            // follow it or it would still be lit over a punch that cannot
            // happen.
            refreshPunchButton();
        }

        playlist.refresh();
        if (view == View::pianoRoll) pianoRoll.refresh();
        if (view == View::rack)      rack.refresh();
        if (view == View::modulators) modPanel.refresh();
        if (mixerWindow != nullptr)  mixer.refresh();
        return;
    }
    refreshPluginLists();
}

// ---------------------------------------------------------------------------
// Channels, instruments, effects

void MainComponent::refreshPluginLists()
{
    instrumentTypes.clearQuick();
    effectTypes.clearQuick();
    for (const auto& d : plugins.knownPlugins.getTypes())
        (d.isInstrument ? instrumentTypes : effectTypes).add (d);

    auto byName = [] (const juce::PluginDescription& a, const juce::PluginDescription& b)
    {
        const int m = a.manufacturerName.compareNatural (b.manufacturerName);
        return m != 0 ? m < 0 : a.name.compareNatural (b.name) < 0;
    };
    std::sort (instrumentTypes.begin(), instrumentTypes.end(), byName);
    std::sort (effectTypes.begin(), effectTypes.end(), byName);

    refreshChannelControls();
}

void MainComponent::refreshChannelControls()
{
    const int ch = engine.getSelectedChannel();

    channelBox.clear (juce::dontSendNotification);
    for (int i = 0; i < kNumChannels; ++i)
    {
        const auto& info = project.channels[(size_t) i];
        channelBox.addItem (juce::String (i + 1) + "   " + (info.name.isNotEmpty() ? info.name : juce::String ("(empty)")), i + 1);
    }
    channelBox.setSelectedId (ch + 1, juce::dontSendNotification);

    instrumentBox.clear (juce::dontSendNotification);
    for (int i = 0; i < instrumentTypes.size(); ++i)
        instrumentBox.addItem (instrumentTypes[i].name + "   (" + instrumentTypes[i].manufacturerName + ")", i + 1);
    if (auto* p = engine.getChannelPlugin (ch))
    {
        const auto id = p->getPluginDescription().createIdentifierString();
        for (int i = 0; i < instrumentTypes.size(); ++i)
            if (instrumentTypes[i].createIdentifierString() == id)
                instrumentBox.setSelectedId (i + 1, juce::dontSendNotification);
    }

    insertBox.clear (juce::dontSendNotification);
    for (int i = 0; i < kNumInserts; ++i)
        insertBox.addItem (engine.insert (i).name, i + 1);
    insertBox.setSelectedId (project.channels[(size_t) ch].insert + 1, juce::dontSendNotification);

    const bool loaded = engine.getChannelPlugin (ch) != nullptr;
    showButton.setEnabled (loaded);
    unloadButton.setEnabled (loaded);
    const bool multiOut = loaded && engine.getChannelOutputBuses (ch) > 1;
    const bool split = engine.getChannelSplitsBuses (ch);

    sumOutsButton.setEnabled (multiOut && ! split);
    sumOutsButton.setToggleState (engine.getChannelSumsOutputs (ch), juce::dontSendNotification);
    routeOutsButton.setEnabled (multiOut);
    routeOutsButton.setButtonText (split ? "Outs: split" : "Route outs...");
    instrumentBox.setEnabled (loadingName.isEmpty());
}

void MainComponent::selectChannel (int channel)
{
    engine.keyboard().allNotesOff (0);
    engine.setSelectedChannel (channel);
    refreshChannelControls();
}

void MainComponent::loadInstrument (const juce::PluginDescription& description)
{
    const int channel = engine.getSelectedChannel();
    if ((recordingMidi || recordingAudio) && channel == midiChannel)
        stopAll();

    if (auto* old = engine.getChannelPlugin (channel))
        closePluginWindow (old);
    engine.setChannelPlugin (channel, nullptr);
    project.channels[(size_t) channel].name.clear();
    project.channels[(size_t) channel].missingPlugin.reset();

    loadingName = description.name;
    setStatus ("Loading " + loadingName + "...");
    refreshChannelControls();

    juce::Component::SafePointer<MainComponent> safeThis (this);
    const auto name = description.name;
    plugins.formats.createPluginInstanceAsync (description, engine.getSampleRate(), engine.getBlockSize(),
        [safeThis, name, channel] (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error)
        {
            if (safeThis == nullptr)
                return;
            safeThis->loadingName.clear();

            if (instance == nullptr)
            {
                safeThis->refreshChannelControls();
                juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                                  .withIconType (juce::MessageBoxIconType::WarningIcon)
                                                  .withTitle ("Couldn't load " + name)
                                                  .withMessage (error.isNotEmpty() ? error : juce::String ("The plugin didn't start."))
                                                  .withButton ("OK"),
                                              nullptr);
                return;
            }

            auto* raw = instance.get();
            raw->addListener (&safeThis->editWatcher);
            safeThis->engine.setChannelPlugin (channel, std::move (instance));
            safeThis->markDirty();
            safeThis->project.channels[(size_t) channel].name = name;
            safeThis->refreshChannelControls();
            safeThis->openPluginWindow (*raw);

            if (! raw->producesMidi())
                safeThis->setStatus (name + " doesn't send MIDI out, so notes you play inside its own window can't be "
                                            "recorded as notes. Everything you play from a keyboard is fine, and "
                                            "\"Rec: Instrument sound\" captures the rest as audio.");

            const int buses = safeThis->engine.getChannelOutputBuses (channel);
            if (buses > 1)
                safeThis->setStatus (name + " has " + juce::String (buses) + " output buses, so they're all mixed into channel "
                                     + juce::String (channel + 1) + ". Turn off \"Mix all outs\" to hear only its first output."
                                     + " If it plays its own patterns, press Play and it follows the tempo.");
            else
                safeThis->setStatus (name + " is on channel " + juce::String (channel + 1) + ". Play it from your controller, the piano, or your computer keys.");
            safeThis->project.changed();
        });
}

void MainComponent::unloadInstrument (int channel)
{
    if ((recordingMidi || recordingAudio) && channel == midiChannel)
        stopAll();
    engine.panic();
    if (auto* p = engine.getChannelPlugin (channel))
        closePluginWindow (p);
    engine.setChannelPlugin (channel, nullptr);
    project.channels[(size_t) channel].name.clear();
    project.channels[(size_t) channel].missingPlugin.reset();
    refreshChannelControls();
    project.changed();
    markDirty();
}

void MainComponent::showFxMenu (int insertIndex, int slot)
{
    auto* current = engine.getFx (insertIndex, slot);
    const auto insertName = engine.insert (insertIndex).name;

    juce::Component::SafePointer<MainComponent> safeThis (this);
    auto pick = [safeThis, insertIndex, slot, insertName]
    {
        if (safeThis == nullptr)
            return;
        PluginPicker::show ("Effect for " + insertName + ", slot " + juce::String (slot + 1),
                            safeThis->effectTypes,
                            [safeThis, insertIndex, slot] (const juce::PluginDescription& d)
                            {
                                if (safeThis != nullptr)
                                    safeThis->loadFx (insertIndex, slot, d);
                            });
    };

    if (current == nullptr)
    {
        // An empty slot goes straight to the search window
        if (effectTypes.isEmpty())
        {
            setStatus ("No effects have been scanned yet. Open Plugins and scan your VST3 folder.");
            return;
        }
        pick();
        return;
    }

    juce::PopupMenu menu;
    menu.addSectionHeader (current->getName());
    menu.addItem (1, "Open window");
    menu.addItem (2, engine.insert (insertIndex).bypass[(size_t) slot].load() ? "Turn back on" : "Bypass");
    menu.addItem (4, "Replace...");
    menu.addSeparator();
    menu.addItem (3, "Remove");

    menu.showMenuAsync (juce::PopupMenu::Options(), [safeThis, insertIndex, slot, pick] (int result)
    {
        if (safeThis == nullptr || result == 0)
            return;
        auto& self = *safeThis;

        if (result == 1)
        {
            if (auto* p = self.engine.getFx (insertIndex, slot))
                self.openPluginWindow (*p);
        }
        else if (result == 2)
        {
            auto& b = self.engine.insert (insertIndex).bypass[(size_t) slot];
            self.engine.setFxBypass (insertIndex, slot, ! b.load());
            self.markDirty();
        }
        else if (result == 3)
        {
            if (auto* p = self.engine.getFx (insertIndex, slot))
                self.closePluginWindow (p);
            self.engine.setFx (insertIndex, slot, nullptr);
            self.project.missingFx.erase (insertIndex * kNumFxSlots + slot);
            self.markDirty();
        }
        else if (result == 4)
        {
            pick();
            return;
        }
        self.mixer.refresh();
    });
}

void MainComponent::loadFx (int insertIndex, int slot, const juce::PluginDescription& description)
{
    setStatus ("Loading " + description.name + "...");
    juce::Component::SafePointer<MainComponent> safeThis (this);
    const auto name = description.name;

    plugins.formats.createPluginInstanceAsync (description, engine.getSampleRate(), engine.getBlockSize(),
        [safeThis, name, insertIndex, slot] (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error)
        {
            if (safeThis == nullptr)
                return;
            auto& self = *safeThis;

            if (instance == nullptr)
            {
                juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                                  .withIconType (juce::MessageBoxIconType::WarningIcon)
                                                  .withTitle ("Couldn't load " + name)
                                                  .withMessage (error.isNotEmpty() ? error : juce::String ("The plugin didn't start."))
                                                  .withButton ("OK"),
                                              nullptr);
                return;
            }

            if (auto* old = self.engine.getFx (insertIndex, slot))
                self.closePluginWindow (old);

            auto* raw = instance.get();
            raw->addListener (&self.editWatcher);
            self.engine.setFx (insertIndex, slot, std::move (instance));
            self.project.missingFx.erase (insertIndex * kNumFxSlots + slot);
            self.markDirty();
            self.mixer.refresh();
            self.openPluginWindow (*raw);
            self.setStatus (name + " added to " + self.engine.insert (insertIndex).name + ".");
        });
}

void MainComponent::openPluginWindow (juce::AudioPluginInstance& plugin)
{
    if (auto it = pluginWindows.find (&plugin); it != pluginWindows.end())
    {
        it->second->setVisible (true);
        it->second->toFront (true);
        return;
    }

    juce::Component::SafePointer<MainComponent> safeThis (this);
    juce::AudioProcessor* key = &plugin;
    auto window = std::make_unique<PluginWindow> (plugin, [safeThis, key]
    {
        juce::MessageManager::callAsync ([safeThis, key]
        {
            if (safeThis != nullptr)
            {
                safeThis->closePluginWindow (key);
                safeThis->grabKeyboardFocus();
            }
        });
    });
    window->addKeyListener (&typing);
    window->setAlwaysOnTop (windowsFloat);
    pluginWindows[key] = std::move (window);
}

void MainComponent::closePluginWindow (juce::AudioProcessor* plugin)
{
    pluginWindows.erase (plugin);
}

/** Keeps the mixer and every plugin editor above the main window.

    They are separate top level windows, so by default clicking the playlist
    puts the main window over them. That is wrong for a mixer deliberately
    opened over the arrangement, and worse for a plugin editor opened from an
    insert: it can land behind everything, which reads as the plugin having
    failed to open rather than as a window in the wrong place.

    They float only while this application is the one in front. Pinning them
    on top permanently would leave them hovering over the browser or the file
    manager as soon as you switched away, which is a more annoying bug than
    the one being fixed here.

    Windows cannot be given an owner after they are created, which is the
    other way to get this behaviour, so this is done by following the
    foreground process instead. Checked on the existing timer rather than with
    one of its own, and only acted on when the answer changes, since setting
    the flag sixty times a second would make the windows flicker.
*/
void MainComponent::updateFloatingWindows()
{
    // Dropped while a dialog is up, because a floating window beats a plain
    // one in the stack whatever opened it, and a plugin editor sitting over
    // the plugin chooser or a "save changes?" box would be its own bug. This
    // only covers dialogs this program puts up: a native file chooser is the
    // operating system's window, not ours to reason about.
    const bool shouldFloat = juce::Process::isForegroundProcess()
                          && juce::Component::getCurrentlyModalComponent() == nullptr;

    if (shouldFloat == windowsFloat)
        return;

    windowsFloat = shouldFloat;

    if (mixerWindow != nullptr)
        mixerWindow->setAlwaysOnTop (shouldFloat);

    for (auto& entry : pluginWindows)
        entry.second->setAlwaysOnTop (shouldFloat);
}

// ---------------------------------------------------------------------------
// Views, keys, dialogs

void MainComponent::undoRedo (bool redo)
{
    if (recordingAudio || recordingMidi)
    {
        setStatus ("Stop recording before undoing.");
        return;
    }
    if (playlist.isEditing() || pianoRoll.isDragging())
        return;

    const bool done = redo ? project.redo() : project.undo();
    if (done)
    {
        markDirty();
        engine.keyboard().allNotesOff (0);
        setStatus (juce::String (redo ? "Redone." : "Undone.") + "   "
                   + juce::String ((int) project.undoSteps()) + " steps to undo, "
                   + juce::String ((int) project.redoSteps()) + " to redo.");
    }
    else
    {
        setStatus (redo ? "Nothing to redo." : "Nothing to undo.");
    }
    updateUndoButtons();
}

void MainComponent::updateUndoButtons()
{
    undoButton.setEnabled (project.undoSteps() > 0);
    redoButton.setEnabled (project.redoSteps() > 0);
}

void MainComponent::toggleMixerWindow()
{
    if (mixerWindow != nullptr)
    {
        plugins.settings().setValue ("mixerBounds", mixerWindow->getWindowStateAsString());
        mixerWindow.reset();
        mixerTab.setToggleState (false, juce::dontSendNotification);
        grabKeyboardFocus();
        return;
    }

    mixer.refresh();
    juce::Component::SafePointer<MainComponent> safeThis (this);
    mixerWindow = std::make_unique<MixerWindow> (mixer, [safeThis]
    {
        if (safeThis != nullptr)
            juce::MessageManager::callAsync ([safeThis] { if (safeThis != nullptr) safeThis->toggleMixerWindow(); });
    });

    const auto saved = plugins.settings().getValue ("mixerBounds");
    if (saved.isNotEmpty())
        mixerWindow->restoreWindowStateFromString (saved);
    Ahp::applyDarkTitleBar (*mixerWindow);
    mixerWindow->setAlwaysOnTop (windowsFloat);
    mixerWindow->toFront (true);
    mixerTab.setToggleState (true, juce::dontSendNotification);
}

void MainComponent::checkModulationLearn()
{
    if (! modPanel.isLearning())
        return;

    juce::AudioProcessor* processor = nullptr;
    int index = -1;

    if (! editWatcher.takeGrab (processor, index))
        return;

    // Work out which channel the grabbed plugin belongs to. Only instruments
    // can be modulated at the moment, so an effect's knob is ignored.
    for (int channel = 0; channel < kNumChannels; ++channel)
        if (engine.getChannelPlugin (channel) == processor)
        {
            juce::String paramName = "Parameter " + juce::String (index + 1);
            const auto& params = processor->getParameters();

            if (juce::isPositiveAndBelow (index, params.size()))
            {
                if (! params[index]->isAutomatable())
                {
                    setStatus ("That control cannot be automated, so it cannot be modulated either.");
                    return;
                }
                paramName = params[index]->getName (24);
            }

            modPanel.parameterTouched (channel, index, paramName);
            setStatus ("Modulating " + paramName + " on channel " + juce::String (channel + 1));
            return;
        }

    setStatus ("Modulation can only be attached to an instrument's own controls.");
}

void MainComponent::showBusRouting (int channel)
{
    if (! juce::isPositiveAndBelow (channel, kNumChannels))
        return;

    const int buses = std::min (engine.getChannelOutputBuses (channel), kMaxOutBuses);
    if (buses <= 1)
        return;

    const bool split = engine.getChannelSplitsBuses (channel);

    juce::PopupMenu menu;
    menu.addSectionHeader (project.channels[(size_t) channel].name + "   "
                             + juce::String (buses) + " outputs");

    menu.addItem (1, "Mix every output into one insert", true, ! split);
    menu.addItem (2, "Give each output its own insert", true, split);
    menu.addSeparator();

    // The useful default: lay the outputs out across consecutive inserts
    // starting at this channel's own, which is how a drum machine's individual
    // outs would normally be patched.
    menu.addItem (3, "Spread across inserts from here");

    if (split)
    {
        menu.addSeparator();
        menu.addSectionHeader ("Where each output goes");

        for (int b = 0; b < buses; ++b)
        {
            juce::PopupMenu destinations;
            const int current = engine.getBusInsert (channel, b);

            destinations.addItem (1000 + b * 100, "Same as the channel", true, current == 0);
            for (int to = 1; to < kNumInserts; ++to)
                destinations.addItem (1000 + b * 100 + to, engine.insert (to).name,
                                      true, current == to);

            menu.addSubMenu (engine.getBusName (channel, b)
                               + (current > 0 ? "   " + engine.insert (current).name
                                              : juce::String()),
                             destinations);
        }
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&routeOutsButton),
                        [this, channel, buses] (int result)
    {
        if (result == 0)
            return;

        if (result == 1)
        {
            engine.setChannelSplitsBuses (channel, false);
        }
        else if (result == 2)
        {
            engine.setChannelSplitsBuses (channel, true);
        }
        else if (result == 3)
        {
            const int first = project.channels[(size_t) channel].insert;

            for (int b = 0; b < buses; ++b)
            {
                const int to = first + b;
                engine.setBusInsert (channel, b, to < kNumInserts ? to : 0);
            }

            engine.setChannelSplitsBuses (channel, true);
            setStatus ("Outputs spread across inserts " + juce::String (first)
                         + " to " + juce::String (std::min (first + buses - 1, kNumInserts - 1)));
        }
        else if (result >= 1000)
        {
            const int bus = (result - 1000) / 100;
            const int to  = (result - 1000) % 100;
            engine.setBusInsert (channel, bus, to);
        }

        // The routing is the engine's, so the project's copy is brought in line
        // with it before anything is saved.
        auto& info = project.channels[(size_t) channel];
        info.splitBuses = engine.getChannelSplitsBuses (channel);
        for (int b = 0; b < kMaxOutBuses; ++b)
            info.busInsert[(size_t) b] = engine.getBusInsert (channel, b);

        markDirty();
        refreshChannelControls();
        mixer.refresh();
    });
}

void MainComponent::setView (View v)
{
    view = v;
    playlist.setVisible (v == View::playlist);
    pianoRoll.setVisible (v == View::pianoRoll);
    rack.setVisible (v == View::rack);
    modPanel.setVisible (v == View::modulators);
    mixReport.setVisible (v == View::mixReport);
    playlistTab.setToggleState (v == View::playlist, juce::dontSendNotification);
    pianoTab.setToggleState (v == View::pianoRoll, juce::dontSendNotification);
    rackTab.setToggleState (v == View::rack, juce::dontSendNotification);
    modTab.setToggleState (v == View::modulators, juce::dontSendNotification);
    reportTab.setToggleState (v == View::mixReport, juce::dontSendNotification);

    if (v == View::rack)
        rack.refresh();
    if (v == View::modulators)
        modPanel.refresh();

    if (v == View::pianoRoll)
    {
        if (pianoRoll.getClipId() == 0 || project.find (pianoRoll.getClipId()) == nullptr)
            for (auto& c : project.clips)
                if (c.isMidi() && project.selection.count (c.id))
                    pianoRoll.setClip (c.id);
        pianoRoll.refresh();
    }
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    const auto cmd = juce::ModifierKeys::commandModifier;
    auto is = [&key] (char c, int mods) { return key == juce::KeyPress (c, mods, 0); };

    if (key == juce::KeyPress::spaceKey)                { togglePlay(); return true; }
    if (key.getKeyCode() == juce::KeyPress::F5Key)      { setView (View::playlist); return true; }
    if (key.getKeyCode() == juce::KeyPress::F6Key)      { setView (View::rack); return true; }
    if (key.getKeyCode() == juce::KeyPress::F7Key)      { setView (View::pianoRoll); return true; }
    if (key.getKeyCode() == juce::KeyPress::F8Key)      { setView (View::modulators); return true; }
    if (key.getKeyCode() == juce::KeyPress::F9Key)      { toggleMixerWindow(); return true; }
    if (is ('r', cmd))                                  { toggleRecord(); return true; }
    if (is ('z', cmd))                                  { undoRedo (false); return true; }
    if (is ('s', cmd))                                  { saveCurrent(); return true; }
    if (is ('s', cmd | juce::ModifierKeys::shiftModifier)) { saveAs (false); return true; }
    if (is ('o', cmd))                                  { openProjectDialog(); return true; }
    if (is ('n', cmd))                                  { newProject(); return true; }
    if (is (',', cmd))                                  { showPreferences (PreferencesComponent::general); return true; }
    if (is ('t', cmd))                                  { showTuner(); return true; }
    if (is ('y', cmd) || is ('z', cmd | juce::ModifierKeys::shiftModifier)) { undoRedo (true); return true; }

    if (view == View::playlist)
    {
        if (key.getKeyCode() == juce::KeyPress::deleteKey
            || key.getKeyCode() == juce::KeyPress::backspaceKey) { playlist.deleteSelection(); return true; }
        if (is ('b', cmd)) { playlist.duplicateSelection(); return true; }
        if (is ('c', cmd)) { playlist.copySelection(); setStatus ("Copied."); return true; }
        if (is ('x', cmd)) { playlist.cutSelection(); return true; }
        if (is ('v', cmd)) { playlist.paste(); return true; }
        if (is ('a', cmd)) { playlist.selectAll(); return true; }
        if (key.getKeyCode() == juce::KeyPress::escapeKey) { project.selection.clear(); playlist.refresh(); return true; }
    }
    return false;
}

void MainComponent::focusLost (FocusChangeType)
{
    typing.allNotesOff();
}

void MainComponent::mouseDown (const juce::MouseEvent&)
{
    grabKeyboardFocus();
}

void MainComponent::updateTypingLabel()
{
    typingLabel.setText ("Keys: Z to /  and  Q to P   |   lowest note "
                             + juce::MidiMessage::getMidiNoteName (typing.getBaseNote(), true, true, 4)
                             + "   |   Up/Down change octave",
                         juce::dontSendNotification);
}

PreferencesActions MainComponent::preferenceActions()
{
    // SafePointer rather than this, because the preferences window outlives
    // nothing here in practice but is owned by the desktop rather than by this
    // component, and a setting changed while the window is closing must not
    // reach a component that has gone.
    juce::Component::SafePointer<MainComponent> safeThis (this);

    PreferencesActions actions;

    actions.setMetronomeGain = [safeThis] (float gain)
    {
        if (safeThis != nullptr)
            safeThis->engine.setMetronomeGain (gain);
    };

    actions.setAutosaveTicks = [safeThis] (int ticks)
    {
        if (safeThis == nullptr)
            return;

        safeThis->autosaveEvery = ticks;

        // So that shortening the interval does not have to wait out the old
        // one, and lengthening it does not fire immediately.
        safeThis->autosaveTicks = 0;
    };

    actions.setDefaultTempo = [safeThis] (double bpm)
    {
        if (safeThis != nullptr)
            safeThis->project.startingBpm = bpm;
    };

    actions.setRecordTrimMs = [safeThis] (double ms)
    {
        if (safeThis != nullptr)
            safeThis->engine.setRecordTrimMs (ms);
    };

    actions.setSoloSettings = [safeThis] (float muteGain, bool overridesMute, bool exclusive)
    {
        if (safeThis != nullptr)
            safeThis->engine.setSoloSettings (muteGain, overridesMute, exclusive);
    };

    actions.recordOffsetSamples = [safeThis]() -> int
    {
        return safeThis != nullptr ? safeThis->engine.getRecordOffsetSamples() : 0;
    };

    actions.findFastestDevice = [safeThis]() -> juce::String
    {
        if (safeThis == nullptr)
            return {};

        const auto result = safeThis->engine.findFastestDevice();

        // The status bar gets one line, since the full record of what was
        // tried belongs in the window that asked for it. The one thing it
        // must carry is the sound card having been taken, because that is a
        // change to the whole machine and somebody who misses it will hear it
        // as a second fault.
        // The first line of the report either way, so the status bar cannot
        // contradict the window: when no device would open, that is what both
        // of them say. The sound card being held is added to it rather than
        // replacing it, because that is a change to the whole machine and
        // somebody who misses it will hear it as a second fault.
        auto headline = result.upToFirstOccurrenceOf ("\n", false, false);

        if (safeThis->engine.hasTakenTheDevice())
            headline << " The sound card is held by the studio, so other applications "
                        "will not play through it while this is open.";

        safeThis->setStatus (headline);
        return result;
    };

    return actions;
}

void MainComponent::showTuner()
{
    // The reference pitch lives in the application's settings rather than in
    // the project. A tuning reference belongs to the instruments in the room
    // rather than to one song: somebody who works at 432 works at 432 on
    // everything they open, and somebody who opens one of their projects
    // should not find their own tuner moved by it.
    showTunerWindow (engine, plugins.settings());
}

void MainComponent::showPreferences (PreferencesComponent::Tab tab)
{
    showPreferencesWindow (plugins, engine.devices(), preferenceActions(), tab);
}

void MainComponent::setStatus (const juce::String& message)
{
    statusMessage = message;
    statusTime = juce::Time::currentTimeMillis();
    repaint (statusBar);
}

void MainComponent::appendStatus (const juce::String& message)
{
    // Stopping a recording can have two things to report: what the MIDI half
    // made and what the audio half did not. Replacing the first with the
    // second loses the clip that was actually created, which is the half the
    // producer has to go and look at.
    const bool fresh = statusMessage.isEmpty()
                         || juce::Time::currentTimeMillis() - statusTime > 200;

    setStatus (fresh ? message : statusMessage + "  " + message);
}

// ---------------------------------------------------------------------------
// Layout and drawing

void MainComponent::resized()
{
    auto area  = getLocalBounds();
    topBar     = area.removeFromTop (52);
    channelBar = area.removeFromTop (42);
    statusBar  = area.removeFromBottom (26);
    pianoArea  = area.removeFromBottom (80);
    workArea   = area;

    auto bar = topBar.reduced (10, 10);
    logo.setBounds (topBar.getX() + 8, topBar.getY() + 3, 38, topBar.getHeight() - 6);

    // The right hand buttons are taken off first, before the left chain has a
    // chance to eat the bar. Taken afterwards, as they were, a left chain
    // wider than the window leaves them a rectangle of negative width and
    // they land on top of each other. The top bar is over subscribed at the
    // smallest window this will open at, so that is not hypothetical: this
    // way the overflow clips the middle of the bar, which is recoverable by
    // widening the window, rather than destroying the buttons at the end.
    pluginsButton.setBounds (bar.removeFromRight (68)); bar.removeFromRight (6);
    // In the right hand chain, which is laid out before the left one, so this
    // cannot be the button that falls off the end of a bar too narrow for
    // everything on it. An audit measured the bar as already over subscribed
    // at the default window size before solo was added, which is in the
    // backlog; what matters here is that the one control whose whole job is to
    // be noticed is not the one that disappears.
    soloAlertButton.setBounds (bar.removeFromRight (40)); bar.removeFromRight (10);
    audioButton  .setBounds (bar.removeFromRight (66)); bar.removeFromRight (14);
    redoButton   .setBounds (bar.removeFromRight (54)); bar.removeFromRight (4);
    undoButton   .setBounds (bar.removeFromRight (54)); bar.removeFromRight (10);

    bar.removeFromLeft (40 + 132);                          // logo + wordmark
    fileButton  .setBounds (bar.removeFromLeft (48));  bar.removeFromLeft (10);
    playButton  .setBounds (bar.removeFromLeft (58));  bar.removeFromLeft (4);
    stopButton  .setBounds (bar.removeFromLeft (58));  bar.removeFromLeft (4);
    recordButton.setBounds (bar.removeFromLeft (50));  bar.removeFromLeft (4);
    recordMode  .setBounds (bar.removeFromLeft (132)); bar.removeFromLeft (4);
    monitorBox  .setBounds (bar.removeFromLeft (116)); bar.removeFromLeft (4);
    countInBox  .setBounds (bar.removeFromLeft (104)); bar.removeFromLeft (8);
    clock       .setBounds (bar.removeFromLeft (80));  bar.removeFromLeft (8);
    tempoLabel  .setBounds (bar.removeFromLeft (42));
    tempo       .setBounds (bar.removeFromLeft (72));  bar.removeFromLeft (6);
    timeSigLabel.setBounds (bar.removeFromLeft (26));
    timeSigField.setBounds (bar.removeFromLeft (52));  bar.removeFromLeft (6);
    clickButton .setBounds (bar.removeFromLeft (48));  bar.removeFromLeft (4);
    punchButton .setBounds (bar.removeFromLeft (58));  bar.removeFromLeft (14);
    playlistTab .setBounds (bar.removeFromLeft (72));
    pianoTab    .setBounds (bar.removeFromLeft (80));
    rackTab     .setBounds (bar.removeFromLeft (52));
    modTab      .setBounds (bar.removeFromLeft (48));
    reportTab   .setBounds (bar.removeFromLeft (64));
    mixerTab    .setBounds (bar.removeFromLeft (58));

    auto cb = channelBar.reduced (10, 7);
    channelLabel .setBounds (cb.removeFromLeft (62));
    channelBox   .setBounds (cb.removeFromLeft (190)); cb.removeFromLeft (6);
    instrumentBox.setBounds (cb.removeFromLeft (238)); cb.removeFromLeft (4);
    browseButton .setBounds (cb.removeFromLeft (62));  cb.removeFromLeft (6);
    showButton   .setBounds (cb.removeFromLeft (58));  cb.removeFromLeft (4);
    unloadButton .setBounds (cb.removeFromLeft (66));  cb.removeFromLeft (6);
    sumOutsButton.setBounds (cb.removeFromLeft (100)); cb.removeFromLeft (6);
    routeOutsButton.setBounds (cb.removeFromLeft (104)); cb.removeFromLeft (14);
    insertLabel  .setBounds (cb.removeFromLeft (46));
    insertBox    .setBounds (cb.removeFromLeft (130)); cb.removeFromLeft (12);
    typingLabel  .setBounds (cb);

    auto work = workArea;
    browser.setBounds (work.removeFromLeft (260));
    playlist.setBounds (work);
    pianoRoll.setBounds (work);
    rack.setBounds (work);
    modPanel.setBounds (work);
    mixReport.setBounds (work);

    piano.setBounds (pianoArea);
    piano.setKeyWidth ((float) pianoArea.getWidth() / 50.0f);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (Ahp::black);

    g.setColour (Ahp::bone);
    g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)).withExtraKerningFactor (0.28f));
    g.drawText ("ALLHAILPAN", juce::Rectangle<int> (topBar.getX() + 52, topBar.getY(), 132, topBar.getHeight()),
                juce::Justification::centredLeft);

    g.setColour (Ahp::line);
    g.fillRect (topBar.withTop (topBar.getBottom() - 1));
    g.fillRect (channelBar.withTop (channelBar.getBottom() - 1));
    g.fillRect (pianoArea.withHeight (1));

    paintStatus (g, statusBar);
}

void MainComponent::paintStatus (juce::Graphics& g, juce::Rectangle<int> r)
{
    g.setColour (Ahp::line);
    g.fillRect (r.withHeight (1));

    auto text = r.reduced (12, 0);
    juce::String info = "No audio device";
    if (auto* d = engine.devices().getCurrentAudioDevice())
    {
        const double sr      = d->getCurrentSampleRate();
        const int    latency = d->getInputLatencyInSamples() + d->getOutputLatencyInSamples();
        const int    plugins = engine.getPluginLatencySamples();

        info = d->getTypeName() + "  |  " + d->getName()
             + "  |  " + juce::String ((int) sr) + " Hz"
             + "  |  " + juce::String (d->getCurrentBufferSizeSamples()) + " samples"
             + "  |  " + juce::String (sr > 0 ? 1000.0 * latency / sr : 0.0, 1) + " ms round trip"
             // Only worth mentioning when a plugin is actually holding things
             // back; it is the amount every track is delayed to stay in time.
             + (plugins > 0 ? "  |  " + juce::String (sr > 0 ? 1000.0 * plugins / sr : 0.0, 1) + " ms compensated"
                            : juce::String())
             + (engine.isMonitoring() ? juce::String ("  |  MONITORING") : juce::String())
             + "  |  CPU " + juce::String (engine.devices().getCpuUsage() * 100.0, 1) + "%"
             + (stretchCache.isBusy() ? juce::String ("  |  stretching audio...") : juce::String());
    }

    auto drawMeter = [&g] (juce::Rectangle<int> m, float level)
    {
        g.setColour (Ahp::panel3);
        g.fillRect (m);
        const float norm = juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (level, -60.0f) + 60.0f) / 60.0f);
        g.setColour (level >= 0.99f ? Ahp::rec : Ahp::bone);
        g.fillRect (m.withWidth ((int) ((float) m.getWidth() * norm)));
    };

    g.setFont (juce::FontOptions (12.0f));
    auto& master = engine.insert (0);
    drawMeter (text.removeFromRight (110).reduced (0, 9), std::max (master.peakL.load(), master.peakR.load()));
    text.removeFromRight (6);
    g.setColour (Ahp::muted);
    g.drawText ("Out", text.removeFromRight (28), juce::Justification::centredRight);
    text.removeFromRight (14);
    drawMeter (text.removeFromRight (110).reduced (0, 9), engine.getInputLevel());
    text.removeFromRight (6);
    g.setColour (Ahp::muted);
    g.drawText ("In", text.removeFromRight (20), juce::Justification::centredRight);

    const bool recording = recordingAudio || recordingMidi;
    const bool showMessage = statusMessage.isNotEmpty()
                          && (recording || juce::Time::currentTimeMillis() - statusTime < 8000);
    if (showMessage)
        g.setColour (recording ? Ahp::rec : Ahp::bone);
    g.drawText (showMessage ? statusMessage : info, text, juce::Justification::centredLeft);
}

void MainComponent::refreshSoloAlert()
{
    const bool soloing = engine.isAnySoloed();

    // Sixty times a second, so it does nothing at all unless the answer has
    // moved. setToggleState would repaint on every tick otherwise.
    if (soloAlertButton.getToggleState() != soloing)
        soloAlertButton.setToggleState (soloing, juce::dontSendNotification);
}

void MainComponent::timerCallback()
{
    if (! focusGrabbed && isShowing())
    {
        grabKeyboardFocus();
        focusGrabbed = true;
        updateTitle();
    }

    if (editWatcher.touched.exchange (false) && juce::Time::getMillisecondCounter() > ignoreEditsUntil)
        markDirty();

    // The mixer's input menus belong to whichever device is open, and the
    // device can change from the preferences window or from the driver
    // search, neither of which comes back through here. Comparing the channel
    // count is cheap and catches every way it can happen, including an
    // interface being unplugged.
    if (const int channels = engine.getInputChannelCount(); channels != lastInputChannelCount)
    {
        lastInputChannelCount = channels;
        mixer.refreshInputs();
    }

    checkModulationLearn();
    updateFloatingWindows();
    refreshSoloAlert();

    // Zero is off. Counting only while it is on, so turning the autosave off
    // does not leave a counter climbing towards a save that never comes.
    if (autosaveEvery > 0 && ++autosaveTicks >= autosaveEvery)
    {
        autosaveTicks = 0;
        if (dirty && ! recordingAudio && ! recordingMidi && ! playlist.isEditing() && ! pianoRoll.isDragging())
            autosave();
    }

    if (recordingMidi)
        collectRecordedMidi();

    checkExportProgress();

    if (engine.isCountingIn())
    {
        // The transport counts quarters and the metronome counts display
        // beats, so the number the player is hearing is neither: a two bar
        // count-in in 6/8 is six quarters and twelve clicks. `Meter` does the
        // conversion and `MeterTest` pins it, because a figure worked out
        // here is a figure nothing tests. Read at the loop start, which is
        // where the count-in's own length was measured, so the two cannot
        // disagree the night a signature can change part way through a song.
        const long long clicksLeft = Meter::clicksRemaining (project.meter.at (project.loopStart),
                                                             engine.countInBeatsLeft());
        clock.setText (juce::String (std::max<long long> (1, clicksLeft)),
                       juce::dontSendNotification);
        clock.setColour (juce::Label::textColourId, Ahp::rec);
    }
    else
    {
        clock.setText (formatPosition (project.meter, engine.getBeatPosition()), juce::dontSendNotification);
        clock.setColour (juce::Label::textColourId, Ahp::bone);
    }
    recordButton.setToggleState (recordingAudio || recordingMidi, juce::dontSendNotification);
    playButton.setToggleState (engine.isPlaying(), juce::dontSendNotification);

    if (view == View::playlist)  playlist.updatePlayhead();
    if (view == View::pianoRoll) pianoRoll.updatePlayhead();
    if (view == View::rack)      rack.updatePlayhead();
    if (mixerWindow != nullptr)  mixer.updateMeters();
    repaint (statusBar);
}

// ---------------------------------------------------------------------------
// Projects

juce::File MainComponent::projectsFolder()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                   .getChildFile ("ALLHAILPAN Studio").getChildFile ("Projects");
    dir.createDirectory();
    return dir;
}

void MainComponent::showFileMenu()
{
    auto item = [] (int id, const juce::String& text, const juce::String& keys, bool enabled = true)
    {
        juce::PopupMenu::Item i (text);
        i.itemID = id;
        i.shortcutKeyDescription = keys;
        i.isEnabled = enabled;
        return i;
    };

    juce::PopupMenu recentMenu;
    recent.createPopupMenuItems (recentMenu, 100, false, true);

    const auto missing = ProjectIO::missingSampleNames (project);

    bool anyMidi = false, anyMidiSelected = false;
    for (const auto& c : project.clips)
    {
        if (c.isAudio() || c.pattern == nullptr || c.pattern->notes.empty())
            continue;
        anyMidi = true;
        if (project.selection.count (c.id) != 0)
            anyMidiSelected = true;
    }

    juce::PopupMenu menu;
    menu.addItem (item (1, "New project", "Ctrl+N"));
    menu.addItem (item (2, "Open...", "Ctrl+O"));
    menu.addSubMenu ("Open recent", recentMenu, recentMenu.getNumItems() > 0);
    menu.addSeparator();
    menu.addItem (item (3, "Save", "Ctrl+S"));
    menu.addItem (item (4, "Save as...", "Ctrl+Shift+S"));
    menu.addItem (item (5, "Save with samples (copies audio beside the project)", {}));
    menu.addSeparator();
    menu.addItem (item (8, "Export song to WAV...", "", project.songEndBeats() > 0.0));
    menu.addItem (item (9, "Export stems (one file per mixer insert)...", "", project.songEndBeats() > 0.0));
    menu.addSeparator();
    menu.addItem (item (10, "Import MIDI file...", {}));
    menu.addItem (item (11, "Export MIDI (notes, one track per channel)...", {}, anyMidi));
    menu.addItem (item (12, "Export selected clips as MIDI...", {}, anyMidiSelected));
    menu.addSeparator();
    menu.addItem (item (6, "Find missing samples" + (missing.isEmpty() ? juce::String() : " (" + juce::String (missing.size()) + ")"),
                        {}, ! missing.isEmpty()));
    menu.addItem (item (7, "Show project in folder", {}, currentFile.existsAsFile()));
    menu.addSeparator();
    menu.addItem (item (14, "Tuner...", "Ctrl+T"));
    menu.addItem (item (13, "Preferences...", "Ctrl+,"));

    juce::Component::SafePointer<MainComponent> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&fileButton), [safeThis] (int result)
    {
        if (safeThis == nullptr || result == 0)
            return;
        auto& self = *safeThis;

        if (result >= 100)
        {
            const auto file = self.recent.getFile (result - 100);
            self.confirmDiscard ([safeThis, file]
            {
                if (safeThis != nullptr)
                    safeThis->loadProject (file);
            });
            return;
        }

        switch (result)
        {
            case 1: self.newProject(); break;
            case 2: self.openProjectDialog(); break;
            case 3: self.saveCurrent(); break;
            case 4: self.saveAs (false); break;
            case 5:
                if (self.currentFile == juce::File()) self.saveAs (true);
                else                                  self.saveProject (self.currentFile, true);
                break;
            case 6: self.findMissingSamples(); break;
            case 8: self.exportAudio (false); break;
            case 9: self.exportAudio (true); break;
            case 7: self.currentFile.revealToUser(); break;
            case 10: self.importMidiDialog(); break;
            case 11: self.exportMidi (false); break;
            case 12: self.exportMidi (true); break;
            case 13: self.showPreferences (PreferencesComponent::general); break;
            case 14: self.showTuner(); break;
            default: break;
        }
    });
}

void MainComponent::markDirty()
{
    if (! dirty)
    {
        dirty = true;
        updateTitle();
    }
}

void MainComponent::updateTitle()
{
    const auto name = currentFile == juce::File() ? juce::String ("Untitled") : currentFile.getFileNameWithoutExtension();
    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
        window->setName (name + (dirty ? " *" : "") + "  -  ALLHAILPAN Studio");
}

std::unique_ptr<juce::AudioPluginInstance> MainComponent::createPluginForProject (const juce::PluginDescription& saved,
                                                                                  juce::String& error)
{
    // Prefer this computer's scanned copy, which may live at a different path
    juce::PluginDescription description = saved;
    bool known = false;
    if (auto match = plugins.knownPlugins.getTypeForIdentifierString (saved.createIdentifierString()))
    {
        description = *match;
        known = true;
    }
    else
    {
        const auto types = plugins.knownPlugins.getTypes();
        for (int pass = 0; pass < 2 && ! known; ++pass)
            for (const auto& t : types)
                if (t.name == saved.name && t.manufacturerName == saved.manufacturerName
                    && (pass == 1 || t.pluginFormatName == saved.pluginFormatName))
                {
                    description = t;
                    known = true;
                    break;
                }
    }

    if (! known && ! juce::File (saved.fileOrIdentifier).exists())
    {
        error = "not installed on this computer (or not scanned yet)";
        return {};
    }

    auto instance = plugins.formats.createPluginInstance (description, engine.getSampleRate(), engine.getBlockSize(), error);
    if (instance == nullptr && error.isEmpty())
        error = "it didn't start";
    if (instance != nullptr)
        instance->addListener (&editWatcher);
    return instance;
}

void MainComponent::clearSession()
{
    if (recordingAudio || recordingMidi)
        finishRecording();
    engine.stopAndRewind();
    engine.panic();
    engine.stopPreview();
    engine.keyboard().allNotesOff (0);

    pluginWindows.clear();
    engine.setSnapshot ({});
    pianoRoll.setClip (0);
    project.selection.clear();
}

void MainComponent::newProject()
{
    juce::Component::SafePointer<MainComponent> safeThis (this);
    confirmDiscard ([safeThis]
    {
        if (safeThis == nullptr) return;
        auto& self = *safeThis;

        self.clearSession();
        ProjectIO::resetEngine (self.engine);
        self.project.clearAll();

        // clearAll put the tempo back to the default; this is what carries
        // that to the engine, the displayed number and any tempo following
        // clip, the same way a tempo change typed in by hand does.
        self.tempo.setValue (self.project.bpm, juce::sendNotificationSync);

        // And the signature, which clearAll put back to 4/4. The engine has
        // its own copy for the click and the play head, so a new project left
        // on the engine's old signature would click in sevens over an empty
        // 4/4 timeline.
        self.applyTimeSignature (self.project.meter.at (0.0));

        self.engine.setSongStart (0.0);
        self.engine.stopAndRewind();

        self.currentFile = juce::File();
        self.dirty = false;
        self.noteLoopRange();
        self.selectChannel (0);
        self.mixer.selectInsert (0);
        self.pushArrangement (true);
        self.playlist.refresh();
        self.updateUndoButtons();
        self.updateTitle();
        self.ignoreEditsForAWhile();
        self.setStatus ("New project.");
    });
}

void MainComponent::openProjectDialog()
{
    juce::Component::SafePointer<MainComponent> safeThis (this);
    confirmDiscard ([safeThis]
    {
        if (safeThis == nullptr) return;
        auto& self = *safeThis;

        const auto start = self.currentFile.existsAsFile() ? self.currentFile.getParentDirectory() : projectsFolder();
        self.chooser = std::make_unique<juce::FileChooser> ("Open a project", start, ProjectIO::wildcard);
        self.chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                   [safeThis] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            if (safeThis != nullptr && file.existsAsFile())
                safeThis->loadProject (file);
        });
    });
}

bool MainComponent::loadProject (const juce::File& file, bool recovered)
{
    clearSession();
    setStatus ("Opening " + file.getFileNameWithoutExtension() + "...");

    ProjectIO::LoadReport report;
    const auto result = ProjectIO::load (file, project, engine, cache,
                                         [this] (const juce::PluginDescription& d, juce::String& e) { return createPluginForProject (d, e); },
                                         report);

    ignoreEditsForAWhile();
    tempo.setValue (report.bpm, juce::dontSendNotification);

    // ProjectIO::load put the signature into the project; this is what carries
    // it to the engine and the field. Not through applyTimeSignature, which
    // marks the project dirty, and opening a file is not an edit to it.
    engine.setTimeSignature (project.meter.at (0.0));
    showTimeSignature();

    // The rate menu's labels are built per row from the bar length, so a 12/8
    // project opened while the Modulators view is the one on screen would keep
    // the previous project's "1 bar" against four quarters. Switching views
    // refreshes it, which is why this hides until somebody is looking at it.
    modPanel.refresh();

    engine.stopAndRewind();

    if (result.failed())
    {
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::WarningIcon)
                                          .withTitle ("Couldn't open the project")
                                          .withMessage (result.getErrorMessage())
                                          .withButton ("OK"),
                                      nullptr);
        currentFile = juce::File();
        dirty = false;
        noteLoopRange();
    }
    else if (recovered)
    {
        currentFile = juce::File();
        dirty = true;
        setStatus ("Recovered your autosaved work. Save it with Ctrl+S.");
    }
    else
    {
        currentFile = file;
        dirty = false;
        noteLoopRange();
        recent.addFile (file);
        plugins.settings().setValue ("recentProjects", recent.toString());
        setStatus ("Opened " + file.getFullPathName());
    }

    selectChannel (0);
    mixer.selectInsert (0);
    pushArrangement (true);
    playlist.refresh();
    updateUndoButtons();
    updateTitle();

    if (result.wasOk() && (! report.missingPlugins.isEmpty() || ! report.missingSamples.isEmpty()))
        showLoadProblems (report);

    return result.wasOk();
}

void MainComponent::showLoadProblems (const ProjectIO::LoadReport& report)
{
    juce::String message;
    if (! report.missingPlugins.isEmpty())
    {
        message << "These plugins didn't load. Their settings are kept in the project, so saving won't lose them:\n\n"
                << report.missingPlugins.joinIntoString ("\n") << "\n\n";
    }
    if (! report.missingSamples.isEmpty())
    {
        juce::StringArray shown;
        for (int i = 0; i < std::min (12, report.missingSamples.size()); ++i)
            shown.add (report.missingSamples[i]);
        if (report.missingSamples.size() > 12)
            shown.add ("...and " + juce::String (report.missingSamples.size() - 12) + " more");

        message << "These audio files couldn't be found:\n\n" << shown.joinIntoString ("\n")
                << "\n\nIf they've moved, pick a folder and I'll look for them by name.";
    }

    auto* w = new juce::AlertWindow ("Some things are missing", message, juce::MessageBoxIconType::WarningIcon);
    if (! report.missingSamples.isEmpty())
        w->addButton ("Find samples in a folder...", 1);
    w->addButton ("OK", 0, juce::KeyPress (juce::KeyPress::returnKey));

    juce::Component::SafePointer<MainComponent> safeThis (this);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis] (int r)
    {
        if (r == 1 && safeThis != nullptr)
            safeThis->findMissingSamples();
    }), true);
}

void MainComponent::findMissingSamples()
{
    const auto start = currentFile.existsAsFile() ? currentFile.getParentDirectory()
                                                  : juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    chooser = std::make_unique<juce::FileChooser> ("Pick a folder to search for the missing audio", start);

    juce::Component::SafePointer<MainComponent> safeThis (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [safeThis] (const juce::FileChooser& fc)
    {
        const auto folder = fc.getResult();
        if (safeThis == nullptr || ! folder.isDirectory())
            return;
        auto& self = *safeThis;

        const int before = ProjectIO::missingSampleNames (self.project).size();
        const int found  = ProjectIO::relinkMissing (self.project, self.cache, folder);
        const int left   = ProjectIO::missingSampleNames (self.project).size();

        if (found > 0)
        {
            self.pushArrangement (true);
            self.playlist.refresh();
            self.markDirty();
        }
        self.setStatus ("Found " + juce::String (before - left) + " of " + juce::String (before) + " missing files."
                        + (left > 0 ? "  " + juce::String (left) + " still missing." : juce::String()));
    });
}

void MainComponent::saveCurrent (std::function<void()> then)
{
    if (currentFile == juce::File())
    {
        saveAs (false, std::move (then));
        return;
    }
    if (saveProject (currentFile, false) && then)
        then();
}

void MainComponent::saveAs (bool collectSamples, std::function<void()> then)
{
    const auto initial = currentFile != juce::File() ? currentFile
                                                     : projectsFolder().getNonexistentChildFile ("Untitled", ProjectIO::extension, false);
    chooser = std::make_unique<juce::FileChooser> (collectSamples ? "Save project with samples" : "Save project",
                                                   initial, ProjectIO::wildcard);

    juce::Component::SafePointer<MainComponent> safeThis (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                              | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safeThis, collectSamples, then] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (safeThis == nullptr || file == juce::File())
            return;
        if (safeThis->saveProject (file, collectSamples) && then)
            then();
    });
}

bool MainComponent::saveProject (const juce::File& file, bool collectSamples)
{
    if (recordingAudio || recordingMidi)
        finishRecording();

    const auto target = file.hasFileExtension (ProjectIO::extension) ? file : file.withFileExtension (ProjectIO::extension);

    ProjectIO::SaveOptions options;
    options.collectSamples = collectSamples;
    const auto result = ProjectIO::save (target, project, engine, options);

    if (result.failed())
    {
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::WarningIcon)
                                          .withTitle ("Couldn't save")
                                          .withMessage (result.getErrorMessage())
                                          .withButton ("OK"),
                                      nullptr);
        return false;
    }

    currentFile = target;
    dirty = false;
    noteLoopRange();
    recent.addFile (target);
    plugins.settings().setValue ("recentProjects", recent.toString());
    plugins.settings().saveIfNeeded();
    ignoreEditsForAWhile();
    updateTitle();
    setStatus ((collectSamples ? "Saved with samples: " : "Saved: ") + target.getFullPathName());
    return true;
}

void MainComponent::autosave()
{
    ProjectIO::SaveOptions options;
    options.isAutosave = true;
    const auto file = ProjectIO::autosaveFile();
    file.getParentDirectory().createDirectory();
    if (ProjectIO::save (file, project, engine, options).wasOk())
        ignoreEditsForAWhile();
}

void MainComponent::offerRecovery()
{
    auto* w = new juce::AlertWindow ("Recover your work?",
                                     "ALLHAILPAN Studio didn't close properly last time.\n"
                                     "An autosave from that session is available.",
                                     juce::MessageBoxIconType::QuestionIcon);
    w->addButton ("Recover", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Start fresh", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<MainComponent> safeThis (this);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis] (int r)
    {
        if (safeThis != nullptr && r == 1)
            safeThis->loadProject (ProjectIO::autosaveFile(), true);
    }), true);
}

void MainComponent::confirmDiscard (std::function<void()> then)
{
    if (! dirty)
    {
        if (then) then();
        return;
    }

    const auto name = currentFile == juce::File() ? juce::String ("this new project") : currentFile.getFileNameWithoutExtension();
    auto* w = new juce::AlertWindow ("Save changes?", "Do you want to save your changes to " + name + "?",
                                     juce::MessageBoxIconType::QuestionIcon);
    w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Don't save", 2);
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<MainComponent> safeThis (this);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, then] (int r)
    {
        if (safeThis == nullptr || r == 0)
            return;
        if (r == 1)
            safeThis->saveCurrent (then);
        else if (then)
            then();
    }), true);
}

void MainComponent::requestQuit (std::function<void()> quitNow)
{
    if (recordingAudio || recordingMidi)
        stopAll();

    confirmDiscard ([quitNow]
    {
        // A clean exit: the autosave is no longer needed
        ProjectIO::autosaveFile().deleteFile();
        if (quitNow) quitNow();
    });
}


// ---------------------------------------------------------------------------
// Export

// ---------------------------------------------------------------------------
// MIDI files
//
// This is how a part written here is taken somewhere else and how a part
// written elsewhere is brought in, so both sides go through MidiFileIO and
// report what actually happened rather than succeeding quietly. Automation is
// not exported: a MIDI file can only carry it as controller numbers, and these
// lanes are plugin parameters by index, so any mapping would be a guess a
// receiving studio would read as something else. The dialog says so.

void MainComponent::exportMidi (bool onlySelected)
{
    auto tracks = MidiFileIO::gather (project, onlySelected);

    if (tracks.empty())
    {
        setStatus (onlySelected ? "Select a MIDI clip first: there are no notes in the selection."
                                : "There are no MIDI notes in the playlist to export.");
        return;
    }

    // Whether anything would be left behind, so the export can say so rather
    // than letting a producer find out in the other studio.
    bool hadAutomation = false;
    for (const auto& c : project.clips)
    {
        // Either kind of automation: a lane inside a pattern, or a curve on the
        // playlist. Neither travels, so neither should go unmentioned.
        const bool hasLanes = c.isMidi() && c.pattern != nullptr && ! c.pattern->lanes.empty();
        if (! hasLanes && ! c.isAutomation())
            continue;
        if (onlySelected && project.selection.count (c.id) == 0)
            continue;
        hadAutomation = true;
        break;
    }

    const auto songName = currentFile != juce::File() ? currentFile.getFileNameWithoutExtension()
                                                      : juce::String ("Untitled");
    const auto folder   = currentFile != juce::File() ? currentFile.getParentDirectory() : projectsFolder();
    const auto initial  = folder.getChildFile (songName + (onlySelected ? " selection.mid" : ".mid"));

    chooser = std::make_unique<juce::FileChooser> (onlySelected ? "Export the selected clips as MIDI"
                                                               : "Export the arrangement as MIDI",
                                                   initial, MidiFileIO::wildcard);

    const int flags = juce::FileBrowserComponent::saveMode
                        | juce::FileBrowserComponent::canSelectFiles
                        | juce::FileBrowserComponent::warnAboutOverwriting;

    juce::Component::SafePointer<MainComponent> safeThis (this);
    chooser->launchAsync (flags, [safeThis, tracks, hadAutomation, songName] (const juce::FileChooser& fc)
    {
        auto chosen = fc.getResult();
        if (safeThis == nullptr || chosen == juce::File())
            return;

        auto& self = *safeThis;

        if (chosen.getFileExtension().isEmpty())
            chosen = chosen.withFileExtension (".mid");

        const auto result = MidiFileIO::write (chosen, tracks, self.project.bpm,
                                               self.project.meter.at (0.0), songName, hadAutomation);

        if (! result.ok)
        {
            juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                              .withIconType (juce::MessageBoxIconType::WarningIcon)
                                              .withTitle ("Couldn't export the MIDI file")
                                              .withMessage (result.error)
                                              .withButton ("OK"),
                                          nullptr);
            return;
        }

        juce::String message;
        message << "Exported " << result.notesWritten
                << (result.notesWritten == 1 ? " note on " : " notes on ")
                << result.tracksWritten << (result.tracksWritten == 1 ? " track" : " tracks")
                << " to " << chosen.getFileName() << ".";

        if (result.droppedOverlaps > 0)
            message << "  " << result.droppedOverlaps
                    << (result.droppedOverlaps == 1 ? " note was" : " notes were")
                    << " left out: one channel can't sound two of the same pitch at once.";

        if (result.hadAutomation)
            message << "  Notes only: automation lanes aren't written, since a MIDI file has no "
                       "way to say which plugin parameter they belong to.";

        self.setStatus (message);
    });
}

void MainComponent::importMidiDialog()
{
    const auto folder = currentFile != juce::File() ? currentFile.getParentDirectory() : projectsFolder();

    chooser = std::make_unique<juce::FileChooser> ("Import a MIDI file", folder, MidiFileIO::wildcard);

    const int flags = juce::FileBrowserComponent::openMode
                        | juce::FileBrowserComponent::canSelectFiles
                        | juce::FileBrowserComponent::canSelectMultipleItems;

    juce::Component::SafePointer<MainComponent> safeThis (this);
    chooser->launchAsync (flags, [safeThis] (const juce::FileChooser& fc)
    {
        if (safeThis == nullptr)
            return;

        const auto files = fc.getResults();
        if (files.isEmpty())
            return;

        auto& self = *safeThis;

        juce::StringArray paths;
        for (const auto& f : files)
            paths.add (f.getFullPathName());

        // The same path a dropped file takes, so the menu and the drop cannot
        // behave differently. The start marker is where "here" is when the
        // position did not come from a mouse.
        self.setView (View::playlist);
        self.playlist.addFiles (paths, self.engine.getSongStart(),
                                self.project.firstEmptyTrackFrom (0));
    });
}

void MainComponent::exportAudio (bool stems)
{
    if (exportJob != nullptr)
    {
        setStatus ("An export is already running.");
        return;
    }
    if (recordingAudio || recordingMidi)
        finishRecording();
    engine.stopAndRewind();

    const double songEnd = project.songEndBeats();
    if (songEnd <= 0.0)
    {
        setStatus ("There's nothing in the playlist to export yet.");
        return;
    }
    if (engine.devices().getCurrentAudioDevice() == nullptr)
    {
        setStatus ("Open Audio settings and choose a device first: exports use its sample rate.");
        return;
    }

    const auto songName = currentFile != juce::File() ? currentFile.getFileNameWithoutExtension() : juce::String ("Untitled");
    const auto folder   = currentFile != juce::File() ? currentFile.getParentDirectory() : projectsFolder();

    // Which inserts actually carry sound
    std::vector<int> used;
    for (int c = 0; c < kNumChannels; ++c)
        if (engine.getChannelPlugin (c) != nullptr)
            used.push_back (project.channels[(size_t) c].insert);
    for (const auto& clip : project.clips)
        if (clip.isAudio())
            used.push_back (project.tracks[(size_t) clip.track].insert);
    std::sort (used.begin(), used.end());
    used.erase (std::unique (used.begin(), used.end()), used.end());
    used.erase (std::remove (used.begin(), used.end(), 0), used.end());

    const auto initial = stems ? folder.getChildFile (songName + " stems")
                               : folder.getChildFile (songName + ".wav");

    chooser = std::make_unique<juce::FileChooser> (stems ? "Choose a folder for the stems" : "Export the song as WAV",
                                                   initial, stems ? juce::String() : juce::String ("*.wav"));

    const int chooserFlags = stems
        ? (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectDirectories)
        : (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
           | juce::FileBrowserComponent::warnAboutOverwriting);

    juce::Component::SafePointer<MainComponent> safeThis (this);
    chooser->launchAsync (chooserFlags, [safeThis, stems, used, songEnd, songName] (const juce::FileChooser& fc)
    {
        const auto chosen = fc.getResult();
        if (safeThis == nullptr || chosen == juce::File())
            return;
        auto& self = *safeThis;

        juce::File masterFile;
        std::vector<std::pair<int, juce::File>> stemFiles;

        if (stems)
        {
            chosen.createDirectory();
            masterFile = chosen.getChildFile (songName + " (full mix).wav");
            for (int index : used)
            {
                auto name = self.engine.insert (index).name.replaceCharacters ("\\/:*?\"<>|", "         ").trim();
                if (name.isEmpty())
                    name = "Insert " + juce::String (index);
                stemFiles.emplace_back (index, chosen.getChildFile (name + ".wav"));
            }
        }
        else
        {
            masterFile = chosen.hasFileExtension ("wav") ? chosen : chosen.withFileExtension ("wav");
        }

        // The depth and the dither come from the stored settings, through the
        // two functions in Preferences.h that turn a combo box id into the
        // enum, so the off by one between the two lives in one tested place
        // rather than here. What the engine actually applies is reported back
        // by the job rather than worked out again below: the policy that only
        // 16 bit is dithered belongs to Dither.h, and a second copy of it here
        // could disagree with the file.
        const auto depth  = Prefs::exportDepthFor (self.plugins.settings()
                                                       .getIntValue (Prefs::Key::exportBitDepth,
                                                                     Prefs::exportBitDepth.fallback));
        const auto dither = Prefs::exportDitherFor (self.plugins.settings()
                                                        .getIntValue (Prefs::Key::exportDither,
                                                                      Prefs::exportDither.fallback));

        self.exportJob = std::make_unique<ExportJob> (self.engine, songEnd, self.engine.getSampleRate(),
                                                      masterFile, std::move (stemFiles), stems,
                                                      depth, dither);
        self.exportJob->startThread();

        // A bounce honours solo, exactly as playback does, because this studio
        // runs two code paths over one graph and anything on one and not the
        // other makes a bounce sound different from what was heard. Printing a
        // stem by soloing it is also an ordinary thing to want.
        //
        // No reachable documentation from any of the four studios researched
        // settles whether an export should honour solo, so it is a decision
        // rather than a citation, and what makes it safe either way is saying
        // so in words before the file is written. See Research/BACKLOG.md.
        // What is about to be written, in words, before the file appears. The
        // depth lives in Preferences rather than in a dialog in front of the
        // chooser, so this line is what stops that being a setting somebody
        // forgets they changed: a 16 bit delivery printed as a 32 bit float
        // file, or the other way round, is otherwise only discovered by
        // whoever receives it.
        //
        // It is in the progress line below as well, and that is the half
        // that makes the argument true rather than merely stated. This
        // message is on screen for one frame: checkExportProgress runs off
        // the 60 Hz timer and replaces the status outright on its first
        // tick, so an announcement made only here is an announcement nobody
        // can read. It has to stay up for the whole export.
        const auto written = self.exportJob->describeFormat();

        self.setStatus (self.engine.isAnySoloed()
                            ? "Exporting " + written + " with a solo still on, so the file has only what "
                              "the solo is letting through..."
                            : "Exporting " + written + "...");
    });
}

void MainComponent::checkExportProgress()
{
    if (exportJob == nullptr)
        return;

    if (! exportJob->done.load())
    {
        // Naming the depth on every tick rather than only once before the
        // first one. This line replaces whatever exportAudio put up, so
        // without the depth in here the studio would announce what it was
        // about to write and then hide it again a sixtieth of a second
        // later, which is the same as not saying it.
        setStatus ("Exporting " + exportJob->describeFormat() + "... "
                       + juce::String (juce::roundToInt (exportJob->progress.load() * 100.0)) + "%");
        return;
    }

    auto job = std::move (exportJob);      // finished: take it off the field
    job->stopThread (2000);

    if (job->problem.isNotEmpty())
    {
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::WarningIcon)
                                          .withTitle ("Export problem")
                                          .withMessage (job->problem)
                                          .withButton ("OK"),
                                      nullptr);
        setStatus ("Export failed.");
        return;
    }

    if (! job->succeeded.load())
    {
        setStatus ("Export stopped early.");
        return;
    }

    const auto file    = job->getMasterFile();
    const auto written = job->describeFormat();

    setStatus (job->exportedStems()
                   ? "Exported the mix and " + juce::String (job->numStems()) + " stems, " + written
                         + ", to " + file.getParentDirectory().getFullPathName()
                   : "Exported " + written + " to " + file.getFullPathName());
    file.revealToUser();
}
