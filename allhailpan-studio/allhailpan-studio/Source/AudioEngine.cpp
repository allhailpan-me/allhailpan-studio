#include "AudioEngine.h"
#include <algorithm>
#include <cmath>

AudioEngine::AudioEngine()
{
    for (int i = 0; i < kNumInserts; ++i)
        controls[(size_t) i].name = i == 0 ? juce::String ("Master") : "Insert " + juce::String (i);
    for (int c = 0; c < kNumChannels; ++c)
        channelSlots[(size_t) c].insert.store (c + 1);
}

AudioEngine::~AudioEngine()
{
    deviceManager.removeMidiInputDeviceCallback ({}, &midiCollector);
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
    forEachPlugin ([] (juce::AudioPluginInstance& p) { p.releaseResources(); });
}

juce::String AudioEngine::start (const juce::XmlElement* savedDeviceState)
{
    auto error = deviceManager.initialise (8, 8, savedDeviceState, true);

    if (savedDeviceState == nullptr)
        for (auto& d : juce::MidiInput::getAvailableDevices())
            deviceManager.setMidiInputDeviceEnabled (d.identifier, true);

    deviceManager.addMidiInputDeviceCallback ({}, &midiCollector);
    deviceManager.addAudioCallback (this);
    return error;
}

int AudioEngine::getRoundTripLatencySamples()
{
    if (auto* d = deviceManager.getCurrentAudioDevice())
        return d->getInputLatencyInSamples() + d->getOutputLatencyInSamples();
    return 0;
}

// ---------------------------------------------------------------------------
// Plugins

void AudioEngine::preparePlugin (juce::AudioPluginInstance& plugin)
{
    plugin.setPlayHead (&playHead);
    plugin.setRateAndBufferSizeDetails (sampleRate, blockSize);
    plugin.prepareToPlay (sampleRate, blockSize);
}

int AudioEngine::channelsFor (const juce::AudioPluginInstance& p)
{
    return std::max ({ 2, p.getTotalNumInputChannels(), p.getTotalNumOutputChannels() });
}

void AudioEngine::setChannelPlugin (int channel, std::unique_ptr<juce::AudioPluginInstance> plugin)
{
    if (! juce::isPositiveAndBelow (channel, kNumChannels))
        return;

    juce::AudioBuffer<float> buffer;
    std::vector<float> lastAuto;
    int channels = 2, buses = 1;
    bool sum = false;
    if (plugin != nullptr)
    {
        // Switch on every output bus, so multi-output instruments can be heard.
        // VST3 marks extra outputs as "auxiliary" and hosts often leave them off.
        plugin->enableAllBuses();
        buses = plugin->getBusCount (false);
        sum   = buses > 1;          // by default, mix them all together

        preparePlugin (*plugin);
        channels = channelsFor (*plugin);
        buffer.setSize (channels, bufferCapacity());
        lastAuto.assign ((size_t) plugin->getParameters().size(), -1.0f);
    }

    std::unique_ptr<juce::AudioPluginInstance> old;
    {
        const juce::SpinLock::ScopedLockType lock (graphLock);
        auto& slot = channelSlots[(size_t) channel];
        old = std::move (slot.plugin);
        slot.plugin   = std::move (plugin);
        slot.buffer   = std::move (buffer);
        slot.channels = channels;
        slot.outputBuses = buses;
        slot.sumOutputs.store (sum);
        slot.lastAuto = std::move (lastAuto);
    }

    if (old != nullptr)
        old->releaseResources();
}

juce::AudioPluginInstance* AudioEngine::getChannelPlugin (int channel) const noexcept
{
    return juce::isPositiveAndBelow (channel, kNumChannels) ? channelSlots[(size_t) channel].plugin.get() : nullptr;
}

void AudioEngine::setChannelInsert (int channel, int insertIndex) noexcept
{
    if (juce::isPositiveAndBelow (channel, kNumChannels))
        channelSlots[(size_t) channel].insert.store (juce::jlimit (0, kNumInserts - 1, insertIndex));
}

void AudioEngine::setChannelSumsOutputs (int channel, bool shouldSum) noexcept
{
    if (juce::isPositiveAndBelow (channel, kNumChannels))
        channelSlots[(size_t) channel].sumOutputs.store (shouldSum);
}

bool AudioEngine::getChannelSumsOutputs (int channel) const noexcept
{
    return juce::isPositiveAndBelow (channel, kNumChannels) && channelSlots[(size_t) channel].sumOutputs.load();
}

int AudioEngine::getChannelOutputBuses (int channel) const noexcept
{
    return juce::isPositiveAndBelow (channel, kNumChannels) ? channelSlots[(size_t) channel].outputBuses : 0;
}

void AudioEngine::setFx (int insertIndex, int slotIndex, std::unique_ptr<juce::AudioPluginInstance> plugin)
{
    if (! juce::isPositiveAndBelow (insertIndex, kNumInserts) || ! juce::isPositiveAndBelow (slotIndex, kNumFxSlots))
        return;

    juce::AudioBuffer<float> scratch;
    int channels = 2;
    if (plugin != nullptr)
    {
        preparePlugin (*plugin);
        channels = channelsFor (*plugin);
        scratch.setSize (channels, bufferCapacity());
    }

    std::unique_ptr<juce::AudioPluginInstance> old;
    {
        const juce::SpinLock::ScopedLockType lock (graphLock);
        auto& slot = insertSlots[(size_t) insertIndex].fx[(size_t) slotIndex];
        old = std::move (slot.plugin);
        slot.plugin   = std::move (plugin);
        slot.scratch  = std::move (scratch);
        slot.channels = channels;
    }
    controls[(size_t) insertIndex].bypass[(size_t) slotIndex].store (false);

    if (old != nullptr)
        old->releaseResources();
}

juce::AudioPluginInstance* AudioEngine::getFx (int insertIndex, int slotIndex) const noexcept
{
    if (! juce::isPositiveAndBelow (insertIndex, kNumInserts) || ! juce::isPositiveAndBelow (slotIndex, kNumFxSlots))
        return nullptr;
    return insertSlots[(size_t) insertIndex].fx[(size_t) slotIndex].plugin.get();
}

// ---------------------------------------------------------------------------
// Arrangement, preview, recording queues

void AudioEngine::setSnapshot (Snapshot&& newSnapshot)
{
    songEnd.store (newSnapshot.songEnd);
    snapshotChanged.store (true);   // notes from the old arrangement must be released
    {
        const juce::SpinLock::ScopedLockType lock (graphLock);
        std::swap (snapshot, newSnapshot);
    }
    // the previous snapshot is freed here, on the message thread
}

void AudioEngine::preview (std::shared_ptr<SampleData> sample)
{
    const SampleData* raw = sample.get();
    {
        const juce::SpinLock::ScopedLockType lock (previewLock);
        previewData = raw;
        previewPos  = 0.0;
    }
    previewHold = std::move (sample);
}

void AudioEngine::startMidiRecording (int channel)
{
    midiRecording.store (false);
    midiFifo.reset();
    {
        const juce::SpinLock::ScopedLockType lock (paramWriteLock);
        paramFifo.reset();
    }
    midiRecordChannel.store (juce::jlimit (0, kNumChannels - 1, channel));
    midiRecordStart.store (-1.0);
    midiRecording.store (true);
}

void AudioEngine::drainRecordedMidi (std::vector<RecordedMidi>& out)
{
    const auto scope = midiFifo.read (midiFifo.getNumReady());
    scope.forEach ([&] (int index) { out.push_back (midiRing[(size_t) index]); });
}

void AudioEngine::drainRecordedParams (std::vector<RecordedParam>& out)
{
    const auto scope = paramFifo.read (paramFifo.getNumReady());
    scope.forEach ([&] (int index) { out.push_back (paramRing[(size_t) index]); });
}

void AudioEngine::pushRecordedParam (int index, float value)
{
    if (! midiRecording.load() || ! playing.load())
        return;

    const double beat = beatPosition.load();
    const juce::SpinLock::ScopedLockType lock (paramWriteLock);
    const auto scope = paramFifo.write (1);
    scope.forEach ([&] (int i) { paramRing[(size_t) i] = { beat, index, value }; });
}

// ---------------------------------------------------------------------------
// Device lifecycle

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    sampleRate = device->getCurrentSampleRate();
    if (sampleRate <= 0.0)
        sampleRate = 44100.0;
    blockSize = std::max (16, device->getCurrentBufferSizeSamples());

    clickDecay = std::exp (-1.0 / (sampleRate * 0.008));
    midiCollector.reset (sampleRate);
    liveMidi.ensureSize (8192);
    fxMidi.ensureSize (2048);
    recorder.prepare (sampleRate);

    const int cap = bufferCapacity();
    blockBeats.assign ((size_t) cap, 0.0);
    clickBuffer.assign ((size_t) cap, 0.0f);

    const juce::SpinLock::ScopedLockType lock (graphLock);
    for (auto& c : channelSlots)
    {
        c.midi.ensureSize (8192);
        if (c.plugin != nullptr)
        {
            c.plugin->releaseResources();
            preparePlugin (*c.plugin);
            c.buffer.setSize (c.channels, cap);
        }
    }
    for (auto& ins : insertSlots)
    {
        ins.buffer.setSize (2, cap);
        for (auto& f : ins.fx)
            if (f.plugin != nullptr)
            {
                f.plugin->releaseResources();
                preparePlugin (*f.plugin);
                f.scratch.setSize (f.channels, cap);
            }
    }
    capacity = cap;
}

void AudioEngine::audioDeviceStopped()
{
    const juce::SpinLock::ScopedLockType lock (graphLock);
    forEachPlugin ([] (juce::AudioPluginInstance& p) { p.releaseResources(); });
}

// ---------------------------------------------------------------------------
// The audio callback

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                                    float* const* outputChannelData, int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    // ---- input meter ----
    float inPeak = 0.0f;
    for (int ch = 0; ch < numInputChannels; ++ch)
        if (auto* in = inputChannelData[ch])
            for (int i = 0; i < numSamples; ++i)
                inPeak = std::max (inPeak, std::abs (in[i]));
    inputLevel.store (std::max (inPeak, inputLevel.load() * 0.85f));

    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (auto* out = outputChannelData[ch])
            std::fill_n (out, numSamples, 0.0f);

    float* outL = numOutputChannels > 0 ? outputChannelData[0] : nullptr;
    float* outR = numOutputChannels > 1 ? outputChannelData[1] : nullptr;

    // ---- transport requests ----
    bool jumped = false;
    if (rewind.exchange (false))
    {
        position = songStart.load();
        lastBeat = -1;
        jumped = true;
    }
    const double jumpTo = locateRequest.exchange (-1.0);
    if (jumpTo >= 0.0)
    {
        position = jumpTo;
        lastBeat = -1;
        jumped = true;
    }

    const bool   isRunning      = playing.load();
    const bool   clickOn        = metronome.load();
    const double currentBpm     = bpm.load();
    const double beatsPerSample = currentBpm / 60.0 / sampleRate;
    const bool   recording      = recorder.isActive() || midiRecording.load();
    const double loopEnd        = songEnd.load();
    const int    slots          = (int) blockBeats.size();
    const int    n              = std::min (numSamples, slots);
    const double blockStartBeat = position;

    // ---- live MIDI (hardware, on-screen piano, typing keyboard) ----
    liveMidi.clear();
    midiCollector.removeNextBlockOfMessages (liveMidi, numSamples);
    keyboardState.processNextMidiBuffer (liveMidi, 0, numSamples, true);

    // ---- audio recording tap ----
    if (isRunning && numInputChannels > 0)
        recorder.push (inputChannelData[0], numInputChannels > 1 ? inputChannelData[1] : nullptr, numSamples, position);

    // ---- playhead, loop, metronome ----
    bool wrapped = false;
    if (n > 0)
        std::fill_n (clickBuffer.begin(), n, 0.0f);

    for (int i = 0; i < numSamples; ++i)
    {
        if (isRunning)
        {
            // Loop at the end of the last clip (not while recording)
            if (! recording && loopEnd > 0.0 && position >= loopEnd)
            {
                position = 0.0;
                lastBeat = -1;
                wrapped  = true;
            }
            if (i < slots)
                blockBeats[(size_t) i] = position;

            const auto beat = (juce::int64) std::floor (position);
            if (beat != lastBeat)
            {
                lastBeat = beat;
                if (clickOn)
                {
                    clickSamplesLeft = (int) (sampleRate * 0.05);
                    clickPhase = 0.0;
                    clickFreq  = (beat % 4 == 0) ? 1760.0 : 1320.0;
                    clickAmp   = 0.35f;
                }
            }
            position += beatsPerSample;
        }

        if (clickSamplesLeft > 0 && i < slots)
        {
            clickBuffer[(size_t) i] = (float) std::sin (clickPhase) * clickAmp;
            clickPhase += juce::MathConstants<double>::twoPi * clickFreq / sampleRate;
            clickAmp   *= (float) clickDecay;
            --clickSamplesLeft;
        }
    }

    const bool sendAllOff = jumped || wrapped || (wasRunning && ! isRunning);
    wasRunning = isRunning;

    // ---- MIDI recording tap ----
    if (midiRecording.load() && isRunning && n > 0)
    {
        if (midiRecordStart.load() < 0.0)
            midiRecordStart.store (blockStartBeat);

        for (const auto meta : liveMidi)
        {
            if (meta.numBytes < 1 || meta.numBytes > 3)
                continue;
            const auto scope = midiFifo.write (1);
            const double beat = blockBeats[(size_t) juce::jlimit (0, n - 1, meta.samplePosition)];
            scope.forEach ([&] (int index)
            {
                auto& r = midiRing[(size_t) index];
                r.beat = beat;
                r.size = meta.numBytes;
                for (int b = 0; b < 3; ++b)
                    r.bytes[b] = b < meta.numBytes ? meta.data[b] : (juce::uint8) 0;
            });
        }
    }

    // ---- the mixer graph ----
    {
        const juce::SpinLock::ScopedTryLockType lock (graphLock);
        if (lock.isLocked() && numSamples <= capacity && numSamples <= slots)
        {
            playHead.bpm       = currentBpm;
            playHead.ppq       = blockStartBeat;
            playHead.seconds   = blockStartBeat * 60.0 / currentBpm;
            playHead.samples   = (juce::int64) (playHead.seconds * sampleRate);
            playHead.playing   = isRunning;
            playHead.recording = recording;
            playHead.looping   = ! recording && loopEnd > 0.0;
            playHead.loopStart = 0.0;
            playHead.loopEnd   = loopEnd;

            for (auto& ins : insertSlots)
                ins.buffer.clear (0, numSamples);

            scheduleMidi (numSamples, sendAllOff, isRunning);
            if (isRunning)
                applyAutomation (blockStartBeat);

            // instruments
            for (auto& c : channelSlots)
            {
                if (c.plugin == nullptr)
                    continue;
                juce::AudioBuffer<float> view (c.buffer.getArrayOfWritePointers(), c.channels, numSamples);
                view.clear();
                c.plugin->processBlock (view, c.midi);

                auto& dest = insertSlots[(size_t) juce::jlimit (0, kNumInserts - 1, c.insert.load())].buffer;
                const int outs = juce::jlimit (1, c.channels, c.plugin->getTotalNumOutputChannels());

                if (c.sumOutputs.load() && outs > 2)
                {
                    // Fold every output bus down into stereo
                    for (int ch = 0; ch < outs; ++ch)
                        dest.addFrom (ch & 1, 0, view, ch, 0, numSamples);
                }
                else
                {
                    dest.addFrom (0, 0, view, 0, 0, numSamples);
                    dest.addFrom (1, 0, view, std::min (1, outs - 1), 0, numSamples);
                }
            }

            if (isRunning)
                renderAudioClips (numSamples);

            // inserts into master
            auto& master = insertSlots[0].buffer;
            for (int i = 1; i < kNumInserts; ++i)
            {
                processInsert (i, numSamples);
                master.addFrom (0, 0, insertSlots[(size_t) i].buffer, 0, 0, numSamples);
                master.addFrom (1, 0, insertSlots[(size_t) i].buffer, 1, 0, numSamples);
            }
            processInsert (0, numSamples);

            if (outL != nullptr) std::copy_n (master.getReadPointer (0), numSamples, outL);
            if (outR != nullptr) std::copy_n (master.getReadPointer (1), numSamples, outR);
        }
    }

    // ---- preview and metronome (after the master, so mastering FX don't touch them) ----
    if (outL != nullptr)
    {
        renderPreview (outL, outR, numSamples);
        for (int i = 0; i < n; ++i)
        {
            outL[i] += clickBuffer[(size_t) i];
            if (outR != nullptr)
                outR[i] += clickBuffer[(size_t) i];
        }
    }

    beatPosition.store (position);
}

void AudioEngine::stopTrackedNotes (int slot, bool includeLive, bool includeClips)
{
    auto& midi = channelSlots[(size_t) slot].midi;

    if (includeClips)
    {
        auto* row = clipNotes.data() + (size_t) slot * 128;
        for (int note = 0; note < 128; ++note)
            if (row[note] != 0)
            {
                midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);
                row[note] = 0;
            }
    }

    if (includeLive)
    {
        auto* rows = liveNotes.data() + (size_t) slot * midiChannels * 128;
        for (int ch = 0; ch < midiChannels; ++ch)
            for (int note = 0; note < 128; ++note)
                if (rows[ch * 128 + note] != 0)
                {
                    midi.addEvent (juce::MidiMessage::noteOff (ch + 1, note), 0);
                    rows[ch * 128 + note] = 0;
                }
    }
}

void AudioEngine::releaseStaleClipNotes (double beat)
{
    for (int slot = 0; slot < kNumChannels; ++slot)
    {
        auto* row = clipNotes.data() + (size_t) slot * 128;
        for (int note = 0; note < 128; ++note)
        {
            if (row[note] == 0)
                continue;

            bool stillHeld = false;
            for (const auto& clip : snapshot.midi)
            {
                if (clip.channel != slot || beat < clip.start || beat >= clip.end)
                    continue;
                for (const auto& n : clip.notes)
                    if (n.note == note && n.on <= beat && n.off > beat)
                    {
                        stillHeld = true;
                        break;
                    }
                if (stillHeld)
                    break;
            }

            if (! stillHeld)
            {
                channelSlots[(size_t) slot].midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);
                row[note] = 0;
            }
        }
    }
}

void AudioEngine::trackLiveMessage (int slot, const juce::MidiMessage& m)
{
    auto* rows = liveNotes.data() + (size_t) slot * midiChannels * 128;
    const int ch = juce::jlimit (1, 16, m.getChannel()) - 1;

    if (m.isNoteOn())
        rows[ch * 128 + m.getNoteNumber()] = 1;
    else if (m.isNoteOff())
        rows[ch * 128 + m.getNoteNumber()] = 0;
    else if (m.isAllNotesOff() || m.isAllSoundOff())
        std::fill_n (rows + ch * 128, 128, (juce::uint8) 0);
}

void AudioEngine::scheduleMidi (int numSamples, bool sendAllOff, bool isRunning)
{
    const bool panicking    = panicRequest.exchange (false);
    const bool releaseAll   = sendAllOff || panicking;
    const bool editedWhilePlaying = snapshotChanged.exchange (false) && isRunning && ! releaseAll;

    for (int slot = 0; slot < kNumChannels; ++slot)
    {
        auto& c = channelSlots[(size_t) slot];
        c.midi.clear();
        if (c.plugin == nullptr)
            continue;

        if (releaseAll)
            stopTrackedNotes (slot, true, true);

        if (releaseAll)
            for (int ch = 1; ch <= 16; ++ch)
            {
                c.midi.addEvent (juce::MidiMessage::controllerEvent (ch, 64, 0), 0);   // sustain pedal up
                c.midi.addEvent (juce::MidiMessage::allNotesOff (ch), 0);
                c.midi.addEvent (juce::MidiMessage::allSoundOff (ch), 0);
            }
    }

    // An edit can delete or shorten a clip whose note is sounding: release
    // anything the new arrangement no longer holds down.
    if (editedWhilePlaying && numSamples > 0)
        releaseStaleClipNotes (blockBeats[0]);

    // Live playing follows the selected channel. If it changes while a note is
    // held, the old channel has to be told, or that note plays forever.
    const int selected = selectedChannel.load();
    if (selected != liveChannel)
    {
        stopTrackedNotes (liveChannel, true, false);
        for (int ch = 1; ch <= 16; ++ch)
            channelSlots[(size_t) liveChannel].midi.addEvent (juce::MidiMessage::allNotesOff (ch), 0);
        liveChannel = selected;
    }

    for (const auto meta : liveMidi)
        trackLiveMessage (selected, meta.getMessage());
    channelSlots[(size_t) selected].midi.addEvents (liveMidi, 0, numSamples, 0);

    if (! isRunning || snapshot.midi.empty())
        return;

    const double beatsPerSample = bpm.load() / 60.0 / sampleRate;

    // Split the block where the loop jumps back
    int segStart = 0;
    for (int i = 1; i <= numSamples; ++i)
    {
        if (i < numSamples && blockBeats[(size_t) i] >= blockBeats[(size_t) i - 1])
            continue;

        const int    s0 = segStart, s1 = i;
        const double a  = blockBeats[(size_t) s0];
        const double b  = blockBeats[(size_t) s1 - 1] + beatsPerSample;
        segStart = i;

        for (const auto& clip : snapshot.midi)
        {
            if (clip.end <= a || clip.start >= b)
                continue;
            auto& slot = channelSlots[(size_t) clip.channel];
            if (slot.plugin == nullptr)
                continue;

            auto sampleFor = [&] (double beat)
            {
                return juce::jlimit (s0, s1 - 1, s0 + (int) ((beat - a) / beatsPerSample));
            };
            auto* active = clipNotes.data() + (size_t) clip.channel * 128;

            for (const auto& note : clip.notes)
            {
                if (note.on >= a && note.on < b)
                {
                    slot.midi.addEvent (juce::MidiMessage::noteOn (1, note.note, note.velocity), sampleFor (note.on));
                    active[note.note] = 1;
                }
                if (note.off >= a && note.off < b)
                {
                    slot.midi.addEvent (juce::MidiMessage::noteOff (1, note.note), sampleFor (note.off));
                    active[note.note] = 0;
                }
            }
        }
    }
}

void AudioEngine::applyAutomation (double beat)
{
    const bool recordingMidi = midiRecording.load();
    const int  recordChannel = midiRecordChannel.load();

    for (const auto& clip : snapshot.midi)
    {
        if (beat < clip.start || beat >= clip.end || clip.lanes.empty())
            continue;
        if (recordingMidi && clip.channel == recordChannel)
            continue;   // don't fight the user while they record

        auto& slot = channelSlots[(size_t) clip.channel];
        if (slot.plugin == nullptr)
            continue;

        const auto& params = slot.plugin->getParameters();
        for (const auto& lane : clip.lanes)
        {
            if (! juce::isPositiveAndBelow (lane.paramIndex, params.size()) || lane.lane.points.empty())
                continue;
            const float v = juce::jlimit (0.0f, 1.0f, lane.lane.valueAt (beat));
            auto& last = slot.lastAuto[(size_t) lane.paramIndex];
            if (std::abs (v - last) < 1.0e-4f)
                continue;
            params[lane.paramIndex]->setValue (v);
            last = v;
        }
    }
}

void AudioEngine::renderAudioClips (int numSamples)
{
    if (snapshot.audio.empty() || numSamples <= 0)
        return;

    const double secondsPerBeat = 60.0 / bpm.load();

    double lo = blockBeats[0], hi = blockBeats[0];
    for (int i = 1; i < numSamples; ++i)
    {
        lo = std::min (lo, blockBeats[(size_t) i]);
        hi = std::max (hi, blockBeats[(size_t) i]);
    }

    for (const auto& clip : snapshot.audio)
    {
        const auto* data = clip.data;
        if (data == nullptr)
            continue;

        const double clipEnd = clip.start + clip.playLength / secondsPerBeat;
        if (clipEnd <= lo || clip.start > hi)
            continue;

        auto& dest = insertSlots[(size_t) juce::jlimit (0, kNumInserts - 1, clip.insert)].buffer;
        auto* left  = dest.getWritePointer (0);
        auto* right = dest.getWritePointer (1);

        const int    frames  = data->audio.getNumSamples();
        const auto*  srcL    = data->audio.getReadPointer (0);
        const auto*  srcR    = data->audio.getReadPointer (std::min (1, data->audio.getNumChannels() - 1));
        const double srcRate = data->sampleRate;

        for (int i = 0; i < numSamples; ++i)
        {
            const double beat = blockBeats[(size_t) i];
            if (beat < clip.start || beat >= clipEnd)
                continue;

            const double seconds = clip.readOffset + (beat - clip.start) * secondsPerBeat * clip.rate;
            const double frame   = seconds * srcRate;
            const int    f0      = (int) frame;
            if (f0 < 0 || f0 >= frames)
                continue;
            const int   f1   = std::min (f0 + 1, frames - 1);
            const float frac = (float) (frame - f0);

            left[i]  += (srcL[f0] + (srcL[f1] - srcL[f0]) * frac) * clip.gain;
            right[i] += (srcR[f0] + (srcR[f1] - srcR[f0]) * frac) * clip.gain;
        }
    }
}

void AudioEngine::processInsert (int index, int numSamples)
{
    auto& slot = insertSlots[(size_t) index];
    auto& ctl  = controls[(size_t) index];

    for (int k = 0; k < kNumFxSlots; ++k)
    {
        auto& fx = slot.fx[(size_t) k];
        if (fx.plugin == nullptr || ctl.bypass[(size_t) k].load())
            continue;

        juce::AudioBuffer<float> view (fx.scratch.getArrayOfWritePointers(), fx.channels, numSamples);
        view.clear();
        const int ins = fx.plugin->getTotalNumInputChannels();
        if (ins > 0)                          view.copyFrom (0, 0, slot.buffer, 0, 0, numSamples);
        if (ins > 1 && fx.channels > 1)       view.copyFrom (1, 0, slot.buffer, 1, 0, numSamples);

        fxMidi.clear();
        fx.plugin->processBlock (view, fxMidi);

        const int outs = std::max (1, fx.plugin->getTotalNumOutputChannels());
        slot.buffer.copyFrom (0, 0, view, 0, 0, numSamples);
        slot.buffer.copyFrom (1, 0, view, std::min (1, outs - 1), 0, numSamples);
    }

    // fader (0.8 = unity) and balance
    const float v    = ctl.volume.load();
    const float gain = ctl.mute.load() ? 0.0f : (v / 0.8f) * (v / 0.8f);
    const float pan  = ctl.pan.load();
    slot.buffer.applyGain (0, 0, numSamples, gain * std::min (1.0f, 1.0f - pan));
    slot.buffer.applyGain (1, 0, numSamples, gain * std::min (1.0f, 1.0f + pan));

    ctl.peakL.store (std::max (slot.buffer.getMagnitude (0, 0, numSamples), ctl.peakL.load() * 0.9f));
    ctl.peakR.store (std::max (slot.buffer.getMagnitude (1, 0, numSamples), ctl.peakR.load() * 0.9f));
}

void AudioEngine::renderPreview (float* left, float* right, int numSamples)
{
    const juce::SpinLock::ScopedTryLockType lock (previewLock);
    if (! lock.isLocked() || previewData == nullptr)
        return;

    const auto* data   = previewData;
    const int   frames = data->audio.getNumSamples();
    const auto* srcL   = data->audio.getReadPointer (0);
    const auto* srcR   = data->audio.getReadPointer (std::min (1, data->audio.getNumChannels() - 1));
    const double step  = data->sampleRate / sampleRate;

    for (int i = 0; i < numSamples; ++i)
    {
        const int f0 = (int) previewPos;
        if (f0 >= frames - 1)
        {
            previewData = nullptr;
            return;
        }
        const float frac = (float) (previewPos - f0);
        left[i] += (srcL[f0] + (srcL[f0 + 1] - srcL[f0]) * frac) * 0.8f;
        if (right != nullptr)
            right[i] += (srcR[f0] + (srcR[f0 + 1] - srcR[f0]) * frac) * 0.8f;
        previewPos += step;
    }
}
