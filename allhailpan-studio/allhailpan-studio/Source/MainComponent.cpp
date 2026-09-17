#include "MainComponent.h"
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

    enum RecordModeId { recAuto = 1, recAudio, recMidi, recBoth };
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

    recordMode.addItem ("Rec: Auto", recAuto);
    recordMode.addItem ("Rec: Audio", recAudio);
    recordMode.addItem ("Rec: MIDI + automation", recMidi);
    recordMode.addItem ("Rec: Audio + MIDI", recBoth);
    recordMode.setSelectedId (plugins.settings().getIntValue ("recordMode", recAuto), juce::dontSendNotification);
    recordMode.onChange = [this] { plugins.settings().setValue ("recordMode", recordMode.getSelectedId()); };
    recordMode.setTooltip ("Auto records MIDI when the selected channel has an instrument, otherwise audio from your interface");

    clickButton.setToggleState (engine.isMetronomeOn(), juce::dontSendNotification);
    clickButton.onClick = [this]
    {
        engine.setMetronome (! engine.isMetronomeOn());
        clickButton.setToggleState (engine.isMetronomeOn(), juce::dontSendNotification);
    };

    playlistTab.onClick = [this] { setView (View::playlist); };
    pianoTab.onClick    = [this] { setView (View::pianoRoll); };
    mixerTab.onClick    = [this] { toggleMixerWindow(); };
    playlistTab.setTooltip ("F5");
    pianoTab.setTooltip ("F7");
    mixerTab.setTooltip ("Opens the mixer window (F9)");

    fileButton.onClick = [this] { showFileMenu(); };
    fileButton.setTooltip ("New, open, save (Ctrl+N, Ctrl+O, Ctrl+S)");
    recent.setMaxNumberOfItems (12);
    recent.restoreFromString (plugins.settings().getValue ("recentProjects"));

    undoButton.onClick = [this] { undoRedo (false); };
    redoButton.onClick = [this] { undoRedo (true); };
    undoButton.setTooltip ("Undo (Ctrl+Z). Unlimited steps.");
    redoButton.setTooltip ("Redo (Ctrl+Y or Ctrl+Shift+Z)");

    audioButton.onClick   = [this] { showAudioSettings(); };
    pluginsButton.onClick = [this] { plugins.showPluginWindow(); };

    tempo.setSliderStyle (juce::Slider::LinearBar);
    tempo.setRange (20.0, 400.0, 0.01);
    tempo.setNumDecimalPlacesToDisplay (2);
    tempo.setValue (plugins.settings().getDoubleValue ("bpm", 128.0), juce::dontSendNotification);
    tempo.onValueChange = [this]
    {
        engine.setBpm (tempo.getValue());
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
        return c.sample != nullptr && StretchCache::needsRender (c.stretch, c.pitch)
            && stretchCache.isPending (*c.sample, c.stretch, c.pitch);
    };
    playlist.onRouteTrack    = [this] (int) { mixer.refresh(); };

    pianoRoll.onInteraction = [this] { grabKeyboardFocus(); };
    pianoRoll.onSetPosition = [this] (double b) { setPosition (b); };

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
    addAndMakeVisible (logo);

    for (auto* c : { static_cast<juce::Component*> (&playButton), static_cast<juce::Component*> (&stopButton),
                     static_cast<juce::Component*> (&recordButton), static_cast<juce::Component*> (&recordMode),
                     static_cast<juce::Component*> (&clickButton), static_cast<juce::Component*> (&playlistTab),
                     static_cast<juce::Component*> (&pianoTab), static_cast<juce::Component*> (&mixerTab),
                     static_cast<juce::Component*> (&audioButton), static_cast<juce::Component*> (&pluginsButton),
                     static_cast<juce::Component*> (&undoButton), static_cast<juce::Component*> (&redoButton),
                     static_cast<juce::Component*> (&fileButton),
                     static_cast<juce::Component*> (&tempo), static_cast<juce::Component*> (&tempoLabel),
                     static_cast<juce::Component*> (&clock), static_cast<juce::Component*> (&channelLabel),
                     static_cast<juce::Component*> (&channelBox), static_cast<juce::Component*> (&instrumentBox),
                     static_cast<juce::Component*> (&insertLabel), static_cast<juce::Component*> (&insertBox),
                     static_cast<juce::Component*> (&browseButton), static_cast<juce::Component*> (&showButton), static_cast<juce::Component*> (&unloadButton), static_cast<juce::Component*> (&sumOutsButton),
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

    if (startupError.isNotEmpty())
        setStatus ("Audio device problem: " + startupError + ". Open Audio settings to pick another device.");

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
    startTimerHz (60);
}

MainComponent::~MainComponent()
{
    stopTimer();
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
        // Pressing Stop again is a panic button: silence anything still ringing
        engine.panic();
        setStatus ("Stopped every note.");
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

    bool wantAudio = false, wantMidi = false;
    switch (recordMode.getSelectedId())
    {
        case recAudio: wantAudio = true; break;
        case recMidi:  wantMidi = true; break;
        case recBoth:  wantAudio = true; wantMidi = true; break;
        default:       wantMidi = instrument != nullptr; wantAudio = ! wantMidi; break;
    }

    if (wantMidi && instrument == nullptr)
    {
        if (! wantAudio) { setStatus ("Load an instrument on channel " + juce::String (channel + 1) + " to record MIDI."); return; }
        wantMidi = false;
    }
    if (wantAudio && ! hasInput)
    {
        if (! wantMidi) { setStatus ("No input is switched on. Open Audio settings and enable an input channel."); return; }
        wantAudio = false;
    }

    int armed = project.firstArmedTrack();
    if (armed < 0)
    {
        armed = project.firstEmptyTrackFrom (0);
        project.tracks[(size_t) armed].armed = true;
    }
    audioTrack  = armed;
    midiTrack   = wantAudio ? project.firstEmptyTrackFrom (armed + 1) : armed;
    midiChannel = channel;

    takeNotes.clear();
    heldNotes.clear();
    takeLanes.clear();

    engine.stopAndRewind();
    if (wantAudio)
        engine.startAudioRecording();
    if (wantMidi)
    {
        engine.startMidiRecording (channel);
        recordingPlugin = instrument;
        recordingPlugin->addListener (&paramRecorder);
    }
    recordingAudio = wantAudio;
    recordingMidi  = wantMidi;
    engine.setPlaying (true);
    project.changed();

    juce::String what = wantAudio && wantMidi ? "audio and MIDI" : wantAudio ? "audio" : "MIDI and automation";
    setStatus ("Recording " + what + ". Press Space or Stop to finish.");
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

            setStatus ("Recorded " + juce::String ((int) pattern->notes.size()) + " notes and "
                       + juce::String ((int) pattern->lanes.size()) + " automation lanes onto "
                       + project.tracks[(size_t) midiTrack].name + ". Double-click the clip to edit it.");
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
        recordingAudio = false;
        auto take = engine.stopAudioRecording();
        if (take.has_value())
        {
            const double sampleRate = engine.getSampleRate();
            const double latency    = engine.getRoundTripLatencySamples() / sampleRate;
            const auto   file       = BrowserPanel::recordingsFolder()
                                          .getChildFile ("Take " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S") + ".wav");

            const bool   saved     = writeWavFile (file, take->audio, sampleRate);
            const double startBeat = take->startBeat;
            const int    dropped   = take->droppedFrames;

            auto sample = cache.adopt (std::move (take->audio), sampleRate, file, file.getFileNameWithoutExtension());
            const double duration = sample->durationSeconds();
            if (duration > 0.01)
            {
                Clip clip;
                clip.type   = ClipType::audio;
                clip.sample = sample;
                clip.track  = audioTrack;
                clip.start  = startBeat;
                clip.offset = std::min (latency, duration * 0.5);
                clip.length = duration - clip.offset;
                project.selection.insert (project.addClip (clip));
                browser.refresh();

                juce::String msg = saved ? "Take saved to " + file.getFullPathName()
                                         : "Take kept in this session, but it couldn't be saved to disk.";
                if (dropped > 0)
                    msg << "  (" << dropped << " samples dropped. Try a larger buffer size.)";
                setStatus (msg);
            }
        }
    }

    playlist.refresh();
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

        if (c.isAudio())
        {
            if (c.sample == nullptr || c.sample->audio.getNumSamples() == 0 || c.sample->audio.getNumChannels() == 0)
                continue;   // missing file

            AudioEngine::AudioClipRT rt;
            rt.insert = track.insert;
            rt.start  = c.start;
            rt.gain   = juce::Decibels::decibelsToGain (c.gainDb);

            if (StretchCache::needsRender (c.stretch, c.pitch))
            {
                keysInUse.insert (StretchCache::keyFor (*c.sample, c.stretch, c.pitch));
                if (auto rendered = stretchCache.get (c.sample, c.stretch, c.pitch, allowRenders))
                {
                    rt.data       = rendered.get();
                    rt.readOffset = c.offset * c.stretch;
                    rt.playLength = c.length * c.stretch;
                    rt.rate       = 1.0;
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
                const double on = origin + n.start;
                if (on < m.start || on >= m.end)
                    continue;
                AudioEngine::NoteRT note;
                note.on       = on;
                note.off      = std::min (on + n.length, m.end);
                note.note     = n.note;
                note.velocity = (juce::uint8) juce::jlimit (1, 127, juce::roundToInt (n.velocity * 127.0f));
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
    }

    snap.songEnd = project.songEndBeats();
    engine.setSnapshot (std::move (snap));
    stretchCache.keepOnly (keysInUse);
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &project)
    {
        const bool editing = playlist.isEditing() || pianoRoll.isDragging();
        pushArrangement (! editing);
        if (! editing)
        {
            if (project.commit())    // a finished edit becomes one undo step
                markDirty();
            updateUndoButtons();
        }
        playlist.refresh();
        if (view == View::pianoRoll) pianoRoll.refresh();
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
    sumOutsButton.setEnabled (loaded && engine.getChannelOutputBuses (ch) > 1);
    sumOutsButton.setToggleState (engine.getChannelSumsOutputs (ch), juce::dontSendNotification);
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
            b.store (! b.load());
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
    pluginWindows[key] = std::move (window);
}

void MainComponent::closePluginWindow (juce::AudioProcessor* plugin)
{
    pluginWindows.erase (plugin);
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
    mixerWindow->toFront (true);
    mixerTab.setToggleState (true, juce::dontSendNotification);
}

void MainComponent::setView (View v)
{
    view = v;
    playlist.setVisible (v == View::playlist);
    pianoRoll.setVisible (v == View::pianoRoll);
    playlistTab.setToggleState (v == View::playlist, juce::dontSendNotification);
    pianoTab.setToggleState (v == View::pianoRoll, juce::dontSendNotification);

    if (v == View::pianoRoll)
    {
        if (pianoRoll.getClipId() == 0 || project.find (pianoRoll.getClipId()) == nullptr)
            for (auto& c : project.clips)
                if (! c.isAudio() && project.selection.count (c.id))
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
    if (key.getKeyCode() == juce::KeyPress::F7Key)      { setView (View::pianoRoll); return true; }
    if (key.getKeyCode() == juce::KeyPress::F9Key)      { toggleMixerWindow(); return true; }
    if (is ('r', cmd))                                  { toggleRecord(); return true; }
    if (is ('z', cmd))                                  { undoRedo (false); return true; }
    if (is ('s', cmd))                                  { saveCurrent(); return true; }
    if (is ('s', cmd | juce::ModifierKeys::shiftModifier)) { saveAs (false); return true; }
    if (is ('o', cmd))                                  { openProjectDialog(); return true; }
    if (is ('n', cmd))                                  { newProject(); return true; }
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

void MainComponent::showAudioSettings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent (engine.devices(), 0, 8, 0, 8, true, false, true, false);
    selector->setSize (560, 480);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (selector);
    o.dialogTitle                  = "Audio settings";
    o.dialogBackgroundColour       = Ahp::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar            = true;
    o.resizable                    = true;
    if (auto* w = o.launchAsync())
        Ahp::applyDarkTitleBar (*w);
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
    bar.removeFromLeft (40 + 132);                          // logo + wordmark
    fileButton  .setBounds (bar.removeFromLeft (52));  bar.removeFromLeft (10);
    playButton  .setBounds (bar.removeFromLeft (58));  bar.removeFromLeft (4);
    stopButton  .setBounds (bar.removeFromLeft (58));  bar.removeFromLeft (4);
    recordButton.setBounds (bar.removeFromLeft (50));  bar.removeFromLeft (4);
    recordMode  .setBounds (bar.removeFromLeft (150)); bar.removeFromLeft (10);
    clock       .setBounds (bar.removeFromLeft (96));  bar.removeFromLeft (10);
    tempoLabel  .setBounds (bar.removeFromLeft (48));
    tempo       .setBounds (bar.removeFromLeft (84));  bar.removeFromLeft (6);
    clickButton .setBounds (bar.removeFromLeft (54));  bar.removeFromLeft (18);
    playlistTab .setBounds (bar.removeFromLeft (76));
    pianoTab    .setBounds (bar.removeFromLeft (86));
    mixerTab    .setBounds (bar.removeFromLeft (62));
    pluginsButton.setBounds (bar.removeFromRight (74)); bar.removeFromRight (6);
    audioButton  .setBounds (bar.removeFromRight (116)); bar.removeFromRight (14);
    redoButton   .setBounds (bar.removeFromRight (54)); bar.removeFromRight (4);
    undoButton   .setBounds (bar.removeFromRight (54));

    auto cb = channelBar.reduced (10, 7);
    channelLabel .setBounds (cb.removeFromLeft (62));
    channelBox   .setBounds (cb.removeFromLeft (190)); cb.removeFromLeft (6);
    instrumentBox.setBounds (cb.removeFromLeft (238)); cb.removeFromLeft (4);
    browseButton .setBounds (cb.removeFromLeft (62));  cb.removeFromLeft (6);
    showButton   .setBounds (cb.removeFromLeft (58));  cb.removeFromLeft (4);
    unloadButton .setBounds (cb.removeFromLeft (66));  cb.removeFromLeft (6);
    sumOutsButton.setBounds (cb.removeFromLeft (100)); cb.removeFromLeft (14);
    insertLabel  .setBounds (cb.removeFromLeft (46));
    insertBox    .setBounds (cb.removeFromLeft (130)); cb.removeFromLeft (12);
    typingLabel  .setBounds (cb);

    auto work = workArea;
    browser.setBounds (work.removeFromLeft (260));
    playlist.setBounds (work);
    pianoRoll.setBounds (work);

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
        info = d->getTypeName() + "  |  " + d->getName()
             + "  |  " + juce::String ((int) sr) + " Hz"
             + "  |  " + juce::String (d->getCurrentBufferSizeSamples()) + " samples"
             + "  |  " + juce::String (sr > 0 ? 1000.0 * latency / sr : 0.0, 1) + " ms round trip"
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

    if (++autosaveTicks >= 60 * 120)        // every two minutes
    {
        autosaveTicks = 0;
        if (dirty && ! recordingAudio && ! recordingMidi && ! playlist.isEditing() && ! pianoRoll.isDragging())
            autosave();
    }

    if (recordingMidi)
        collectRecordedMidi();

    clock.setText (formatPosition (engine.getBeatPosition()), juce::dontSendNotification);
    recordButton.setToggleState (recordingAudio || recordingMidi, juce::dontSendNotification);
    playButton.setToggleState (engine.isPlaying(), juce::dontSendNotification);

    if (view == View::playlist)  playlist.updatePlayhead();
    if (view == View::pianoRoll) pianoRoll.updatePlayhead();
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

    juce::PopupMenu menu;
    menu.addItem (item (1, "New project", "Ctrl+N"));
    menu.addItem (item (2, "Open...", "Ctrl+O"));
    menu.addSubMenu ("Open recent", recentMenu, recentMenu.getNumItems() > 0);
    menu.addSeparator();
    menu.addItem (item (3, "Save", "Ctrl+S"));
    menu.addItem (item (4, "Save as...", "Ctrl+Shift+S"));
    menu.addItem (item (5, "Save with samples (copies audio beside the project)", {}));
    menu.addSeparator();
    menu.addItem (item (6, "Find missing samples" + (missing.isEmpty() ? juce::String() : " (" + juce::String (missing.size()) + ")"),
                        {}, ! missing.isEmpty()));
    menu.addItem (item (7, "Show project in folder", {}, currentFile.existsAsFile()));

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
            case 7: self.currentFile.revealToUser(); break;
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
        self.engine.setSongStart (0.0);
        self.engine.stopAndRewind();

        self.currentFile = juce::File();
        self.dirty = false;
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
