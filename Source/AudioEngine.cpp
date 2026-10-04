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
// Delay compensation
//
// Plugins that look ahead report how many samples they hold back. Left alone,
// a track carrying one plays late against the others, and by a different
// amount every time a plugin is loaded.
//
// Every place signals merge has to be levelled, and sends make that a graph
// rather than two fixed stages: a bus fed by another insert cannot be ready
// before its source is. Because a send may only feed a higher numbered insert,
// one pass from 1 upward resolves it. For each insert:
//
//   inLat  = the latest anything feeding it arrives
//   outLat = inLat + its own effects
//
// Each thing entering an insert is then delayed to that insert's inLat, and
// each insert's output delayed to the master's. Everything therefore reaches
// the master the same distance behind the playhead, which is what
// getPluginLatencySamples() reports.
//
// Must be called with graphLock held.
// ---------------------------------------------------------------------------
void AudioEngine::updateLatency()
{
    auto latencyOf = [] (const juce::AudioPluginInstance* p)
    {
        return p != nullptr ? std::max (0, p->getLatencySamples()) : 0;
    };

    int slowestInstrument = 0;
    for (auto& c : channelSlots)
        if (c.plugin != nullptr)
            slowestInstrument = std::max (slowestInstrument, latencyOf (c.plugin.get()));

    std::array<int, kNumInserts> fxLatency {}, inLat {}, outLat {};

    for (int i = 0; i < kNumInserts; ++i)
    {
        int sum = 0;
        for (int k = 0; k < kNumFxSlots; ++k)
            if (! controls[(size_t) i].bypass[(size_t) k].load())
                sum += latencyOf (insertSlots[(size_t) i].fx[(size_t) k].plugin.get());

        fxLatency[(size_t) i] = sum;
        inLat[(size_t) i] = slowestInstrument;    // instruments and playlist audio
    }

    // Forward pass: a send cannot reach its destination before it has been
    // produced, so each destination waits for the latest of its feeds.
    for (int i = 1; i < kNumInserts; ++i)
    {
        outLat[(size_t) i] = inLat[(size_t) i] + fxLatency[(size_t) i];

        for (int s = 0; s < kNumSends; ++s)
        {
            const int to = controls[(size_t) i].sendTo[(size_t) s].load();
            if (to > i && to < kNumInserts)
                inLat[(size_t) to] = std::max (inLat[(size_t) to], outLat[(size_t) i]);
        }
    }

    int masterIn = 0;
    for (int i = 1; i < kNumInserts; ++i)
        masterIn = std::max (masterIn, outLat[(size_t) i]);

    const int block = std::max (blockSize, 1);
    const int widest = std::max (masterIn, slowestInstrument);

    // Instruments wait for whatever else lands on the same insert. A channel
    // whose buses are split can land on several at once, so each bus waits for
    // its own destination.
    for (auto& c : channelSlots)
    {
        const int own = latencyOf (c.plugin.get());
        const int main = juce::jlimit (0, kNumInserts - 1, c.insert.load());

        c.align.prepare (2, widest, block);
        c.align.setDelay (std::max (0, inLat[(size_t) main] - own));

        if (! c.splitBuses.load())
            continue;

        for (int b = 0; b < kMaxOutBuses; ++b)
        {
            if (c.busChannelCount[(size_t) b] <= 0)
                continue;

            const int routed = c.busInsert[(size_t) b].load();
            const int to = routed > 0 ? juce::jlimit (1, kNumInserts - 1, routed) : main;

            c.busAlign[(size_t) b].prepare (2, widest, block);
            c.busAlign[(size_t) b].setDelay (std::max (0, inLat[(size_t) to] - own));
        }
    }

    for (int i = 1; i < kNumInserts; ++i)
    {
        auto& slot = insertSlots[(size_t) i];

        slot.align.prepare (2, widest, block);
        slot.align.setDelay (std::max (0, masterIn - outLat[(size_t) i]));

        for (int s = 0; s < kNumSends; ++s)
        {
            const int to = controls[(size_t) i].sendTo[(size_t) s].load();
            const int target = (to > i && to < kNumInserts) ? inLat[(size_t) to] : outLat[(size_t) i];

            slot.sendAlign[(size_t) s].prepare (2, widest, block);
            slot.sendAlign[(size_t) s].setDelay (std::max (0, target - outLat[(size_t) i]));
        }
    }

    for (int i = 0; i < kNumInserts; ++i)
        clipDelaySamples[(size_t) i].store (inLat[(size_t) i]);

    totalLatency.store (masterIn + fxLatency[0]);
}

void AudioEngine::setSend (int insertIndex, int sendIndex, int destination, float level)
{
    if (! juce::isPositiveAndBelow (insertIndex, kNumInserts)
        || ! juce::isPositiveAndBelow (sendIndex, kNumSends))
        return;

    // Forward only. Allowing a send backwards would either feed a bus that has
    // already been mixed this block, or close a loop.
    const int to = (destination > insertIndex && destination < kNumInserts) ? destination : 0;

    controls[(size_t) insertIndex].sendTo[(size_t) sendIndex].store (to);
    controls[(size_t) insertIndex].sendLevel[(size_t) sendIndex].store (juce::jlimit (0.0f, 1.0f, level));

    const juce::SpinLock::ScopedLockType lock (graphLock);
    updateLatency();
}

void AudioEngine::setFxBypass (int insertIndex, int slotIndex, bool shouldBypass)
{
    if (! juce::isPositiveAndBelow (insertIndex, kNumInserts)
        || ! juce::isPositiveAndBelow (slotIndex, kNumFxSlots))
        return;

    controls[(size_t) insertIndex].bypass[(size_t) slotIndex].store (shouldBypass);

    const juce::SpinLock::ScopedLockType lock (graphLock);
    updateLatency();
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

    juce::AudioBuffer<float> mixdown (2, bufferCapacity());

    std::unique_ptr<juce::AudioPluginInstance> old;
    {
        const juce::SpinLock::ScopedLockType lock (graphLock);
        auto& slot = channelSlots[(size_t) channel];
        old = std::move (slot.plugin);
        slot.plugin   = std::move (plugin);
        slot.buffer   = std::move (buffer);
        slot.mixdown  = std::move (mixdown);
        slot.channels = channels;
        slot.outputBuses = buses;
        slot.sumOutputs.store (sum);
        slot.splitBuses.store (false);
        slot.lastAuto = std::move (lastAuto);

        // Where each bus sits in the plugin's buffer. Buses are not always
        // stereo and not always in order, so this is asked rather than assumed.
        for (int b = 0; b < kMaxOutBuses; ++b)
        {
            slot.busInsert[(size_t) b].store (0);
            slot.busFirstChannel[(size_t) b] = -1;
            slot.busChannelCount[(size_t) b] = 0;
        }

        if (slot.plugin != nullptr)
            for (int b = 0; b < std::min (buses, kMaxOutBuses); ++b)
                if (auto* bus = slot.plugin->getBus (false, b))
                {
                    slot.busFirstChannel[(size_t) b] =
                        slot.plugin->getChannelIndexInProcessBlockBuffer (false, b, 0);
                    slot.busChannelCount[(size_t) b] = bus->getNumberOfChannels();
                }

        updateLatency();
    }

    if (old != nullptr)
        old->releaseResources();
}

juce::AudioPluginInstance* AudioEngine::getChannelPlugin (int channel) const noexcept
{
    return juce::isPositiveAndBelow (channel, kNumChannels) ? channelSlots[(size_t) channel].plugin.get() : nullptr;
}

void AudioEngine::setChannelInsert (int channel, int insertIndex)
{
    if (! juce::isPositiveAndBelow (channel, kNumChannels))
        return;

    channelSlots[(size_t) channel].insert.store (juce::jlimit (0, kNumInserts - 1, insertIndex));

    // How far this instrument is held back depends on what else arrives at the
    // insert it now feeds.
    const juce::SpinLock::ScopedLockType lock (graphLock);
    updateLatency();
}

void AudioEngine::resetInstruments()
{
    const juce::SpinLock::ScopedLockType lock (graphLock);
    for (auto& c : channelSlots)
    {
        if (c.plugin == nullptr)
            continue;
        c.plugin->suspendProcessing (true);
        c.plugin->reset();
        c.plugin->releaseResources();
        preparePlugin (*c.plugin);
        c.plugin->suspendProcessing (false);
    }
    std::fill (clipNotes.begin(), clipNotes.end(), (juce::uint8) 0);
    std::fill (liveNotes.begin(), liveNotes.end(), (juce::uint8) 0);
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

void AudioEngine::setChannelSplitsBuses (int channel, bool shouldSplit)
{
    if (! juce::isPositiveAndBelow (channel, kNumChannels))
        return;

    channelSlots[(size_t) channel].splitBuses.store (shouldSplit);

    // Each bus may now land on a different insert, so the compensation has to
    // be worked out again.
    const juce::SpinLock::ScopedLockType lock (graphLock);
    updateLatency();
}

bool AudioEngine::getChannelSplitsBuses (int channel) const noexcept
{
    return juce::isPositiveAndBelow (channel, kNumChannels)
        && channelSlots[(size_t) channel].splitBuses.load();
}

void AudioEngine::setBusInsert (int channel, int bus, int insertIndex)
{
    if (! juce::isPositiveAndBelow (channel, kNumChannels)
        || ! juce::isPositiveAndBelow (bus, kMaxOutBuses))
        return;

    channelSlots[(size_t) channel].busInsert[(size_t) bus]
        .store (juce::jlimit (0, kNumInserts - 1, insertIndex));

    const juce::SpinLock::ScopedLockType lock (graphLock);
    updateLatency();
}

int AudioEngine::getBusInsert (int channel, int bus) const noexcept
{
    if (! juce::isPositiveAndBelow (channel, kNumChannels)
        || ! juce::isPositiveAndBelow (bus, kMaxOutBuses))
        return 0;

    return channelSlots[(size_t) channel].busInsert[(size_t) bus].load();
}

juce::String AudioEngine::getBusName (int channel, int bus) const
{
    if (! juce::isPositiveAndBelow (channel, kNumChannels)
        || ! juce::isPositiveAndBelow (bus, kMaxOutBuses))
        return {};

    auto& slot = channelSlots[(size_t) channel];
    if (slot.plugin == nullptr)
        return {};

    if (auto* b = slot.plugin->getBus (false, bus))
    {
        const auto name = b->getName();
        if (name.isNotEmpty())
            return name;
    }

    return "Out " + juce::String (bus + 1);
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

        // Bypass is cleared before the recount, so the new plugin's delay is
        // included rather than measured as though it were switched off.
        controls[(size_t) insertIndex].bypass[(size_t) slotIndex].store (false);
        updateLatency();
    }

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
    pluginMidiCaptured.store (0);
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
    silence.assign ((size_t) cap, 0.0f);

    const juce::SpinLock::ScopedLockType lock (graphLock);
    for (auto& c : channelSlots)
    {
        c.midi.ensureSize (8192);
        c.mixdown.setSize (2, cap, false, true, true);
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
        ins.sendScratch.setSize (2, cap, false, true, true);
        for (auto& f : ins.fx)
            if (f.plugin != nullptr)
            {
                f.plugin->releaseResources();
                preparePlugin (*f.plugin);
                f.scratch.setSize (f.channels, cap);
            }
    }
    capacity = cap;

    loudness.prepare (sampleRate);
    corrLR = corrLL = corrRR = 0.0;

    // Plugins report their latency only once prepared, and the block size just
    // changed, so the delay lines are sized and set here.
    updateLatency();
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
    // Reverb and delay tails decay toward zero and become denormal floats,
    // which some CPUs handle in microcode at around a hundred times the cost.
    // Without this the CPU meter spikes seconds after the sound has stopped.
    juce::ScopedNoDenormals noDenormals;

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

    if (offlineActive.load())
        return;                    // an export owns the graph right now

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
    const double jumpTo = locateRequest.exchange (-1.0e9);
    if (jumpTo > -1.0e8)
    {
        position = jumpTo;
        lastBeat = -1;
        jumped = true;
    }

    const bool   isRunning      = playing.load();
    const double countEnd       = countInEnd.load();
    const bool   counting       = isRunning && position < countEnd;
    if (! counting && countingIn.load())
        cancelCountIn();                       // the count reached the record point
    const bool   clickOn        = metronome.load() || counting;
    const double currentBpm     = bpm.load();
    const double beatsPerSample = currentBpm / 60.0 / sampleRate;
    const bool   recording      = recorder.isActive() || midiRecording.load();
    const bool   sequencing     = isRunning && ! counting;   // clips stay quiet during the count
    const double loopEnd        = songEnd.load();
    const int    slots          = (int) blockBeats.size();
    const int    n              = std::min (numSamples, slots);
    const double blockStartBeat = position;

    // ---- live MIDI (hardware, on-screen piano, typing keyboard) ----
    liveMidi.clear();
    midiCollector.removeNextBlockOfMessages (liveMidi, numSamples);
    keyboardState.processNextMidiBuffer (liveMidi, 0, numSamples, true);

    // ---- audio recording tap ----
    const int recordFrom = recordSource.load();
    bool recordingTapped = false;
    if (sequencing && recordFrom < 0 && numInputChannels > 0)
    {
        recorder.push (inputChannelData[0], numInputChannels > 1 ? inputChannelData[1] : nullptr, numSamples, position);
        recordingTapped = true;
    }

    // ---- playhead, loop, metronome ----
    bool wrapped = false;
    if (n > 0)
        std::fill_n (clickBuffer.begin(), n, 0.0f);

    for (int i = 0; i < numSamples; ++i)
    {
        if (isRunning)
        {
            // Loop at the end of the last clip (not while recording)
            if (! recording && ! counting && loopEnd > 0.0 && position >= loopEnd)
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
    if (midiRecording.load() && sequencing && n > 0)
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

            // After a locate, whatever is sitting in the compensation buffers
            // belongs to the old position, so it is dropped rather than played
            // over the new one. A loop wrap is left alone: that audio really is
            // the continuation of what was just heard.
            if (jumped)
            {
                for (auto& c : channelSlots)
                {
                    c.align.reset();
                    for (auto& busLine : c.busAlign)
                        busLine.reset();
                }
                for (auto& ins : insertSlots)
                {
                    ins.align.reset();
                    for (auto& sendLine : ins.sendAlign)
                        sendLine.reset();
                }
            }

            scheduleMidi (numSamples, sendAllOff, sequencing);
            if (sequencing)
                applyAutomation (blockStartBeat);

            // Locked to the song while it plays, free running while stopped.
            applyModulation (isRunning ? blockStartBeat : modFreeClock);
            modFreeClock += numSamples * beatsPerSample;

            // instruments
            const int recordingChannel = (midiRecording.load() && sequencing) ? midiRecordChannel.load() : -1;
            for (int slotIndex = 0; slotIndex < kNumChannels; ++slotIndex)
            {
                auto& c = channelSlots[(size_t) slotIndex];
                if (c.plugin == nullptr)
                    continue;

                // Remember what goes in, so anything the plugin passes straight
                // through isn't recorded a second time
                sentToPluginCount = 0;
                if (slotIndex == recordingChannel)
                    for (const auto meta : c.midi)
                        if (meta.numBytes >= 2 && sentToPluginCount < (int) sentToPlugin.size())
                            sentToPlugin[(size_t) sentToPluginCount++] =
                                (juce::uint32) ((meta.data[0] << 8) | meta.data[1]);

                juce::AudioBuffer<float> view (c.buffer.getArrayOfWritePointers(), c.channels, numSamples);
                view.clear();
                c.plugin->processBlock (view, c.midi);

                // Notes played inside the plugin's own window (Synplant's branches,
                // arpeggiators, internal sequencers) come back out here
                if (slotIndex == recordingChannel)
                    capturePluginMidi (slotIndex, c.midi, numSamples);

                mixChannelOutput (c, view, numSamples);
            }

            // Recording an instrument's own output: tap it before the mixer
            if (sequencing && recordFrom >= 0 && recorder.isActive()
                && juce::isPositiveAndBelow (recordFrom, kNumChannels))
            {
                auto& source = channelSlots[(size_t) recordFrom].buffer;
                if (source.getNumChannels() >= 2 && source.getNumSamples() >= numSamples)
                {
                    recorder.push (source.getReadPointer (0), source.getReadPointer (1), numSamples, blockStartBeat);
                    recordingTapped = true;
                }
            }

            if (sequencing)
                renderAudioClips (numSamples);

            // Input monitoring goes in here, so it is heard through the
            // insert's effects, but without the compensation delay the other
            // sources get. That delay exists to line internal paths up with
            // each other; on a monitor path it would just be latency the
            // player feels.
            mixMonitorInput (inputChannelData, numInputChannels, numSamples);

            // inserts into master
            auto& master = insertSlots[0].buffer;
            for (int i = 1; i < kNumInserts; ++i)
            {
                processInsert (i, numSamples);

                auto& slot = insertSlots[(size_t) i];

                // Sends are taken post fader and fed forward, so the
                // destination is still ahead in this same pass.
                routeSends (i, numSamples);

                // Then hold this insert back to match the others.
                slot.align.process (slot.buffer, 2, numSamples);

                master.addFrom (0, 0, slot.buffer, 0, 0, numSamples);
                master.addFrom (1, 0, slot.buffer, 1, 0, numSamples);
            }
            processInsert (0, numSamples);

            // Measured here, after every effect: this is what actually leaves
            // the studio, which is what a loudness figure has to describe.
            measureMix (master, numSamples, isRunning);

            if (outL != nullptr) std::copy_n (master.getReadPointer (0), numSamples, outL);
            if (outR != nullptr) std::copy_n (master.getReadPointer (1), numSamples, outR);
        }
    }

    // If the graph was skipped this block, keep the take in time
    if (sequencing && recordFrom >= 0 && recorder.isActive() && ! recordingTapped
        && numSamples <= (int) silence.size())
        recorder.push (silence.data(), silence.data(), numSamples, blockStartBeat);

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

void AudioEngine::capturePluginMidi (int slot, const juce::MidiBuffer& produced, int numSamples)
{
    juce::ignoreUnused (slot);
    const int n = std::min (numSamples, (int) blockBeats.size());
    if (n <= 0)
        return;

    for (const auto meta : produced)
    {
        if (meta.numBytes < 2 || meta.numBytes > 3)
            continue;

        // Skip anything we fed in that the plugin simply passed along
        const auto key = (juce::uint32) ((meta.data[0] << 8) | meta.data[1]);
        bool echoed = false;
        for (int i = 0; i < sentToPluginCount; ++i)
            if (sentToPlugin[(size_t) i] == key)
            {
                sentToPlugin[(size_t) i] = 0xffffffff;   // consume one match
                echoed = true;
                break;
            }
        if (echoed)
            continue;

        const auto message = juce::MidiMessage (meta.data, meta.numBytes, 0.0);
        if (! message.isNoteOnOrOff() && ! message.isController() && ! message.isPitchWheel())
            continue;

        pluginMidiCaptured.fetch_add (1);
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
            if (! params[lane.paramIndex]->isAutomatable())
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

// ---------------------------------------------------------------------------
// Modulation
//
// A modulator rides on top of whatever a parameter is already set to, rather
// than replacing it: the knob still means what it means, and the modulator
// moves it around that point. Several modulators on one parameter add up.
//
// Telling the engine's own writing apart from a knob the user grabbed needs no
// extra plumbing. Before writing, the parameter is compared with what was
// written last block; if it has moved on its own, somebody else set it, and
// that becomes the new resting value. That covers the user turning it, a
// plugin preset loading, and automation, all with one check.
// ---------------------------------------------------------------------------
void AudioEngine::applyModulation (double beat)
{
    if (snapshot.modulators.empty())
        return;

    for (int channel = 0; channel < kNumChannels; ++channel)
    {
        auto& slot = channelSlots[(size_t) channel];
        if (slot.plugin == nullptr)
            continue;

        const auto& params = slot.plugin->getParameters();
        const auto  count  = (size_t) params.size();

        if (slot.modBase.size() != count)
        {
            slot.modBase.assign (count, 0.0f);
            slot.modWritten.assign (count, -1.0f);
            slot.modActive.assign (count, false);
        }

        // Gather this block's offset for every parameter this channel modulates.
        bool anyThisChannel = false;
        for (const auto& m : snapshot.modulators)
        {
            if (! m.enabled)
                continue;

            for (const auto& t : m.targets)
                if (t.channel == channel && juce::isPositiveAndBelow (t.paramIndex, (int) count))
                {
                    const size_t p = (size_t) t.paramIndex;

                    if (! slot.modActive[p])
                    {
                        // First time this parameter is touched: wherever it sits
                        // now is the value to move around.
                        slot.modBase[p] = params[t.paramIndex]->getValue();
                        slot.modActive[p] = true;
                        slot.modWritten[p] = slot.modBase[p];
                    }

                    anyThisChannel = true;
                }
        }

        if (! anyThisChannel)
            continue;

        for (size_t p = 0; p < count; ++p)
        {
            if (! slot.modActive[p])
                continue;

            auto* param = params[(int) p];
            if (! param->isAutomatable())
                continue;

            const float current = param->getValue();
            if (std::abs (current - slot.modWritten[p]) > 1.0e-4f)
                slot.modBase[p] = current;      // somebody else moved it

            float offset = 0.0f;
            for (const auto& m : snapshot.modulators)
            {
                if (! m.enabled)
                    continue;

                for (const auto& t : m.targets)
                    if (t.channel == channel && (size_t) t.paramIndex == p)
                        offset += m.valueAt (beat) * t.depth;
            }

            const float wanted = juce::jlimit (0.0f, 1.0f, slot.modBase[p] + offset);
            if (std::abs (wanted - slot.modWritten[p]) > 1.0e-5f)
            {
                param->setValue (wanted);
                slot.modWritten[p] = wanted;
            }
        }
    }
}

void AudioEngine::renderAudioClips (int numSamples)
{
    if (snapshot.audio.empty() || numSamples <= 0)
        return;

    const double currentBpm     = bpm.load();
    const double secondsPerBeat = 60.0 / currentBpm;

    // Playlist audio has no plugin in front of it, so it would otherwise run
    // ahead of whatever else lands on the same insert. Reading that far back in
    // the arrangement delays it by the same amount, without a delay line. The
    // amount is per insert, because a send can make one bus run later than
    // another.
    const double beatsPerSample = currentBpm / 60.0 / sampleRate;

    double lo = blockBeats[0], hi = lo;
    for (int i = 1; i < numSamples; ++i)
    {
        lo = std::min (lo, blockBeats[(size_t) i]);
        hi = std::max (hi, blockBeats[(size_t) i]);
    }

    int widestClipDelay = 0;
    for (auto& d : clipDelaySamples)
        widestClipDelay = std::max (widestClipDelay, d.load());
    lo -= widestClipDelay * beatsPerSample;

    for (const auto& clip : snapshot.audio)
    {
        const auto* data = clip.data;
        if (data == nullptr)
            continue;

        const double clipEnd = clip.start + clip.playLength / secondsPerBeat;
        if (clipEnd <= lo || clip.start > hi)
            continue;

        const int insertIndex = juce::jlimit (0, kNumInserts - 1, clip.insert);
        const double shift = clipDelaySamples[(size_t) insertIndex].load() * beatsPerSample;

        auto& dest = insertSlots[(size_t) insertIndex].buffer;
        auto* left  = dest.getWritePointer (0);
        auto* right = dest.getWritePointer (1);

        const int    frames  = data->audio.getNumSamples();
        const auto*  srcL    = data->audio.getReadPointer (0);
        const auto*  srcR    = data->audio.getReadPointer (std::min (1, data->audio.getNumChannels() - 1));
        const double srcRate = data->sampleRate;

        // Warped clips read along the piecewise line their markers describe.
        // The segment table is precomputed on the message thread, so the cost
        // here is one comparison and one multiply-add per sample, and the cursor
        // means a block that plays forward does not search at all. It is
        // corrected rather than trusted, so looping back is safe.
        // The range is checked rather than trusted. It costs one test per clip
        // per block, and the alternative is reading off the end of the table on
        // the audio thread, which takes the whole studio down.
        const bool warpOk = clip.warpCount > 0 && clip.warpFirst >= 0
                         && (size_t) clip.warpFirst + (size_t) clip.warpCount <= snapshot.warp.size();
        const WarpSegment* warp = warpOk ? snapshot.warp.data() + clip.warpFirst : nullptr;
        int warpCursor = 0;

        for (int i = 0; i < numSamples; ++i)
        {
            const double beat = blockBeats[(size_t) i] - shift;
            if (beat < clip.start || beat >= clipEnd)
                continue;

            const double seconds = warp != nullptr
                                 ? warpSourceAtSegment (warp, clip.warpCount, warpCursor, beat)
                                 : clip.readOffset + (beat - clip.start) * secondsPerBeat * clip.rate;
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

// ---------------------------------------------------------------------------
/** Moves one instrument's output into the mixer.

    Normally everything the plugin produced is folded to stereo and added to
    the channel's insert. When the channel is set to split, each output bus
    goes to its own insert instead, which is what gives every drum in a machine
    like Microtonic its own strip, fader and effects.

    Shared by playback and export, so a bounce cannot route differently from
    what was heard.
*/
// ---------------------------------------------------------------------------
/** Passes the interface's input through to an insert, so the player hears
    themselves through the studio's own effects.

    Deliberately not delay compensated. That compensation lines internal paths
    up with each other; on a monitor path it would only add latency the player
    feels directly, and the recorder already lines the take itself up with the
    arrangement.

    The level ramps rather than switching, because turning monitoring on or off
    mid-performance would otherwise click, and a click into a guitar amp
    simulator is loud.
*/
void AudioEngine::mixMonitorInput (const float* const* inputChannelData, int numInputChannels,
                                   int numSamples)
{
    const auto mode = monitorMode.load();
    const bool wanted = numInputChannels > 0
                     && (mode == Monitor::always
                         || (mode == Monitor::armed && (inputArmed.load() || recorder.isActive())));

    // About five milliseconds either way, which is short enough to feel
    // immediate and long enough not to click.
    const float step = (float) (1.0 / std::max (1.0, sampleRate * 0.005));
    const float target = wanted ? 1.0f : 0.0f;

    if (monitorRamp <= 0.0f && target <= 0.0f)
        return;

    auto& dest = insertSlots[(size_t) juce::jlimit (0, kNumInserts - 1, monitorInsert.load())].buffer;
    const float gain = monitorGain.load();

    const auto* left  = inputChannelData[0];
    const auto* right = numInputChannels > 1 ? inputChannelData[1] : inputChannelData[0];

    if (left == nullptr)
        return;

    if (right == nullptr)
        right = left;

    auto* outL = dest.getWritePointer (0);
    auto* outR = dest.getWritePointer (1);

    for (int i = 0; i < numSamples; ++i)
    {
        if (monitorRamp < target)      monitorRamp = std::min (target, monitorRamp + step);
        else if (monitorRamp > target) monitorRamp = std::max (target, monitorRamp - step);

        const float level = monitorRamp * gain;
        outL[i] += left[i]  * level;
        outR[i] += right[i] * level;
    }

    monitoring.store (monitorRamp > 0.0f);
}

void AudioEngine::mixChannelOutput (ChannelSlot& c, const juce::AudioBuffer<float>& view,
                                    int numSamples)
{
    const int outs = juce::jlimit (1, c.channels, c.plugin->getTotalNumOutputChannels());
    const int main = juce::jlimit (0, kNumInserts - 1, c.insert.load());

    juce::AudioBuffer<float> folded (c.mixdown.getArrayOfWritePointers(), 2, numSamples);

    if (c.splitBuses.load() && c.outputBuses > 1)
    {
        for (int b = 0; b < std::min (c.outputBuses, kMaxOutBuses); ++b)
        {
            const int first = c.busFirstChannel[(size_t) b];
            const int count = c.busChannelCount[(size_t) b];

            if (first < 0 || count <= 0 || first >= outs)
                continue;

            folded.clear();

            // A mono bus is heard in both ears rather than only the left.
            folded.addFrom (0, 0, view, first, 0, numSamples);
            folded.addFrom (1, 0, view, std::min (first + (count > 1 ? 1 : 0), outs - 1), 0, numSamples);

            c.busAlign[(size_t) b].process (folded, 2, numSamples);

            const int routed = c.busInsert[(size_t) b].load();
            const int to = routed > 0 ? juce::jlimit (1, kNumInserts - 1, routed) : main;

            auto& dest = insertSlots[(size_t) to].buffer;
            dest.addFrom (0, 0, folded, 0, 0, numSamples);
            dest.addFrom (1, 0, folded, 1, 0, numSamples);
        }

        return;
    }

    folded.clear();

    if (c.sumOutputs.load() && outs > 2)
    {
        // Fold every output bus down into stereo
        for (int ch = 0; ch < outs; ++ch)
            folded.addFrom (ch & 1, 0, view, ch, 0, numSamples);
    }
    else
    {
        folded.addFrom (0, 0, view, 0, 0, numSamples);
        folded.addFrom (1, 0, view, std::min (1, outs - 1), 0, numSamples);
    }

    c.align.process (folded, 2, numSamples);

    auto& dest = insertSlots[(size_t) main].buffer;
    dest.addFrom (0, 0, folded, 0, 0, numSamples);
    dest.addFrom (1, 0, folded, 1, 0, numSamples);
}

void AudioEngine::measureMix (const juce::AudioBuffer<float>& master, int numSamples, bool isRunning)
{
    if (analysisReset.exchange (false))
    {
        loudness.prepare (sampleRate);
        corrLR = corrLL = corrRR = 0.0;
    }

    // Only while the transport rolls, so an idle session does not quietly
    // average itself down toward silence.
    if (! isRunning || numSamples <= 0 || master.getNumChannels() < 2)
    {
        analysisRunning.store (false);
        return;
    }

    analysisRunning.store (true);
    loudness.process (master.getArrayOfReadPointers(), 2, numSamples);

    // Correlation between the two channels, which is what tells you whether a
    // mix will survive being folded to mono.
    const auto* l = master.getReadPointer (0);
    const auto* r = master.getReadPointer (1);

    for (int i = 0; i < numSamples; ++i)
    {
        corrLR += (double) l[i] * r[i];
        corrLL += (double) l[i] * l[i];
        corrRR += (double) r[i] * r[i];
    }

    // Decay, so the figure follows the music rather than the whole session.
    const double keep = std::pow (0.5, numSamples / (sampleRate * 2.0));
    corrLR *= keep; corrLL *= keep; corrRR *= keep;

    const double denominator = std::sqrt (corrLL * corrRR);
    analysisCorrelation.store (denominator > 1.0e-12 ? (float) (corrLR / denominator) : 1.0f);

    analysisIntegrated.store (loudness.getIntegratedLufs());
    analysisShortTerm.store (loudness.getShortTermLufs());
    analysisMomentary.store (loudness.getMomentaryLufs());
    analysisRange.store (loudness.getLoudnessRange());
    analysisTruePeak.store (loudness.getTruePeak());
}

AudioEngine::MixReading AudioEngine::getMixReading() const
{
    MixReading r;
    r.integratedLufs = analysisIntegrated.load();
    r.shortTermLufs  = analysisShortTerm.load();
    r.momentaryLufs  = analysisMomentary.load();
    r.loudnessRange  = analysisRange.load();
    r.truePeak       = analysisTruePeak.load();
    r.correlation    = analysisCorrelation.load();
    r.measuring      = analysisRunning.load();
    return r;
}

void AudioEngine::routeSends (int index, int numSamples)
{
    auto& slot = insertSlots[(size_t) index];
    auto& ctl  = controls[(size_t) index];

    for (int s = 0; s < kNumSends; ++s)
    {
        const int   to    = ctl.sendTo[(size_t) s].load();
        const float level = ctl.sendLevel[(size_t) s].load();

        // Forward only, so the destination has not been mixed yet this block.
        if (to <= index || to >= kNumInserts || level <= 0.0f)
            continue;

        if (slot.sendScratch.getNumChannels() < 2 || slot.sendScratch.getNumSamples() < numSamples)
            continue;

        juce::AudioBuffer<float> copy (slot.sendScratch.getArrayOfWritePointers(), 2, numSamples);
        copy.copyFrom (0, 0, slot.buffer, 0, 0, numSamples);
        copy.copyFrom (1, 0, slot.buffer, 1, 0, numSamples);
        copy.applyGain (level);

        // A bus fed from here runs at least as late as this insert, so the send
        // waits for whatever else arrives there.
        slot.sendAlign[(size_t) s].process (copy, 2, numSamples);

        auto& dest = insertSlots[(size_t) to].buffer;
        dest.addFrom (0, 0, copy, 0, 0, numSamples);
        dest.addFrom (1, 0, copy, 1, 0, numSamples);
    }
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


// ---------------------------------------------------------------------------
// Offline export

bool AudioEngine::renderOffline (double fromBeat, double toBeat, double tailSeconds,
                                 const std::vector<int>& stemInserts,
                                 const RenderSink& sink,
                                 const std::function<bool (double)>& progress)
{
    if (capacity <= 0 || blockSize <= 0 || sampleRate <= 0.0)
        return false;

    // Same reason as the live callback: decaying tails would otherwise make an
    // export crawl on CPUs that trap denormals.
    juce::ScopedNoDenormals noDenormals;

    offlineActive.store (true);
    juce::Thread::sleep (60);                 // let the device callback fall quiet

    const juce::SpinLock::ScopedLockType lock (graphLock);

    // Remember the live transport so the session is undisturbed afterwards
    const double savedPosition = position;
    const bool   savedPlaying  = playing.load();
    const auto   savedLastBeat = lastBeat;

    playing.store (true);
    position = fromBeat;
    lastBeat = -1;
    liveMidi.clear();
    panicRequest.store (true);                // start from silence

    // Compensation buffers hold audio from the live session; an export starts
    // clean.
    for (auto& c : channelSlots)
    {
        c.align.reset();
        for (auto& busLine : c.busAlign)
            busLine.reset();
    }
    for (auto& ins : insertSlots)
    {
        ins.align.reset();
        for (auto& sendLine : ins.sendAlign)
            sendLine.reset();
    }

    const int    n              = std::min (blockSize, capacity);
    const double beatsPerSample = bpm.load() / 60.0 / sampleRate;
    const double tailBlocks     = std::ceil (tailSeconds * sampleRate / n);
    const double totalBeats     = std::max (1.0e-6, toBeat - fromBeat);

    bool ok = true;
    int  tailLeft = -1;

    while (true)
    {
        const bool sequencing = position < toBeat;
        if (! sequencing)
        {
            if (tailLeft < 0)
                tailLeft = (int) tailBlocks;
            if (tailLeft-- <= 0)
                break;
        }

        const double blockStart = position;
        for (int i = 0; i < n; ++i)
        {
            blockBeats[(size_t) i] = position;
            position += beatsPerSample;
        }

        playHead.bpm       = bpm.load();
        playHead.ppq       = blockStart;
        playHead.seconds   = blockStart * 60.0 / playHead.bpm;
        playHead.samples   = (juce::int64) (playHead.seconds * sampleRate);
        playHead.playing   = sequencing;
        playHead.recording = false;
        playHead.looping   = false;

        for (auto& ins : insertSlots)
            ins.buffer.clear (0, n);

        scheduleMidi (n, false, sequencing);
        if (sequencing)
            applyAutomation (blockStart);
        applyModulation (blockStart);

        for (int slotIndex = 0; slotIndex < kNumChannels; ++slotIndex)
        {
            auto& c = channelSlots[(size_t) slotIndex];
            if (c.plugin == nullptr)
                continue;

            juce::AudioBuffer<float> view (c.buffer.getArrayOfWritePointers(), c.channels, n);
            view.clear();
            c.plugin->processBlock (view, c.midi);

            // The export must route and line up exactly as playback does, or a
            // bounce would not match what was heard.
            mixChannelOutput (c, view, n);
        }

        if (sequencing)
            renderAudioClips (n);

        auto& master = insertSlots[0].buffer;
        for (int i = 1; i < kNumInserts; ++i)
        {
            processInsert (i, n);

            auto& slot = insertSlots[(size_t) i];
            routeSends (i, n);
            slot.align.process (slot.buffer, 2, n);

            master.addFrom (0, 0, slot.buffer, 0, 0, n);
            master.addFrom (1, 0, slot.buffer, 1, 0, n);

            if (std::find (stemInserts.begin(), stemInserts.end(), i) != stemInserts.end())
                sink (i, slot.buffer.getArrayOfReadPointers(), n);
        }
        processInsert (0, n);
        sink (-1, master.getArrayOfReadPointers(), n);

        if (progress && ! progress (juce::jlimit (0.0, 1.0, (position - fromBeat) / totalBeats)))
        {
            ok = false;
            break;
        }
    }

    // Leave the session exactly as it was
    panicRequest.store (true);
    position = savedPosition;
    lastBeat = savedLastBeat;
    playing.store (savedPlaying);
    playHead.playing = savedPlaying;

    offlineActive.store (false);
    return ok;
}
