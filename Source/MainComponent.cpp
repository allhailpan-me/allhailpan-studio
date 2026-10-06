#include "MainComponent.h"
#include "AudioDefaults.h"
#include "MidiFileIO.h"
#include "AhpLookAndFeel.h"
#include "DarkTitleBar.h"
#include "Recorder.h"
#include <cmath>

namespace
{
    juce::String formatPosition (double beats)
    {
        beats = std::max (0.0, beats);
        const auto bar  = (int) (beats / 4.0) + 1;
        const auto beat = (int) std::fmod (beats, 4.0) + 1;
        const auto tick = (int) ((beats - std::floor (beats)) * 96.0);
        return juce::String (bar) + ":" + juce::String (beat) + ":" + juce::String (tick).paddedLeft ('0', 2);
    }

    enum RecordModeId { recAuto = 1, recAudio, recMidi, recBoth, recInstrument, recInstrumentMidi };

    constexpr const char* inputTooltip =
        "Which input you hear and record.\n"
        "One instrument in one socket is a mono input, and it is heard in the middle "
        "rather than out of one speaker. Pick a pair only for something genuinely "
        "stereo, like a keyboard using two sockets.";
}

MainComponent::MainComponent()
{
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

    inputBox.setTooltip (inputTooltip);
    inputBox.onChange = [this]
    {
        const int id = inputBox.getSelectedId();

        if (id <= 0)
            return;

        const auto choice = InputSource::fromStored (id - 1);
        engine.setInputSource (choice);
        plugins.settings().setValue (Prefs::Key::inputSource, id - 1);
    };

    monitorBox.addItem ("Monitor: off", 1);
    monitorBox.addItem ("Monitor: armed", 2);
    monitorBox.addItem ("Monitor: on", 3);
    monitorBox.setSelectedId (1, juce::dontSendNotification);
    monitorBox.setTooltip ("Hear your input through this studio's own effects.\n"
                           "armed = only while a track is armed, on = always.\n"
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
            message << "Monitoring through " << engine.insert (engine.getMonitorInsert()).name
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
    refreshInputSources();

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
                     static_cast<juce::Component*> (&recordButton), static_cast<juce::Component*> (&recordMode), static_cast<juce::Component*> (&inputBox), static_cast<juce::Component*> (&monitorBox), static_cast<juce::Component*> (&countInBox),
                     static_cast<juce::Component*> (&clickButton), static_cast<juce::Component*> (&playlistTab),
                     static_cast<juce::Component*> (&pianoTab), static_cast<juce::Component*> (&rackTab),
                     static_cast<juce::Component*> (&modTab), static_cast<juce::Component*> (&reportTab),
                     static_cast<juce::Component*> (&mixerTab),
                     static_cast<juce::Component*> (&audioButton), static_cast<juce::Component*> (&pluginsButton),
                     static_cast<juce::Component*> (&undoButton), static_cast<juce::Component*> (&redoButton),
                     static_cast<juce::Component*> (&fileButton),
                     static_cast<juce::Component*> (&tempo), static_cast<juce::Component*> (&tempoLabel),
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
    pushArmedState();
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

    int armed = project.firstArmedTrack();
    if (armed < 0)
    {
        armed = project.firstEmptyTrackFrom (0);
        project.tracks[(size_t) armed].armed = true;
    }
    const bool recordingSomeAudio = wantInput || wantInstrument;
    audioTrack  = armed;
    midiTrack   = recordingSomeAudio ? project.firstEmptyTrackFrom (armed + 1) : armed;
    midiChannel = channel;

    takeNotes.clear();
    heldNotes.clear();
    takeLanes.clear();

    const double countBeats = (countInBox.getSelectedId() - 1) * 4.0;

    // Recording over a loop starts at the loop, not wherever the playhead
    // happened to be. Otherwise the first pass would be the odd one out: it
    // would start somewhere else and be a different length from the nine
    // after it, and nothing in the folder would line up.
    if (project.hasLoopRange())
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
    if (project.hasLoopRange() && recordingSomeAudio)
        msg << "  Every time round the loop is kept as its own take.";
    setStatus (msg);
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
            clip.length  = std::max (4.0, std::ceil (std::max (stopBeat - start, pattern->lastBeat()) / 4.0 - 1e-9) * 4.0);
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
        // Read before it is cleared: whether the take came back through the
        // interface is what decides if its head has to be trimmed, and
        // clearing the flag first silently trimmed every instrument take by
        // a round trip it never made.
        const bool throughInterface = ! recordingInstrument;
        recordingAudio = false;
        recordingInstrument = false;
        engine.setRecordSource (-1);
        finishAudioRecording (engine.stopAudioRecording(), throughInterface);
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
void MainComponent::finishAudioRecording (std::vector<Recorder::Take> passes, bool throughInterface)
{
    if (passes.empty())
        return;

    const double sampleRate = engine.getSampleRate();

    // The interface's round trip is why a take lands late against the
    // arrangement, so the head of every pass is trimmed by it. Recording an
    // instrument's own output never goes out to the interface and back, so
    // there is nothing to trim there.
    const double latency = throughInterface ? engine.getRoundTripLatencySamples() / sampleRate : 0.0;

    const auto stem = ! throughInterface && project.channels[(size_t) midiChannel].name.isNotEmpty()
                          ? project.channels[(size_t) midiChannel].name + " take "
                          : juce::String ("Take ");
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S");

    int  dropped   = 0;
    int  unsaved   = 0;
    bool anyToDisk = false;

    std::vector<Take> takes;
    double folderStart = -1.0;

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
        const double trim     = std::min (latency, duration * 0.5);
        if (duration - trim <= 0.01)
            continue;                      // a pass too short to be one

        if (folderStart < 0.0)
            folderStart = pass.startBeat;

        Take take;
        take.name   = "Take " + juce::String ((int) takes.size() + 1);
        take.sample = sample;
        take.offset = trim;
        take.length = duration - trim;

        // Where this pass sits inside the folder. Loop recording puts every
        // pass at the same place, and this is zero for all of them; a pass
        // that began later than the first is held back by the difference, so
        // a punch in does not slide to the front of the folder.
        take.start = std::max (0.0, (pass.startBeat - folderStart) * 60.0 / std::max (1.0, project.bpm));

        takes.push_back (std::move (take));
    }

    if (takes.empty())
        return;

    juce::String msg;

    if (takes.size() == 1)
    {
        // One pass is one clip, the way it has always been. Wrapping a single
        // take in a folder would mean every ordinary recording came back as
        // something that needs comping.
        Clip clip;
        clip.type   = ClipType::audio;
        clip.sample = takes[0].sample;
        clip.track  = audioTrack;
        clip.start  = folderStart;
        clip.offset = takes[0].offset;
        clip.length = takes[0].length;
        project.selection.insert (project.addClip (clip));
        msg = unsaved == 0 ? "Take saved to " + takes[0].sample->file.getFullPathName()
                           : juce::String ("Take kept in this session, but it couldn't be saved to disk.");
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
        if (auto* existing = project.takeFolderAt (audioTrack, folderStart, totalSeconds))
        {
            id = existing->id;
            for (auto& t : takes)
                project.addTake (id, std::move (t));
        }
        else
        {
            id = project.addTakeFolder (audioTrack, folderStart, std::move (takes));
        }

        project.selection = { id };
        const auto* folder = project.folderFor (id);
        const int   count  = folder != nullptr ? folder->size() : 0;

        msg = juce::String (count) + " takes in a folder on "
            + project.tracks[(size_t) audioTrack].name
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

                snap.audio.push_back (rt);
            }
        }
        else if (c.isAudio())
        {
            if (c.sample == nullptr || c.sample->audio.getNumSamples() == 0 || c.sample->audio.getNumChannels() == 0)
                continue;   // missing file

            AudioEngine::AudioClipRT rt;
            rt.insert = track.insert;
            rt.start  = c.start;
            rt.gain   = juce::Decibels::decibelsToGain (c.gainDb);

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

    snap.songEnd    = project.songEndBeats();
    snap.modulators = project.modulators;      // small, so copied whole
    engine.setSnapshot (std::move (snap));

    if (project.hasLoopRange())
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
        pushArmedState();

        // The loop range is saved with the project but is deliberately not
        // part of the undo history, so commit() above never notices it. It
        // still has to make the project worth saving, or moving the loop and
        // closing the window would quietly lose it.
        if (project.loopStart != lastLoopStart || project.loopEnd != lastLoopEnd)
        {
            lastLoopStart = project.loopStart;
            lastLoopEnd   = project.loopEnd;
            markDirty();
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

void MainComponent::pushArmedState()
{
    bool armed = false;
    for (const auto& t : project.tracks)
        if (t.armed)
        {
            armed = true;
            break;
        }

    engine.setInputArmed (armed);
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

void MainComponent::showPreferences (PreferencesComponent::Tab tab)
{
    showPreferencesWindow (plugins, engine.devices(), preferenceActions(), tab);
}

void MainComponent::refreshInputSources()
{
    const int channels = engine.getInputChannelCount();
    const auto all = InputSource::options (channels);

    // The id is the packed choice plus one, because a ComboBox treats zero as
    // "nothing selected". That makes the menu rebuildable without a lookup
    // table: whatever is selected can be unpacked straight back into a choice.
    //
    // Read from the settings file rather than from the engine. The engine
    // holds what is working now, which after a fallback is not what the
    // person chose: unplug an eight input interface and the choice falls back
    // to input 1, and taking that back off the engine next time would make
    // the fallback permanent, quietly ignoring a preference still sitting on
    // disk and still right. Falling back must not be the same as choosing.
    const int stored = plugins.settings().getIntValue (
                           Prefs::Key::inputSource,
                           InputSource::toStored (InputSource::defaultChoice()));

    const int wantedId = InputSource::toStored (InputSource::fromStored (stored)) + 1;

    inputBox.clear (juce::dontSendNotification);

    for (const auto& choice : all)
        inputBox.addItem (InputSource::describe (choice), InputSource::toStored (choice) + 1);

    if (all.empty())
    {
        inputBox.setTextWhenNoChoicesAvailable ("No inputs");
        return;
    }

    // Keep what was stored when this device still has it, and fall back to
    // the first input when it does not. Deliberately without notification, so
    // the fallback is not written back over the preference: plugging the
    // interface back in has to restore the choice rather than find it gone.
    const bool stillThere = inputBox.indexOfItemId (wantedId) >= 0;
    const int  chosenId   = stillThere ? wantedId
                                       : InputSource::toStored (InputSource::defaultChoice()) + 1;

    inputBox.setSelectedId (chosenId, juce::dontSendNotification);
    engine.setInputSource (InputSource::fromStored (chosenId - 1));

    // What each entry is on the hardware in front of them. "In 1" is the
    // first input the device hands over, which is the first socket only while
    // every channel is enabled: untick one in the audio settings and the
    // numbering shifts. Spelling the mapping out here costs a tooltip and
    // removes the one way this menu could still mislead.
    juce::String tip (inputTooltip);
    const auto names = engine.getActiveInputChannelNames();

    if (names.size() == channels && channels > 0)
    {
        tip << "\nOn this device: ";

        for (int i = 0; i < channels; ++i)
            tip << (i > 0 ? ", " : "") << "In " << (i + 1) << " is " << names[i];
    }

    inputBox.setTooltip (tip);
}

void MainComponent::setStatus (const juce::String& message)
{
    statusMessage = message;
    statusTime = juce::Time::currentTimeMillis();
    repaint (statusBar);
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
    audioButton  .setBounds (bar.removeFromRight (66)); bar.removeFromRight (14);
    redoButton   .setBounds (bar.removeFromRight (54)); bar.removeFromRight (4);
    undoButton   .setBounds (bar.removeFromRight (54)); bar.removeFromRight (10);

    bar.removeFromLeft (40 + 132);                          // logo + wordmark
    fileButton  .setBounds (bar.removeFromLeft (48));  bar.removeFromLeft (10);
    playButton  .setBounds (bar.removeFromLeft (58));  bar.removeFromLeft (4);
    stopButton  .setBounds (bar.removeFromLeft (58));  bar.removeFromLeft (4);
    recordButton.setBounds (bar.removeFromLeft (50));  bar.removeFromLeft (4);
    recordMode  .setBounds (bar.removeFromLeft (132)); bar.removeFromLeft (4);
    inputBox    .setBounds (bar.removeFromLeft (72));  bar.removeFromLeft (4);
    monitorBox  .setBounds (bar.removeFromLeft (116)); bar.removeFromLeft (4);
    countInBox  .setBounds (bar.removeFromLeft (104)); bar.removeFromLeft (8);
    clock       .setBounds (bar.removeFromLeft (80));  bar.removeFromLeft (8);
    tempoLabel  .setBounds (bar.removeFromLeft (42));
    tempo       .setBounds (bar.removeFromLeft (72));  bar.removeFromLeft (6);
    clickButton .setBounds (bar.removeFromLeft (48));  bar.removeFromLeft (14);
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

    // The input menu belongs to whichever device is open, and the device can
    // change from the preferences window or from the driver search, neither of
    // which comes back through here. Comparing the channel count is cheap and
    // catches every way it can happen, including an interface being unplugged.
    if (const int channels = engine.getInputChannelCount(); channels != lastInputChannelCount)
    {
        lastInputChannelCount = channels;
        refreshInputSources();
    }

    checkModulationLearn();
    updateFloatingWindows();

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
        const int beatsLeft = (int) std::ceil (engine.countInBeatsLeft() - 1.0e-6);
        clock.setText (juce::String (std::max (1, beatsLeft)), juce::dontSendNotification);
        clock.setColour (juce::Label::textColourId, Ahp::rec);
    }
    else
    {
        clock.setText (formatPosition (engine.getBeatPosition()), juce::dontSendNotification);
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

        const auto result = MidiFileIO::write (chosen, tracks, self.project.bpm, songName, hadAutomation);

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

        self.exportJob = std::make_unique<ExportJob> (self.engine, songEnd, self.engine.getSampleRate(),
                                                      masterFile, std::move (stemFiles), stems);
        self.exportJob->startThread();
        self.setStatus ("Exporting...");
    });
}

void MainComponent::checkExportProgress()
{
    if (exportJob == nullptr)
        return;

    if (! exportJob->done.load())
    {
        setStatus ("Exporting... " + juce::String (juce::roundToInt (exportJob->progress.load() * 100.0)) + "%");
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

    const auto file = job->getMasterFile();
    setStatus (job->exportedStems()
                   ? "Exported the mix and " + juce::String (job->numStems()) + " stems to " + file.getParentDirectory().getFullPathName()
                   : "Exported to " + file.getFullPathName());
    file.revealToUser();
}
