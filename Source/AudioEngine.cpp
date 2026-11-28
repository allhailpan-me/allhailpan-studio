#include "AudioEngine.h"
#include "AudioDefaults.h"
#include "LatencyGraph.h"
#include "SoloGraph.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

AudioEngine::AudioEngine()
{
    for (int i = 0; i < kNumInserts; ++i)
        controls[(size_t) i].name = i == 0 ? juce::String ("Master") : "Insert " + juce::String (i);
    for (int c = 0; c < kNumChannels; ++c)
        channelSlots[(size_t) c].insert.store (c + 1);

    // An atomic<float> default constructs to zero and zero here is silence,
    // so this is not tidiness: without it the mixer would be mute until the
    // first graph update, and the first graph update happens when the device
    // opens, which is after this.
    for (auto& g : soloGain)
        g.store (1.0f);

    // Same argument, and the same consequence if it is left out: a pre fader
    // send would be silent until the first graph update, which is when the
    // device opens.
    for (auto& g : preFaderSendGain)
        g.store (1.0f);

    // Derived in Tuner.h rather than written down here, because the ring's
    // capacity requirement is a correctness requirement and the two numbers it
    // relates used to meet in this file, which no test can compile. It comes
    // out at about half a second at 192 kHz, the longest half second the
    // studio can be asked for, so the ring never has to be resized once the
    // audio thread may be writing to it.
    // The block bound goes with it, because the tear check subtracts the block
    // the write counter is published too late to describe. The same constant
    // the capacity is derived from, so the two cannot drift apart here the way
    // the capacity and the requirement once did.
    tunerRing.setSize (Tuner::studioRingCapacity(), Tuner::largestStudioBlockSamples);

    // The same discipline for the spectrum's ring, and for the same reason:
    // the capacity and the requirement it comes from are both in
    // SpectrumAnalyser.h, where a test can compile them together. The block
    // bound is the tuner's, because it is a property of the device callback
    // rather than of either display, doubled because this ring holds the two
    // legs interleaved.
    spectrumRing.setSize (Spectrum::interleavedRingCapacity (Tuner::largestStudioBlockSamples),
                          2 * Tuner::largestStudioBlockSamples);

    // Sized here and never again, because the audio thread interleaves through
    // it on every block and must not allocate.
    spectrumInterleave.assign ((std::size_t) (2 * Tuner::largestStudioBlockSamples), 0.0f);
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
    {
        // First run only. After this the saved state is either the user's own
        // choice or the choice made here on a previous launch, and overriding
        // it every time would mean a setting that will not stay set.
        chooseLowLatencyDevice();

        // A reconfiguration may have opened a device where the first attempt
        // could not, so what initialise said about it is out of date.
        if (deviceManager.getCurrentAudioDevice() != nullptr)
            error.clear();

        for (auto& d : juce::MidiInput::getAvailableDevices())
            deviceManager.setMidiInputDeviceEnabled (d.identifier, true);
    }

    deviceManager.addMidiInputDeviceCallback ({}, &midiCollector);
    deviceManager.addAudioCallback (this);
    return error;
}

double AudioEngine::monitoringRoundTripMs() const
{
    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
        return 0.0;

    // Estimated from the buffer when the driver reports nothing, rather than
    // read as zero. See AudioDefaults::roundTripOrEstimate: an ASIO driver
    // whose getLatencies call fails leaves both figures at zero, and zero is
    // the best possible score.
    return AudioDefaults::roundTripOrEstimate (device->getInputLatencyInSamples(),
                                               device->getOutputLatencyInSamples(),
                                               device->getCurrentBufferSizeSamples(),
                                               device->getCurrentSampleRate());
}

bool AudioEngine::monitoringLatencyIsEstimated() const
{
    auto* device = deviceManager.getCurrentAudioDevice();

    return device != nullptr
        && device->getInputLatencyInSamples() + device->getOutputLatencyInSamples() <= 0;
}

namespace
{
    /** Whether ASIO was compiled in.

        Worth being able to say out loud. When it is not, JUCE does not
        register the type at all, so a search for it looks exactly like a
        machine that has no ASIO driver installed. Those are completely
        different problems with completely different answers, and telling them
        apart from the outside is impossible, which is how an interface's own
        driver can sit unused on somebody's machine for days while the studio
        reports that it looked and found nothing. */
    bool builtWithAsio() noexcept
    {
       #if JUCE_ASIO
        return true;
       #else
        return false;
       #endif
    }
}

juce::String AudioEngine::findFastestDevice()
{
    chooseLowLatencyDevice();

    if (deviceManager.getCurrentAudioDevice() == nullptr)
        return "No audio device would open. Pick one above.";

    return deviceSearchReport();
}

/** What the search tried and what each one gave.

    This exists because the last three attempts at this problem were all made
    without knowing what the machine had actually done. The studio reported
    one number, that number was wrong or irrelevant, and the next change was a
    guess. A list of what was tried turns "still slow" into something that can
    be read off a screenshot in one go, which is the difference between
    diagnosing this and iterating at it.
*/
juce::String AudioEngine::deviceSearchReport() const
{
    const juce::String now = deviceManager.getCurrentAudioDeviceType();

    juce::String report;
    report << "Monitoring is " << juce::String (monitoringRoundTripMs(), 1)
           << " ms round trip on " << now << ".";

    if (monitoringLatencyIsEstimated())
        report << " That is estimated from the buffer size, because this driver does not "
                  "report its own latency.";

    if (hasTakenTheDevice())
    {
        report << "\nThe sound card is held by the studio, so other applications will not "
                  "play through it while this is open.";

        if (searchChangedTheDeviceType)
            report << " Nothing that shares it was fast enough.";

        report << " Change Audio device type above to undo that.";
    }

   #if JUCE_WINDOWS
    // Said whether or not a search has run, because it is the first thing
    // worth knowing on a machine with an audio interface: without ASIO the
    // studio cannot reach the interface's own driver at all, whatever else it
    // tries.
    report << (builtWithAsio()
                 ? "\nThis build includes ASIO, so an interface's own driver can be used."
                 : "\nThis build has no ASIO, so an interface's own driver cannot be used.");
   #endif

    if (deviceSearch.isEmpty())
        return report;

    report << "\n\nWhat was tried:";

    for (const auto& line : deviceSearch)
        report << "\n  " << line;

    return report;
}

// ---------------------------------------------------------------------------
// Choosing a driver and a buffer size on first run.
//
// This is here because of a bug report that was not a settings problem: a
// guitarist turned on monitoring, could not play through the delay, and had
// no reason to know that the answer was a driver dropdown two menus away.
// Whatever the operating system hands out by default is tuned for playing
// back a video without glitching, not for hearing yourself in time, and on
// Windows the gap between the two is tens of milliseconds.
//
// The order of operations matters. Nothing is touched unless the default is
// actually too slow to play through, because on a machine that is already
// fast, changing the device is all risk and no gain. Switching driver type
// costs a second and a half inside JUCE, which closes the device and waits
// for the operating system to let go of it, so each attempt stops as soon as
// it has an answer rather than measuring them all.

void AudioEngine::chooseLowLatencyDevice()
{
    deviceSearch.clear();
    searchChangedTheDeviceType = false;

    // The buffer size is worth tightening whatever driver we are on, and it
    // is the cheap half: no device switch, so no wait.
    tightenBufferSize();

    const juce::String fallbackType = deviceManager.getCurrentAudioDeviceType();

    if (deviceManager.getCurrentAudioDevice() != nullptr)
    {
        const double current = monitoringRoundTripMs();

        note (fallbackType, current, "where the system had it");

        if (AudioDefaults::playable (current))
            return;
    }

    double       bestRoundTrip = deviceManager.getCurrentAudioDevice() != nullptr
                                   ? monitoringRoundTripMs()
                                   : std::numeric_limits<double>::max();
    juce::String bestType      = fallbackType;
    bool         bestTakesIt   = AudioDefaults::takesTheDevice (fallbackType.toStdString());

    for (const auto& candidate : AudioDefaults::searchOrder())
    {
        const juce::String typeName (candidate.name);

        // Where the search started. Measured above when a device was open, and
        // reached again by the fallback at the end when one was not, so there
        // is nothing to gain from closing and reopening it here.
        if (typeName == fallbackType)
            continue;

        // Only types the machine actually has, and only ones with a device on
        // them: an empty type would be switched to and then fail to open,
        // which costs the wait for nothing.
        const bool present = [this, &typeName]
        {
            for (auto* type : deviceManager.getAvailableDeviceTypes())
                if (type->getTypeName() == typeName)
                    return ! type->getDeviceNames (false).isEmpty()
                        || ! type->getDeviceNames (true).isEmpty();

            return false;
        }();

        if (! present)
        {
           #if JUCE_WINDOWS
            const bool missingFromBuild = typeName == "ASIO" && ! builtWithAsio();
           #else
            // ASIO is a Windows interface. Saying "not compiled in" on a Mac
            // would imply a build option that does not exist.
            const bool missingFromBuild = false;
           #endif

            note (typeName, -1.0, missingFromBuild ? "not compiled into this build"
                                                   : "not on this machine");
            continue;
        }

        if (! openDeviceType (typeName))
        {
            note (typeName, -1.0, "would not open");
            continue;
        }

        tightenBufferSize();

        const double roundTrip = monitoringRoundTripMs();

        note (typeName, roundTrip, candidate.takesTheDevice ? "takes the sound card" : "");

        // A driver that holds the device only wins if it is playable and
        // nothing which shares the device was. Being merely faster is not
        // enough to pay that price: the loop reaches these last, so anything
        // already found was both sharing and not good enough.
        const bool worthTaking = ! candidate.takesTheDevice
                               || AudioDefaults::playable (roundTrip);

        if (roundTrip > 0.0 && roundTrip < bestRoundTrip && worthTaking)
        {
            bestRoundTrip = roundTrip;
            bestType      = typeName;
            bestTakesIt   = candidate.takesTheDevice;
        }

        if (AudioDefaults::playable (roundTrip))
            break;                     // good enough: stop paying for attempts
    }

    // Settle on whichever was least bad, which can be the driver we started
    // on, in which case the machine simply does not go faster and the report
    // says so in milliseconds.
    if (deviceManager.getCurrentAudioDeviceType() != bestType
        || deviceManager.getCurrentAudioDevice() == nullptr)
    {
        openDeviceType (bestType);
        tightenBufferSize();
    }

    // Whatever happened above, the studio must not be left without a device.
    if (deviceManager.getCurrentAudioDevice() == nullptr)
    {
        openDeviceType (fallbackType);
        bestTakesIt = AudioDefaults::takesTheDevice (fallbackType.toStdString());

        if (deviceManager.getCurrentAudioDevice() == nullptr)
            deviceManager.initialise (8, 8, nullptr, true);
    }

    // Deliberately read back off the device manager rather than carried along
    // in bestTakesIt, which an audit found could be false while the studio was
    // in fact holding the sound card: setCurrentAudioDeviceType does nothing
    // at all when handed a name it does not recognise, so the last type the
    // loop opened can still be the live one. Being wrong in that direction is
    // the one that hurts, because then nothing tells the person why the rest
    // of their machine went quiet.
    searchChangedTheDeviceType = deviceManager.getCurrentAudioDeviceType() != fallbackType;
    juce::ignoreUnused (bestTakesIt);
}

/** One line of the record of what was tried. A round trip below zero means it
    was never measured, and the reason says why. */
void AudioEngine::note (const juce::String& typeName, double roundTrip, const juce::String& why)
{
    juce::String line;
    line << typeName << ": ";

    if (roundTrip < 0.0)
        line << why;
    else
        line << juce::String (roundTrip, 1) << " ms"
             << (why.isNotEmpty() ? ", " + why : juce::String());

    deviceSearch.add (line);
}

bool AudioEngine::openDeviceType (const juce::String& typeName)
{
    if (deviceManager.getCurrentAudioDeviceType() != typeName)
        deviceManager.setCurrentAudioDeviceType (typeName, true);

    // setCurrentAudioDeviceType does nothing at all when the name it is given
    // is already the current one, and it reports nothing when the device it
    // then tries to open fails. Both of those leave the type selected with no
    // device behind it, and initialise is the only public call that fills in
    // default device names for the current type and opens them.
    if (deviceManager.getCurrentAudioDevice() == nullptr)
        deviceManager.initialise (8, 8, nullptr, true);

    return deviceManager.getCurrentAudioDeviceType() == typeName
        && deviceManager.getCurrentAudioDevice() != nullptr;
}

void AudioEngine::tightenBufferSize()
{
    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
        return;

    const double rate = device->getCurrentSampleRate();

    if (rate <= 0.0)
        return;

    std::vector<int> sizes;

    for (int size : device->getAvailableBufferSizes())
        sizes.push_back (size);

    const int wanted = AudioDefaults::chooseBufferSize (sizes, rate);

    if (wanted == 0 || wanted == device->getCurrentBufferSizeSamples())
        return;

    const auto previous = deviceManager.getAudioDeviceSetup();
    auto       tighter  = previous;
    tighter.bufferSize  = wanted;

    // A driver can list a size and then refuse it. Putting the working setup
    // back is better than leaving the device closed over a buffer size.
    if (deviceManager.setAudioDeviceSetup (tighter, true).isNotEmpty()
        || deviceManager.getCurrentAudioDevice() == nullptr)
        deviceManager.setAudioDeviceSetup (previous, true);
}

RecordAlign::Figures AudioEngine::getRecordFigures() const
{
    RecordAlign::Figures f;
    f.engineLatency = totalLatency.load();
    f.trimMs        = recordTrimMs.load();
    f.sampleRate    = sampleRate;

    if (auto* d = deviceManager.getCurrentAudioDevice())
    {
        f.inputLatency  = d->getInputLatencyInSamples();
        f.outputLatency = d->getOutputLatencyInSamples();
        f.bufferSize    = d->getCurrentBufferSizeSamples();

        if (d->getCurrentSampleRate() > 0.0)
            f.sampleRate = d->getCurrentSampleRate();
    }

    return f;
}

int AudioEngine::getRecordOffsetSamples() const
{
    return RecordAlign::captureOffsetSamples (getRecordFigures());
}


// ---------------------------------------------------------------------------
// Delay compensation
//
// The arithmetic is in LatencyGraph.h, which carries no JUCE and has its own
// test. It was here, and that was the wrong place for it: this is the
// calculation in the studio least able to announce that it is wrong. Nothing
// errors and nothing crackles, tracks just sit a few milliseconds apart by an
// amount that depends on which plugins are loaded. The bug that moved it out
// had a comment above it claiming the opposite of what the code did.
//
// So what is left here is only the three things that genuinely need JUCE:
// reading each plugin's reported latency, reading the routing out of the
// atomics, and pushing the answers into the delay lines. No decisions.
//
// Must be called with graphLock held.
// ---------------------------------------------------------------------------
void AudioEngine::updateLatency()
{
    auto latencyOf = [] (const juce::AudioPluginInstance* p)
    {
        return p != nullptr ? std::max (0, p->getLatencySamples()) : 0;
    };

    // latencySetup and latencyGraph are members, reused. Everything below is
    // resize or assign on vectors that are already the right size after the
    // first call, so nothing here allocates while graphLock is held.
    auto& setup = latencySetup;

    setup.inserts.resize ((size_t) kNumInserts);
    setup.channels.resize ((size_t) kNumChannels);

    for (int i = 0; i < kNumInserts; ++i)
    {
        auto& entry = setup.inserts[(size_t) i];

        int sum = 0;
        for (int k = 0; k < kNumFxSlots; ++k)
            if (! controls[(size_t) i].bypass[(size_t) k].load())
                sum += latencyOf (insertSlots[(size_t) i].fx[(size_t) k].plugin.get());

        entry.fxLatency = sum;

        // An insert carrying a live input is exempt from compensation, and
        // several can be at once: recording a band is a microphone on each of
        // several inserts, and every one of those players has to be able to
        // play. Deliberately not conditioned on the per insert arm, which
        // changes without a graph update: being exempt when it did not have
        // to be costs the alignment of other material on that insert, which
        // is the trade already documented for monitoring, while being
        // compensated when it should not be is unplayable latency.
        entry.monitored = i >= 1
                       && ! exportingFullyCompensated
                       && monitorMode.load() != Monitor::off
                       && InputSource::assignmentFromStored (insertInput[(size_t) i].load()).assigned;

        // Insert 0 is the master and has no send of its own to resolve, so
        // its row is left empty rather than gathered and ignored.
        entry.sendTo.resize (i == 0 ? 0 : (size_t) kNumSends);

        for (int s = 0; s < (int) entry.sendTo.size(); ++s)
            entry.sendTo[(size_t) s] = controls[(size_t) i].sendTo[(size_t) s].load();
    }

    for (int c = 0; c < kNumChannels; ++c)
    {
        auto& source  = channelSlots[(size_t) c];
        auto& channel = setup.channels[(size_t) c];

        channel.latency    = latencyOf (source.plugin.get());
        channel.insert     = source.insert.load();
        channel.splitBuses = source.splitBuses.load();

        if (! channel.splitBuses)
            continue;

        channel.busInsert.resize ((size_t) kMaxOutBuses);
        channel.busChannels.resize ((size_t) kMaxOutBuses);

        for (int b = 0; b < kMaxOutBuses; ++b)
        {
            channel.busInsert[(size_t) b]  = source.busInsert[(size_t) b].load();
            channel.busChannels[(size_t) b] = source.busChannelCount[(size_t) b];
        }
    }

    LatencyGraph::compute (setup, latencyGraph);

    const auto& graph = latencyGraph;
    const int   block = std::max (blockSize, 1);

    for (int c = 0; c < kNumChannels; ++c)
    {
        auto& slot = channelSlots[(size_t) c];

        slot.align.prepare (2, graph.widest, block);
        slot.align.setDelay (graph.channelDelay[(size_t) c]);

        const auto& buses = graph.busDelay[(size_t) c];

        for (size_t b = 0; b < buses.size() && b < (size_t) kMaxOutBuses; ++b)
        {
            if (slot.busChannelCount[b] <= 0)
                continue;

            slot.busAlign[b].prepare (2, graph.widest, block);
            slot.busAlign[b].setDelay (buses[b]);
        }
    }

    for (int i = 1; i < kNumInserts; ++i)
    {
        auto& slot = insertSlots[(size_t) i];

        slot.align.prepare (2, graph.widest, block);
        slot.align.setDelay (graph.insertDelay[(size_t) i]);

        const auto& sends = graph.sendDelay[(size_t) i];

        for (size_t s = 0; s < sends.size() && s < (size_t) kNumSends; ++s)
        {
            slot.sendAlign[s].prepare (2, graph.widest, block);
            slot.sendAlign[s].setDelay (sends[s]);
        }
    }

    for (int i = 0; i < kNumInserts; ++i)
        clipDelaySamples[(size_t) i].store (graph.clipDelay[(size_t) i]);

    totalLatency.store (graph.totalLatency);

    // Which inserts are audible depends on the same routing this function has
    // just read, so it is recomputed here rather than at a second list of call
    // sites that would have to be kept in step with this one. See updateSolo.
    updateSolo();
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

void AudioEngine::setSendTap (int insertIndex, int sendIndex, SendTap::Tap tap) noexcept
{
    if (! juce::isPositiveAndBelow (insertIndex, kNumInserts)
        || ! juce::isPositiveAndBelow (sendIndex, kNumSends))
        return;

    // No graph update of any kind, and both halves of that are deliberate.
    //
    // No compensation update, because the two taps sit at the same point in
    // the latency chain: SendTap.h has the derivation and says that a pre FX
    // tap would be the one that breaks it.
    //
    // No solo update either, which is the less obvious half. A send is a
    // propagation edge for solo whatever point it is tapped at, so neither the
    // reachability matrix nor any insert's gain depends on this. Ardour's
    // Route::direct_feeds_according_to_reality walks its processors without
    // ever asking whether one is pre or post fader, which is the primary
    // source for that. A graph recompute here would be work that cannot change
    // its own answer, taken under the lock the audio thread is trying for.
    controls[(size_t) insertIndex].sendTap[(size_t) sendIndex].store (tap);
}

void AudioEngine::setMuteAffectsPreFaderSends (bool shouldAffect)
{
    muteAffectsPreFaderSends.store (shouldAffect);

    // This one does change an answer, for every insert at once: it decides
    // whether the pre fader point is in the mute set, so it is what
    // preFaderSendGain is made of.
    const juce::SpinLock::ScopedLockType lock (graphLock);
    updateSolo();
}

// ---------------------------------------------------------------------------
/** Works out which inserts are audible, and what each one's output is
    multiplied by.

    The rule, what it was checked against, and what it costs are all in
    SoloGraph.h. Everything that needs JUCE is here: reading the routing and
    the three flags out of the atomics, and publishing the answers.

    This hangs off the end of updateLatency rather than having its own set of
    call sites, and that is deliberate. The closure reads exactly the routing
    updateLatency has just read, and every path that changes a signal route in
    this studio already has to call updateLatency under the graph lock:
    loading or removing a plugin, bypassing an effect, moving a channel to
    another insert, changing a send, splitting a channel's buses. A second
    list of call sites to keep in step with that one is a list that will fall
    out of step, and the symptom would be an insert audible or silent for a
    routing that no longer exists.

    Ardour needs two further pieces of code for exactly this, one correcting
    its counters at both ends when an aux send appears or disappears and one
    zeroing a route's solo before removing it, and it still ships a repair
    path for the counters that leak anyway.

    Must be called with graphLock held. It allocates nothing: soloSetup and
    soloResult are members, and everything below is assign or resize on
    vectors that are already the right size after the first call.
*/
// ---------------------------------------------------------------------------
GainRamp::Targets AudioEngine::insertTargets (int index, PanLaw::Law law) const noexcept
{
    const auto& ctl = controls[(size_t) index];

    // What a fader position means is FaderLaw.h's and which pair of gains a
    // pan position means is PanLaw.h's, and both are tested, because neither
    // can announce that it is wrong: a law a decibel out does not error, does
    // not crackle and looks entirely normal on a meter.
    const auto pan = PanLaw::gains (ctl.panMode.load(), law, (double) ctl.pan.load());

    return { (double) soloGain[(size_t) index].load(),
             FaderLaw::positionToGain ((double) ctl.volume.load()),
             pan.left,
             pan.right };
}

void AudioEngine::latchGainRamps (PanLaw::Law law)
{
    for (int i = 0; i < kNumInserts; ++i)
    {
        auto& ctl  = controls[(size_t) i];
        auto& slot = insertSlots[(size_t) i];

        stripRamps[(size_t) i].latch (insertTargets (i, law));

        for (int sendIndex = 0; sendIndex < kNumSends; ++sendIndex)
        {
            const int to = ctl.sendTo[(size_t) sendIndex].load();

            // Forward only, so anything else is off and feeds nothing. Same
            // test as routeSends makes, and it has to be the same test: a
            // ramp latched to a level whose destination the send loop then
            // refuses would sit at that level forever without ever being
            // applied, and would be the level the send jumped to the moment a
            // valid destination was chosen.
            const bool valid = to > i && to < kNumInserts;

            // The tap too, for the same reason and with the same consequence
            // if it is left out: a render whose first block found a tap
            // different from the one in sendActiveTap would read that as a tap
            // change and fade the send in from nothing over the first few
            // milliseconds of the bounce. Nothing about that looks wrong, and
            // it would be a bounce quietly different from what was heard.
            const auto tap = ctl.sendTap[(size_t) sendIndex].load();

            slot.sendActiveTo[(size_t) sendIndex]  = to;
            slot.sendActiveTap[(size_t) sendIndex] = tap;
            slot.sendTapLatched[(size_t) sendIndex] = tap;

            // And no crossfade in progress, for the same reason as the rest of
            // this: a render that inherited one would spend its first few
            // milliseconds mixing in a tap the send is no longer on.
            slot.sendTapFade[(size_t) sendIndex]  = 0.0;
            slot.sendFadeGain[(size_t) sendIndex] = 0.0;
            slot.sendFadeFrom[(size_t) sendIndex] = tap;

            // Through SendTap::levelTarget rather than off the level, so that a
            // render of a muted strip with a pre fader send starts at the level
            // that strip is actually sending at instead of ramping down to it
            // from the raw knob position.
            slot.sendRamp[(size_t) sendIndex].latch (
                valid ? SendTap::levelTarget (tap, (double) ctl.sendLevel[(size_t) sendIndex].load(),
                                              (double) preFaderSendGain[(size_t) i].load())
                      : 0.0);

            // Nothing is fading, so there is nothing left to drain. A render
            // that inherited a flush would spend its first blocks emptying a
            // line on behalf of a fade that happened before the bounce began.
            slot.sendFlush[(size_t) sendIndex] = 0;
        }

        // Nothing has been copied into it yet for whatever is about to run, and
        // a stale copy is the one thing a pre fader send must not be allowed to
        // read. processInsert fills it before routeSends looks every block, so
        // this only has to be true for the first one.
        slot.preFaderReady = false;
    }
}

void AudioEngine::updateSolo()
{
    auto& setup = soloSetup;

    setup.inserts.resize ((size_t) kNumInserts);

    setup.settings.soloMuteGain      = soloMuteGain.load();
    setup.settings.soloOverridesMute = soloOverridesMute.load();

    // The one setting the pre fader point answers to. It is folded into a gain
    // here rather than being read on the audio thread, so that the mixing loop
    // asks one question of one number whichever tap a send is on.
    setup.settings.muteAffectsPreFaderSends = muteAffectsPreFaderSends.load();

    for (int i = 0; i < kNumInserts; ++i)
    {
        auto& entry = setup.inserts[(size_t) i];
        auto& ctl   = controls[(size_t) i];

        entry.selfSoloed = ctl.solo.load();
        entry.muted      = ctl.mute.load();
        entry.isolated   = ctl.soloIsolate.load();

        // Insert 0 is the master and has no send of its own, so its row is
        // left empty rather than gathered and ignored, which is what
        // updateLatency does with the same row.
        entry.sendTo.resize (i == 0 ? 0 : (size_t) kNumSends);

        entry.sendLevel.resize (entry.sendTo.size());

        for (int sendIndex = 0; sendIndex < (int) entry.sendTo.size(); ++sendIndex)
        {
            entry.sendTo[(size_t) sendIndex]    = ctl.sendTo[(size_t) sendIndex].load();

            // The level as well as the destination, because a send turned all
            // the way down carries no audio and routeSends skips it. See
            // SoloGraph::sendCarriesAudio for what counting it anyway would
            // do to soloing an effects return.
            entry.sendLevel[(size_t) sendIndex] = ctl.sendLevel[(size_t) sendIndex].load();
        }
    }

    SoloGraph::compute (setup, soloResult);

    anySoloed.store (soloResult.anySoloed);

    for (int i = 0; i < kNumInserts; ++i)
    {
        const auto& state = soloResult.inserts[(size_t) i];

        soloGain[(size_t) i].store (state.gain);
        preFaderSendGain[(size_t) i].store (state.preFaderSendGain);
        soloedByOthers[(size_t) i].store (state.soloedByOthers);
        mutedByOthersSoloing[(size_t) i].store (state.mutedByOthersSoloing);
    }
}

// ---------------------------------------------------------------------------

void AudioEngine::setMute (int insert, bool shouldMute)
{
    if (! juce::isPositiveAndBelow (insert, kNumInserts))
        return;

    controls[(size_t) insert].mute.store (shouldMute);

    const juce::SpinLock::ScopedLockType lock (graphLock);
    updateSolo();
}

void AudioEngine::setSolo (int insert, bool shouldSolo)
{
    // Never the master. Soloing the thing every other insert is summed into
    // would mean soloing the whole mix, and the strip does not offer it;
    // this is the guard for a project file that says otherwise. Ardour draws
    // the same line with Route::can_solo.
    if (insert < 1 || insert >= kNumInserts)
        return;

    controls[(size_t) insert].solo.store (shouldSolo);

    const juce::SpinLock::ScopedLockType lock (graphLock);
    updateSolo();
}

void AudioEngine::setSoloIsolate (int insert, bool shouldIsolate)
{
    if (insert < 1 || insert >= kNumInserts)
        return;

    controls[(size_t) insert].soloIsolate.store (shouldIsolate);

    const juce::SpinLock::ScopedLockType lock (graphLock);
    updateSolo();
}

void AudioEngine::applySoloGesture (int insert, bool modifierHeld)
{
    if (insert < 1 || insert >= kNumInserts)
        return;

    const bool already = controls[(size_t) insert].solo.load();

    switch (SoloGraph::gestureFor (exclusiveSolo.load(), modifierHeld, already))
    {
        case SoloGraph::Gesture::clearAll:
            clearAllSolos();
            return;

        case SoloGraph::Gesture::exclusive:
            // Written as one pass over every insert followed by one recompute
            // rather than as a setSolo per strip: fifteen recomputes would
            // each take the graph lock, and the intermediate states are mixes
            // nobody asked for.
            for (int i = 1; i < kNumInserts; ++i)
                controls[(size_t) i].solo.store (i == insert);
            break;

        case SoloGraph::Gesture::toggle:
            controls[(size_t) insert].solo.store (! already);
            break;
    }

    const juce::SpinLock::ScopedLockType lock (graphLock);
    updateSolo();
}

void AudioEngine::clearAllSolos()
{
    for (int i = 1; i < kNumInserts; ++i)
        controls[(size_t) i].solo.store (false);

    const juce::SpinLock::ScopedLockType lock (graphLock);
    updateSolo();
}

void AudioEngine::setSoloSettings (float gain, bool overridesMute, bool exclusive)
{
    // Stored as it arrives, bounds and all. SoloGraph refuses a figure it
    // cannot make sense of rather than clamping it, because a clamp hands a
    // NaN straight back, and it falls back to full mute, which is both the
    // documented default and the one reading that cannot damage a mix.
    soloMuteGain.store (gain);
    soloOverridesMute.store (overridesMute);
    exclusiveSolo.store (exclusive);

    const juce::SpinLock::ScopedLockType lock (graphLock);
    updateSolo();
}


void AudioEngine::setControlRoom (ControlRoom::State s) noexcept
{
    // Brought inside the range here as well as where it is used, so what a
    // caller reads back is what the speakers will do rather than what it
    // asked for. A settings file is the thing that puts a number out of range
    // in here, and the guard in ControlRoom.h says what a NaN one would cost.
    controlRoomDimDb.store (ControlRoom::clampDimDb (s.dimDb));
    controlRoomLatches.store (ControlRoom::pack (s));
}

ControlRoom::State AudioEngine::getControlRoom() const noexcept
{
    return ControlRoom::unpack (controlRoomLatches.load(), controlRoomDimDb.load());
}

/*
    The control room, and the one thing about it that needs saying next to the
    code rather than only in its header: this runs on the way to the speakers
    and nowhere else.

    Applied after the master, after the preview and after the click, which is
    the second deliberate choice here. Not after absolutely everything,
    and the two exceptions are worth naming rather than leaving the comment to
    overstate itself. JUCE sums the output of every audio callback added after
    this engine's into the buffer once this one has returned, and it sums its
    own Test tone in after all of them, so the loopback probe's tone and the
    Test button in Preferences > Audio both bypass this section. The first of
    those is the right way round and is load bearing: a round trip measurement
    taken through a dim or a cut would be a measurement of the control room,
    and an audit confirmed the engine is always the first callback, so the
    probe is safe by construction. The second is a reachable contradiction of
    everything below, since pressing Test while the speakers are cut or
    dimmed for somebody puts a tone out at half scale, and it is JUCE's button
    rather than one of ours. Ardour's metronome bypasses its monitor
    section entirely (session_click.cc:254-256 sends the click straight to its
    own ports) and Harrison documents the consequence, that the monitor volume
    knob does not control the click. That is the wrong trade for this studio.
    Dim is the thing you reach for while somebody is playing, and a dim that
    leaves the click at full level is not a dim. A mono check that includes a
    click which was never in the mix is not a mono check either. On a console
    everything the room hears goes through the monitor path, and that is the
    behaviour taken here: in practice the click is centred, so the fold leaves
    it alone, and what changes is that a dim dims it and a cut cuts it.

    The coefficients are slewed per sample rather than per block, for the
    reason GainRamp.h gives at length: a ramp across the buffer makes the
    gesture's length a function of the buffer size. It matters less here than
    anywhere else in the mixer, since nothing downstream of this is recorded
    or rendered, but there is one law in this engine rather than one law and
    one exception, and the exception is what somebody later would copy.
*/
void AudioEngine::applyControlRoom (float* outL, float* outR, int numSamples) noexcept
{
    if (outL == nullptr || numSamples <= 0)
        return;

    const auto state  = getControlRoom();
    const double step = GainRamp::stepPerSample (sampleRate, GainRamp::levelSeconds);
    auto& ramps = controlRoomRamps;

    // One output channel is one leg, and ControlRoom.h carries why that is a
    // figure of its own rather than the matrix with a missing column.
    if (outR == nullptr)
    {
        const double target = ControlRoom::oneChannelGain (state);

        for (int i = 0; i < numSamples; ++i)
            outL[i] = (float) (ramps.oneChannel.next (target, step) * (double) outL[i]);

        return;
    }

    const auto m = ControlRoom::matrixFor (state);

    for (int i = 0; i < numSamples; ++i)
    {
        // Both legs read before either is written, because each output is a
        // function of both inputs. Written in place one at a time, the left
        // output is fine and the right one then reads the left leg it has
        // already overwritten: under a fold the right leg would come out as
        // a quarter of the left plus three quarters of the right, which is a
        // stereo image pulled to one side rather than a fold, and it would
        // sound like a mix decision. This is the one line in here that is a
        // bug if it moves.
        const double inL = (double) outL[i];
        const double inR = (double) outR[i];

        const double ll = ramps.leftFromLeft  .next (m.leftFromLeft,   step);
        const double lr = ramps.leftFromRight .next (m.leftFromRight,  step);
        const double rl = ramps.rightFromLeft .next (m.rightFromLeft,  step);
        const double rr = ramps.rightFromRight.next (m.rightFromRight, step);

        outL[i] = (float) (ll * inL + lr * inR);
        outR[i] = (float) (rl * inL + rr * inR);
    }
}

void AudioEngine::setMonitorMode (Monitor m)
{
    if (monitorMode.load() == m)
        return;

    monitorMode.store (m);

    // Turning monitoring on takes the compensation delay off the monitored
    // insert, and turning it off puts it back, so the graph has to be redone
    // either way. See the note in updateLatency.
    const juce::SpinLock::ScopedLockType lock (graphLock);
    updateLatency();
}

void AudioEngine::startAudioRecording()
{
    const auto   figures = getRecordFigures();
    const double rate    = figures.sampleRate > 0.0 ? figures.sampleRate : sampleRate;
    const double bps     = rate > 0.0 ? bpm.load() / 60.0 / rate : 0.0;

    // An instrument recording taps a channel's own output and has nothing to
    // do with sockets, so it keeps its own recorder and none of the input
    // ones are started.
    if (recordSource.load() >= 0)
    {
        // And no offset either, which is Ardour's CaptureTime alignment
        // rather than its ExistingMaterial one: it picks between them the
        // same way, by whether a track is fed from a physical input, and this
        // one is fed from inside the mixer.
        //
        // Nothing to put back on the converter side, since the audio never
        // leaves the studio. What is arguable is the rest of it: when the
        // notes being bounced are a pattern playing off the arrangement there
        // is nothing to correct and an offset would drag the bounce off the
        // grid, but when somebody is playing the keyboard into it live they
        // are following backing they hear late, so their performance lands
        // late by the same amount a MIDI recording does. The mode cannot tell
        // those apart, and a bounce that drifts off the grid is the worse of
        // the two, so this stays at zero.
        //
        // This used to say it was the same open problem as the MIDI tap, and
        // it is not: the tap is fed only from the hardware input and the
        // keyboard, so it knows it has a live performance and is offset. The
        // consequence is that the two halves of Rec: Instrument sound and its
        // notes would disagree with each other, which is worse than both of
        // them being late together, so the tap holds its offset at zero for
        // exactly this mode. See the tap for the other end of that.
        recorder.setCaptureOffset (0, bps);
        recorder.begin();
        return;
    }

    // Latched here, once, for every armed strip: the figure has to be the same
    // on all of them or two microphones on one performance would come back
    // out of phase with each other.
    const int offset = RecordAlign::captureOffsetSamples (figures);

    for (int i = 1; i < kNumInserts; ++i)
    {
        const auto assignment = InputSource::assignmentFromStored (insertInput[(size_t) i].load());

        // Armed and wired. Armed with nothing plugged in would produce a file
        // of silence and a clip nobody asked for.
        if (assignment.assigned && insertArmed[(size_t) i].load())
        {
            inputRecorders[(size_t) i].setCaptureOffset (offset, bps);
            inputRecorders[(size_t) i].begin();
        }
    }
}

std::vector<AudioEngine::CapturedInput> AudioEngine::stopAudioRecording()
{
    std::vector<CapturedInput> captured;

    for (int i = 1; i < kNumInserts; ++i)
    {
        if (! inputRecorders[(size_t) i].isActive())
            continue;

        auto passes = inputRecorders[(size_t) i].end();

        if (! passes.empty())
            captured.push_back ({ i, std::move (passes) });
    }

    return captured;
}

bool AudioEngine::isRecordingAudio() const noexcept
{
    if (recorder.isActive())
        return true;

    for (const auto& capture : inputRecorders)
        if (capture.isActive())
            return true;

    return false;
}

void AudioEngine::setInsertInput (int insertIndex, InputSource::Assignment assignment)
{
    // Never the master. It is where every insert is summed after each has
    // been lined up, so a live input there would run through the whole master
    // chain and pick up its lookahead, which is the one thing the monitoring
    // exemption exists to prevent.
    if (! juce::isPositiveAndBelow (insertIndex, kNumInserts) || insertIndex < 1)
        return;

    const int wanted = InputSource::assignmentToStored (assignment);

    // Nothing to do when it has not moved. Worth the test: clearing every
    // input, resetting the engine and loading a project all walk every insert
    // in a loop, and without this a project open took the graph lock about
    // thirty times. The audio thread only ever tries that lock, so each one
    // is a chance of a dropped block.
    if (insertInput[(size_t) insertIndex].exchange (wanted) == wanted)
        return;

    // An insert carrying a live input is exempt from delay compensation and
    // one that has stopped carrying it is not, so the graph has to be redone.
    const juce::SpinLock::ScopedLockType lock (graphLock);
    updateLatency();
}

void AudioEngine::setInsertArmed (int insertIndex, bool armed)
{
    if (juce::isPositiveAndBelow (insertIndex, kNumInserts) && insertIndex >= 1)
        insertArmed[(size_t) insertIndex].store (armed);
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
    std::vector<float> lastAuto, modBase, modWritten;
    std::vector<bool>  modActive;
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

        // Sized here, on the message thread, for the same reason the effect
        // slots size theirs here: the audio thread indexes these and must not
        // be the one to grow them.
        const auto numParams = (size_t) plugin->getParameters().size();
        lastAuto.assign (numParams, -1.0f);
        modBase.assign (numParams, 0.0f);
        modWritten.assign (numParams, -1.0f);
        modActive.assign (numParams, false);
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

        // Moved over rather than left alone, so a modulator does not carry
        // the previous plugin's resting values onto the new one's parameters
        // when the two happen to expose the same number of them.
        slot.modBase    = std::move (modBase);
        slot.modWritten = std::move (modWritten);
        slot.modActive  = std::move (modActive);

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
    std::fill (livePedal.begin(), livePedal.end(), (juce::uint8) 0);
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
    std::vector<float> lastAuto;
    int channels = 2;
    if (plugin != nullptr)
    {
        preparePlugin (*plugin);
        channels = channelsFor (*plugin);
        scratch.setSize (channels, bufferCapacity());

        // Allocated here, on the message thread, because the audio thread
        // indexes it and must not be the one to grow it.
        lastAuto.assign ((size_t) plugin->getParameters().size(), -1.0f);
    }

    std::unique_ptr<juce::AudioPluginInstance> old;
    {
        const juce::SpinLock::ScopedLockType lock (graphLock);
        auto& slot = insertSlots[(size_t) insertIndex].fx[(size_t) slotIndex];
        old = std::move (slot.plugin);
        slot.plugin   = std::move (plugin);
        slot.scratch  = std::move (scratch);
        slot.lastAuto = std::move (lastAuto);
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

    // Discarded by reading rather than by reset(). juce::AbstractFifo::reset
    // writes both indices and is not safe against a writer still inside a
    // write, which the audio thread can be: it checked midiRecording a few
    // instructions ago. A read is the one side this thread is allowed to
    // touch, and leaves the FIFO empty just the same.
    { const auto discard = midiFifo.read (midiFifo.getNumReady()); juce::ignoreUnused (discard); }
    { const auto discard = paramFifo.read (paramFifo.getNumReady()); juce::ignoreUnused (discard); }

    midiRecordChannel.store (juce::jlimit (0, kNumChannels - 1, channel));
    pluginMidiCaptured.store (0);
    midiRecordStart.store (-1.0);

    // Latched once, here, for the same reason startAudioRecording latches the
    // audio one and Ardour refuses an alignment change while a DiskWriter is
    // running (session.cc:8156-8163). The figure moves whenever somebody
    // loads a plugin anywhere in the mixer, and a figure that moved partway
    // through a take would shift the notes after the change against the notes
    // before it: a performance with a step in the middle of it, put there by
    // something that happened on another track.
    midiCaptureOffset.store (RecordAlign::midiCaptureOffsetSamples (getRecordFigures()));
    midiCaptureOffsetBeats.store (0.0);

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

// Called from whichever thread the plugin reports its parameter moves on,
// which can be the audio thread: a plugin is free to report from inside its
// own processBlock. The lock is therefore tried rather than taken, and the
// move is dropped if another thread is mid-write. A parameter stream arrives
// dozens of times a second while a knob is turning, so losing one of them is
// inaudible, and the alternative is the audio thread spinning.
void AudioEngine::pushRecordedParam (int index, float value)
{
    if (! midiRecording.load() || ! playing.load())
        return;

    // Offset with the notes rather than left on the raw transport. A knob
    // moved while playing is the same gesture following the same late
    // monitoring, and its points are written into the same clip as the notes,
    // so leaving it unmoved would split a performance from its own automation
    // by the whole figure.
    //
    // What this does not correct for is the plugin's own reporting delay,
    // which nothing measures, exactly as the typed keyboard's dispatch delay
    // is left in the notes.
    const auto placed = RecordAlign::placeMidiEvent (beatPosition.load(),
                                                     midiCaptureOffsetBeats.load(),
                                                     midiRecordStart.load());
    if (! placed.keep)
        return;

    const double beat = placed.beat;
    const juce::SpinLock::ScopedTryLockType lock (paramWriteLock);
    if (! lock.isLocked())
        return;

    const auto scope = paramFifo.write (1);
    scope.forEach ([&] (int i) { paramRing[(size_t) i] = { beat, index, value }; });
}

// ---------------------------------------------------------------------------
// Device lifecycle

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const double newRate = device->getCurrentSampleRate() > 0.0
                         ? device->getCurrentSampleRate() : 44100.0;
    const int newBlockSize = std::max (16, device->getCurrentBufferSizeSamples());

    midiCollector.reset (newRate);
    recorder.prepare (newRate);

    // All of them, because arming happens while the device is open and
    // preparing allocates. A ring is four seconds of two channels, so
    // seventeen of them is a few tens of megabytes and nothing is reserved
    // for a take until that insert is actually armed.
    for (auto& capture : inputRecorders)
        capture.prepare (newRate);

    // Everything from here is behind the graph lock, including the engine's
    // own block sized buffers and the rate and block size themselves. JUCE
    // stops the device callback around a restart, so the live path would be
    // safe either way, but an offline export holds this lock for its whole
    // run and writes blockBeats every block. Changing the audio device while
    // a bounce is in flight was therefore reallocating the array that export
    // was writing through.
    const juce::SpinLock::ScopedLockType lock (graphLock);

    sampleRate = newRate;
    blockSize  = newBlockSize;
    clickDecay = std::exp (-1.0 / (sampleRate * 0.008));
    liveMidi.ensureSize (8192);
    fxMidi.ensureSize (2048);

    const int cap = bufferCapacity();
    capacity = 0;                 // nothing may run the graph while it is being resized
    blockBeats.assign ((size_t) cap, 0.0);
    clickBuffer.assign ((size_t) cap, 0.0f);
    silence.assign ((size_t) cap, 0.0f);

    for (auto& c : channelSlots)
    {
        // Bytes, not events, and JUCE spends about nine of them per three
        // byte message. The worst case here is stopTrackedNotes releasing a
        // note on every one of the sixteen MIDI channels at once, which is
        // two thousand messages, and growing this buffer is the audio thread
        // allocating inside the graph lock.
        c.midi.ensureSize (32768);
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

        // The pre fader copy of this insert, for whichever of its sends are
        // taken there. Sized with everything else rather than when a send first
        // asks for one, because the audio thread is where it would first be
        // asked for and the audio thread may not allocate.
        ins.preFaderScratch.setSize (2, cap, false, true, true);
        ins.preFaderReady = false;
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

    // Where the ramps are going, not where they were. A device opening or
    // changing its rate is not a moment to fade anything: the step size is
    // worked out from the sample rate, and a ramp left half way across a
    // restart would be a fade over whatever the new rate makes of it.
    latchGainRamps (panLaw.load());

    // The control room is latched for the same reason, and the reason is not
    // cosmetic here either: the step size comes from the sample rate, so a
    // device opening with the fold already engaged would otherwise fade into
    // mono over however long the new rate makes of five milliseconds.
    {
        // How many legs the feed has, read where the device is, so the strip
        // and the arithmetic are asking the same question.
        controlRoomLegs.store (device != nullptr
                                   ? device->getActiveOutputChannels().countNumberOfSetBits()
                                   : 2);

        const auto state = getControlRoom();
        const auto m     = ControlRoom::matrixFor (state);

        controlRoomRamps.leftFromLeft  .latch (m.leftFromLeft);
        controlRoomRamps.leftFromRight .latch (m.leftFromRight);
        controlRoomRamps.rightFromLeft .latch (m.rightFromLeft);
        controlRoomRamps.rightFromRight.latch (m.rightFromRight);
        controlRoomRamps.oneChannel    .latch (ControlRoom::oneChannelGain (state));
    }
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
    // Only the channels actually wired to an insert. Metering everything
    // looks helpful and is the opposite: it moves for an instrument in a
    // socket the studio is not recording, which is exactly the "looks fine,
    // is wrong" shape that made the original one speaker fault so hard to
    // place. With nothing wired it reads the first input, so somebody who
    // has not set anything up yet can still see that a signal is arriving.
    float inPeak = 0.0f;

    if (inputChannelData != nullptr && numInputChannels > 0)
    {
        bool metered = false;

        for (int i = 1; i < kNumInserts; ++i)
        {
            const auto assignment = InputSource::assignmentFromStored (insertInput[(size_t) i].load());

            if (! assignment.assigned)
                continue;

            const auto pick = InputSource::resolve (assignment.choice, numInputChannels);

            if (! pick.valid)
                continue;

            metered = true;

            for (int ch : { pick.left, pick.right })
                if (auto* in = inputChannelData[ch])
                    for (int s = 0; s < numSamples; ++s)
                        inPeak = std::max (inPeak, std::abs (in[s]));
        }

        if (! metered)
            if (auto* in = inputChannelData[0])
                for (int s = 0; s < numSamples; ++s)
                    inPeak = std::max (inPeak, std::abs (in[s]));
    }

    inputLevel.store (std::max (inPeak, inputLevel.load() * 0.85f));

    // ---- the tuner's tap ----
    // Only while the tuner is on screen, and only one channel. The left of the
    // resolved pick rather than a sum of the pair: a mono choice resolves both
    // sides to the same channel, so one instrument in one socket is read
    // exactly, and summing two microphones on one source would comb filter it,
    // which is the one thing that puts a false dip in a difference function.
    if (tunerListening.load())
    {
        int readChannel = -1;

        if (inputChannelData != nullptr && numInputChannels > 0)
        {
            // Which input to read, including the fallbacks, is in
            // InputSource.h where it can be tested. All that is left here is
            // reading the atomics it asks for.
            const auto choice = InputSource::tunerSource (getTunerSource(), kNumInserts,
                                                          [this] (int i)
            {
                return InputSource::assignmentFromStored (insertInput[(size_t) i].load());
            });

            const auto pick = InputSource::resolve (choice, numInputChannels);

            if (pick.valid)
                if (auto* in = inputChannelData[pick.left])
                {
                    tunerRing.write (in, numSamples);
                    readChannel = pick.left;
                }
        }

        tunerChannel.store (readChannel);
    }

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
        lastClickIndex = noClickYet;
        jumped = true;
    }
    const double jumpTo = locateRequest.exchange (-1.0e9);
    if (jumpTo > -1.0e8)
    {
        position = jumpTo;
        lastClickIndex = noClickYet;
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

    // Read once per block, like the pan law, so that one block is clicked
    // under one signature. The tempo conversion above has no business knowing
    // about it: the signature moves bar lines and nothing else, so
    // beatsPerSample stays the tempo over sixty with no denominator in it.
    const Meter::Signature signature = getTimeSignature();
    // isRecordingAudio rather than recorder.isActive: the input takes live in
    // inputRecorders now, and reading only the instrument recorder here meant
    // the transport looped at the end of the last clip during an input
    // recording and played over the take that had just been made.
    const bool   recording      = isRecordingAudio() || midiRecording.load();
    const bool   sequencing     = isRunning && ! counting;   // clips stay quiet during the count
    const double songTail       = songEnd.load();
    const int    slots          = (int) blockBeats.size();
    const int    n              = std::min (numSamples, slots);
    const double blockStartBeat = position;

    // A loop range set on the playlist repeats over itself, and it does so
    // while recording as well, because recording round a loop is the only
    // reason to set one. With no range, playback loops at the end of the bar
    // the last clip finishes in (`songEnd` arrives already rounded up to a
    // whole bar, see SongLength.h), which is a different thing: it is for
    // auditioning an arrangement, and looping it while recording would record
    // over the take just played.
    const double rangeStart = loopRangeStart.load();
    const double rangeEnd   = loopRangeEnd.load();
    const bool   ranged     = rangeEnd - rangeStart >= kMinLoopBeats;
    const double loopFrom   = ranged ? rangeStart : 0.0;
    const double loopTo     = ranged ? rangeEnd : ((! recording && songTail > 0.0) ? songTail : 0.0);

    // ---- live MIDI (hardware, on-screen piano, typing keyboard) ----
    liveMidi.clear();
    midiCollector.removeNextBlockOfMessages (liveMidi, numSamples);
    keyboardState.processNextMidiBuffer (liveMidi, 0, numSamples, true);

    // ---- playhead, loop, metronome ----
    bool wrapped = false;
    int  blockWrapCount = 0;
    if (n > 0)
        std::fill_n (clickBuffer.begin(), n, 0.0f);

    for (int i = 0; i < numSamples; ++i)
    {
        if (isRunning)
        {
            if (! counting && loopTo > loopFrom && position >= loopTo)
            {
                position = loopFrom;
                lastClickIndex = noClickYet;
                wrapped  = true;

                // Where the recorder splits one continuous capture into one
                // take per time round. Noted here rather than acted on,
                // because the audio for this block has not been handed over
                // yet and the offset is into that block.
                if (blockWrapCount < (int) blockWraps.size())
                    blockWraps[(size_t) blockWrapCount++] = { i, position };
            }
            if (i < slots)
                blockBeats[(size_t) i] = position;

            // The click sounds once per display beat, which is the
            // denominator unit: six times a 6/8 bar rather than four times
            // three quarters. So the index this counts is a click index and
            // not a quarter, and the two agree only in 4/4.
            const auto clickIndex = Meter::clickIndexAt (signature, position);
            if (clickIndex != lastClickIndex)
            {
                lastClickIndex = clickIndex;
                if (clickOn)
                {
                    // Fifty milliseconds whatever the signature, because a
                    // fast click cannot buzz: the burst is written into the
                    // buffer with = rather than +=, and a retrigger resets both
                    // the phase and the level, so a click arriving before the
                    // last has finished replaces it instead of summing with it.
                    // Shortening the burst to fit the gap instead, which this
                    // did for a while, cuts the decay three times higher up.
                    // The click is one per display beat, so a thirty second
                    // note click at 400 bpm puts them 18.75 ms apart: a fixed
                    // fifty is replaced at exp(-18.75/8) of its level against
                    // the 8 ms decay, a tenth, while half the gap cuts it at
                    // exp(-9.4/8), a third. Neither is a clean zero and the
                    // quieter one is what the studio has always shipped.
                    clickSamplesLeft = (int) (sampleRate * 0.05);
                    clickPhase = 0.0;
                    clickFreq  = Meter::isAccentedClick (signature, clickIndex) ? 1760.0 : 1320.0;

                    // Read once per click rather than per sample: a level
                    // changed mid click would otherwise step on a decay that
                    // is already running, which is audible as a chirp.
                    clickAmp   = clickGain.load();
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

    // What each of these is worth is in TransportSilence.h, deliberately not
    // here: it used to be, and a wrap quietly got the same answer as a stop.
    //
    // Named rather than positional. The engine reading this table by position
    // would mean somebody reordering the fields of Event silently rewires the
    // transport, which is the same trade the instrument makes when it saves
    // every choice by its id rather than its index.
    const TransportSilence::Event transportEvent
    {
        .stopped   = wasRunning && ! isRunning,
        .jumped    = jumped,
        .wrapped   = wrapped,
        .panicking = false,          // read inside scheduleMidi, from the atomic
    };

    wasRunning = isRunning;

    // ---- audio recording tap ----
    // After the transport loop rather than before it, so that a pass boundary
    // found in this block is handed over with the block it falls inside.
    const int recordFrom = recordSource.load();
    bool recordingTapped = false;
    if (sequencing && recordFrom < 0 && numInputChannels > 0 && inputChannelData != nullptr)
    {
        for (int i = 1; i < kNumInserts; ++i)
        {
            auto& capture = inputRecorders[(size_t) i];

            if (! capture.isActive())
                continue;

            const auto assignment = InputSource::assignmentFromStored (insertInput[(size_t) i].load());

            const auto pick = assignment.assigned
                                ? InputSource::resolve (assignment.choice, numInputChannels)
                                : InputSource::Resolved{};

            const bool usable = pick.valid
                             && pick.left  < numInputChannels
                             && pick.right < numInputChannels;

            // An active take that cannot be read this block is padded rather
            // than skipped. Skipping shortens the take and shifts everything
            // after the gap earlier against the arrangement, with nothing to
            // say it happened. Clearing a strip's input mid take is two
            // clicks, so this is reachable.
            if (! usable)
            {
                if (numSamples <= (int) silence.size())
                    capture.push (silence.data(), silence.data(), numSamples, blockStartBeat,
                                  blockWraps.data(), blockWrapCount);
                continue;
            }

            // A null right channel is how the recorder is told this is one
            // instrument: it writes the same samples to both sides, so the
            // take is centred rather than a stereo file with silence down one
            // of them.
            capture.push (inputChannelData[pick.left],
                          pick.isCentred() ? nullptr : inputChannelData[pick.right],
                          numSamples, blockStartBeat, blockWraps.data(), blockWrapCount);
        }
    }

    // ---- MIDI recording tap ----
    // Aligned, by RecordAlign::midiCaptureOffsetSamples, which has the whole
    // derivation and the sources. The figure is the studio's own compensation
    // plus the output latency plus one audio block, with no input latency in
    // it, and it is latched in startMidiRecording rather than read here.
    //
    // Everything in liveMidi is somebody playing, now: it is filled a few
    // hundred lines above from the hardware MIDI input and from the on-screen
    // and typing keyboard, and a pattern playing off the arrangement never
    // reaches it. That is what makes the offset unambiguous here, and it is
    // what distinguishes this path from the instrument recorder in
    // startAudioRecording, which taps a channel's output inside the mixer,
    // cannot tell a live performance from a bounce, and stays at zero.
    //
    // The on-screen and typing keyboard shares this offset and is right to
    // within a block rather than exactly, which is worth saying rather than
    // leaving to be discovered. MidiKeyboardState::processNextMidiBuffer puts
    // a lone pending note at sample 0 of the current block where the collector
    // anchors to the end of it, so the typed path's own correct offset is one
    // block smaller. It is not given a second figure because the difference is
    // one block while the message thread's dispatch delay in front of it is
    // unmeasured and of the same order: two offsets would claim a precision
    // that is not there. Before this it was late by the whole compensation
    // plus the output latency, so a block is the residual of a fix rather
    // than a fault introduced.
    if (midiRecording.load() && sequencing && n > 0)
    {
        if (midiRecordStart.load() < 0.0)
            midiRecordStart.store (blockStartBeat);

        // Held at zero for an instrument take, which is Rec: Instrument sound
        // and its notes. That take has two halves, the channel's own audio and
        // the notes that drove it, and the audio half cannot be offset because
        // the instrument recorder cannot tell a live performance from a
        // pattern being bounced off the grid. Offsetting only the notes would
        // make the two halves of one performance disagree by the whole figure,
        // and the project would then play it as a flam that moves whenever
        // somebody loads a plugin elsewhere. Both late together is the lesser
        // fault and is what every version so far has done, so that mode is
        // left exactly as it was. See startAudioRecording for the other end.
        const double offsetBeats = recordSource.load() >= 0
                                     ? 0.0
                                     : (double) midiCaptureOffset.load() * beatsPerSample;

        // Published for pushRecordedParam, which runs on whichever thread a
        // plugin reports a knob move on and so has no tempo of its own to
        // convert a sample count with.
        midiCaptureOffsetBeats.store (offsetBeats);

        const double recordStart = midiRecordStart.load();

        for (const auto meta : liveMidi)
        {
            if (meta.numBytes < 1 || meta.numBytes > 3)
                continue;

            const double raw = blockBeats[(size_t) juce::jlimit (0, n - 1, meta.samplePosition)];

            const auto placed = RecordAlign::placeMidiEvent (raw, offsetBeats, recordStart);

            // Asked before the FIFO is written to rather than after. A write
            // scope taken and then abandoned is a slot published with whatever
            // the last take left in it.
            if (! placed.keep)
                continue;

            const auto scope = midiFifo.write (1);
            scope.forEach ([&] (int index)
            {
                auto& r = midiRing[(size_t) index];
                r.beat = placed.beat;
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
            playHead.signature = signature;
            playHead.ppq       = blockStartBeat;
            playHead.seconds   = blockStartBeat * 60.0 / currentBpm;
            playHead.samples   = (juce::int64) (playHead.seconds * sampleRate);
            playHead.playing   = isRunning;
            playHead.recording = recording;
            playHead.looping   = loopTo > loopFrom;
            playHead.loopStart = loopFrom;
            playHead.loopEnd   = loopTo;

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

            scheduleMidi (numSamples, transportEvent, sequencing);
            if (sequencing)
                applyAutomation (blockStartBeat);

            // And after a locate, whatever the gain ramps are holding belongs
            // to the old position too. Latched here rather than up in the
            // branch above, because applyAutomation has just written the
            // volume and pan atomics for the new position and those are two of
            // the four things these ramps follow: latching before it would put
            // them on the gains of wherever the transport used to be and then
            // slew from there. That ordering is the whole of the fault this
            // line exists to avoid, and it is the same one the authority
            // behind the latch names: Ardour reseeds from the first sample of
            // the curve, not from the control it left behind (amp.cc:409-428).
            //
            // A loop wrap is deliberately left to ramp, for the same reason
            // the branch above leaves its compensation lines alone: that audio
            // really is the continuation of what was just heard, and a gain
            // stepping at every wrap is a click once a bar.
            if (jumped)
            {
                latchGainRamps (panLaw.load());

                for (auto& ins : insertSlots)
                    ins.sendFlush.fill (0);
            }

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
                    recorder.push (source.getReadPointer (0), source.getReadPointer (1),
                                   numSamples, blockStartBeat, blockWraps.data(), blockWrapCount);
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

            // One law for the whole block, read once. Read inside the loop
            // instead and a change arriving mid block would be applied to some
            // inserts and not others, so the mix would be momentarily mixed
            // under two laws at once.
            const auto blockLaw = panLaw.load();

            // inserts into master
            auto& master = insertSlots[0].buffer;
            for (int i = 1; i < kNumInserts; ++i)
            {
                processInsert (i, numSamples, blockLaw);

                auto& slot = insertSlots[(size_t) i];

                // Sends are taken post fader and fed forward, so the
                // destination is still ahead in this same pass.
                routeSends (i, numSamples);

                // Then hold this insert back to match the others.
                slot.align.process (slot.buffer, 2, numSamples);

                master.addFrom (0, 0, slot.buffer, 0, 0, numSamples);
                master.addFrom (1, 0, slot.buffer, 1, 0, numSamples);
            }
            processInsert (0, numSamples, blockLaw);

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
        recorder.push (silence.data(), silence.data(), numSamples, blockStartBeat,
                       blockWraps.data(), blockWrapCount);

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

    // ---- the control room, last of all and on the way out only ----
    // Everything this engine puts in front of the speakers has now been
    // summed, the click and the preview included, which is the point: this is
    // the speaker feed and not the mix. applyControlRoom carries why the
    // click is inside it, why renderOffline has no counterpart, and the two
    // things JUCE itself adds after this engine has returned and which this
    // therefore cannot reach.
    applyControlRoom (outL, outR, numSamples);

    beatPosition.store (position);
}

void AudioEngine::stopTrackedNotes (int slot, bool includeLive, bool includeClips, bool liftPedal)
{
    auto& midi = channelSlots[(size_t) slot].midi;
    const HoldPedal::Rows rows { includeLive, includeClips };

    // THE PEDAL AND THE NOTE OFFS ARE ONE SEQUENCE AND CANNOT BE TWO CALLS.
    // The first version of this had the lift in a function of its own, called
    // by the two paths that want it, immediately before this one. It compiled,
    // it sent the right bytes in the right order, and it defeated the whole
    // change: lifting the pedal releases every key the pedal was holding up,
    // so by the time this ran there was nothing left that needed an off and
    // the individual note offs the specification asks for were never sent.
    // Byte for byte what the studio sent before the pedal was modelled at
    // all, with a model underneath it that nothing could read. An audit found
    // it by running the two versions side by side; nothing about it looked
    // wrong.
    //
    // So the decision is HoldPedal's, which is tested, and this is left with
    // the translation into JUCE. On the stack rather than a member, because
    // the audio thread must not allocate and 774 bytes of stack is nothing.
    HoldPedal::Message out[HoldPedal::kMaxReleaseMessages];

    for (int ch = 1; ch <= midiChannels; ++ch)
    {
        const int count = HoldPedal::releaseSequence (pedalChannel (slot, ch), ch, rows,
                                                      liftPedal, false,
                                                      out, HoldPedal::kMaxReleaseMessages);

        // A REFUSAL HERE WOULD NOT BE A SAFE FAILURE, which is why it is
        // asserted rather than left to the loop below. `releaseSequence`
        // returns -1 when the room it was given could not hold the messages,
        // and writes nothing: the loop then runs zero times, so every note
        // off AND the pedal lift for this MIDI channel are dropped with
        // nothing in the signal to say so. At a stop that is every tracked
        // note on this channel hanging, which is a louder version of the
        // fault this whole change exists to end.
        //
        // It cannot happen: the array is `kMaxReleaseMessages` long, which is
        // the most a release can ever write, and `theWorstCaseFitsExactly` in
        // HoldPedalTest pins that constant against a release with every key
        // of both rows sounding. An audit cut the constant by one and the rest
        // of the suite stayed green, which is why that check exists and why
        // this assertion names it.
        jassert (count >= 0);

        for (int i = 0; i < count; ++i)
            midi.addEvent (juce::MidiMessage ((int) out[i].status,
                                              (int) out[i].data1,
                                              (int) out[i].data2), 0);
    }
}

void AudioEngine::releaseStaleClipNotes (double beat)
{
    for (int slot = 0; slot < kNumChannels; ++slot)
    {
        auto* row = clipNotes.data() + (size_t) slot * 128;
        const bool pedalDown = pedalChannel (slot, 1).pedalDown();

        for (int note = 0; note < 128; ++note)
        {
            // `false`, because an edit to a clip somewhere in the arrangement
            // is no reason to take the player's foot off the pedal, and a key
            // whose off is already waiting on that pedal needs nothing from
            // here.
            if (! HoldPedal::needsNoteOff (row[note], false))
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

                // Deferred rather than acted on if the pedal is down, because
                // that is what the receiver will do with it. The pedal is not
                // lifted here: an edit to a clip somewhere in the arrangement
                // is no reason to take the player's foot off.
                HoldPedal::noteOff (row, note, pedalDown);
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
        // Deliberately not offset, where the live tap is. A plugin generates
        // against the transport it is handed, so an arpeggiator running free
        // is already on the grid and moving it earlier would drag it off. That
        // is the same reason the instrument recorder stays at zero, and it
        // does mean one clip can hold both conventions: a chord played in live
        // sits earlier than the notes the plugin made from it. Each is right
        // in its own frame and neither is guessed at.
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

HoldPedal::Channel AudioEngine::pedalChannel (int slot, int midiChannel) noexcept
{
    const int s  = juce::jlimit (0, kNumChannels - 1, slot);
    const int ch = juce::jlimit (1, midiChannels, midiChannel) - 1;

    HoldPedal::Channel c;
    c.live = liveNotes.data() + ((size_t) s * midiChannels + (size_t) ch) * 128;
    c.clip = ch == 0 ? clipNotes.data() + (size_t) s * 128 : nullptr;
    c.down = livePedal.data() + (size_t) s * midiChannels + (size_t) ch;
    return c;
}

void AudioEngine::trackLiveMessage (int slot, const juce::uint8* data, int numBytes)
{
    if (data == nullptr || numBytes < 2)
        return;

    // The whole rule, including the pedal, is HoldPedal.h's, which is tested.
    // This is left with the one thing it is for, which is working out whose
    // keys these are.
    const int channel = (data[0] & 0x0f) + 1;
    HoldPedal::receive (pedalChannel (slot, channel), data, numBytes);
}

void AudioEngine::scheduleMidi (int numSamples, TransportSilence::Event transportEvent, bool isRunning)
{
    // The panic is read here, where it always has been, rather than beside the
    // other three. That is worth more than it looks: this function runs inside
    // the graph lock, so a panic raised during a block whose try lock failed is
    // still waiting in the next block that gets through. The other three are
    // computed outside the lock and are lost with that block, which is a real
    // fault and is written up in Research/BACKLOG.md rather than fixed here.
    transportEvent.panicking = panicRequest.exchange (false);

    const auto silence = TransportSilence::actionFor (transportEvent);
    const bool editedWhilePlaying = snapshotChanged.exchange (false) && isRunning && ! silence.anything();

    for (int slot = 0; slot < kNumChannels; ++slot)
    {
        auto& c = channelSlots[(size_t) slot];
        c.midi.clear();
        if (c.plugin == nullptr)
            continue;

        // The pedal has to arrive ahead of the note offs rather than after
        // them: a note the pedal is holding up survives a note off, and
        // survives All Notes Off too, until the pedal is lifted. Written the
        // other way round, which is how this was until an audit put a buffer
        // order against the comment that claimed it, the pedal is lifted
        // after everything it was holding has already been latched.
        //
        // `stopTrackedNotes` is what keeps that order, in one call, for the
        // reason its own comment gives at length. MidiBuffer::addEvent keeps
        // insertion order for events at the same sample, so what goes in
        // first arrives first.
        // One call, so the lift cannot be separated from the note offs it is
        // there to let through. TransportSilence only ever asks for the lift
        // alongside both sets of note offs, and the condition says so rather
        // than assuming it.
        if (silence.sustainUp || silence.clipNoteOffs || silence.liveNoteOffs)
            stopTrackedNotes (slot, silence.liveNoteOffs, silence.clipNoteOffs, silence.sustainUp);

        if (silence.allNotesOff || silence.allSoundOff)
            for (int ch = 1; ch <= midiChannels; ++ch)
            {
                if (silence.allNotesOff)
                    c.midi.addEvent (juce::MidiMessage::allNotesOff (ch), 0);

                if (silence.allSoundOff)
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
        // THE PEDAL COMES UP FIRST, and this is the path that needed it most.
        // A note off and All Notes Off are both latched by a pedal that is
        // down, and the lift, when the player's foot eventually comes up, is
        // routed to the newly selected channel, because that is where live
        // MIDI goes: the channel being left here would never hear it at all,
        // so the note rang until the transport was next touched. The next
        // stop, rewind or click in the ruler lifts the pedal on every slot,
        // which is a long time while tracking over a stopped transport and
        // can be the whole of a take.
        //
        // The same call the stop path makes, with the same answer to the
        // lift. A wrap deliberately answers it the other way.
        stopTrackedNotes (liveChannel, true, false, true);
        for (int ch = 1; ch <= midiChannels; ++ch)
            channelSlots[(size_t) liveChannel].midi.addEvent (juce::MidiMessage::allNotesOff (ch), 0);
        liveChannel = selected;
    }

    // meta.data rather than meta.getMessage(): juce::MidiMessage keeps four
    // bytes inline and mallocs for anything longer, so building one here
    // would allocate on the audio thread for every system exclusive message
    // a control surface sends. Nothing below needs more than the first two
    // bytes anyway.
    for (const auto meta : liveMidi)
        trackLiveMessage (selected, meta.data, meta.numBytes);
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

            // The arrangement goes out on MIDI channel 1, so the pedal that
            // can latch its releases is the player's pedal on that channel of
            // this same slot. One plugin, one cable, one receiver.
            const bool pedalDown = pedalChannel (clip.channel, 1).pedalDown();

            for (const auto& note : clip.notes)
            {
                if (note.on >= a && note.on < b)
                {
                    slot.midi.addEvent (juce::MidiMessage::noteOn (1, note.note, note.velocity), sampleFor (note.on));
                    HoldPedal::noteOn (active, note.note);
                }
                if (note.off >= a && note.off < b)
                {
                    slot.midi.addEvent (juce::MidiMessage::noteOff (1, note.note), sampleFor (note.off));

                    // Not forgotten while the pedal holds it up. Forgetting it
                    // is what left a note at a loop wrap releasable only by a
                    // panic: the engine believed it had gone, so the next stop
                    // sent no note off for it and leaned on All Notes Off,
                    // which a receiver is not required to recognise at all.
                    HoldPedal::noteOff (active, note.note, pedalDown);
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
            // Checked against the vector that is actually indexed, not only
            // against the parameter list, which is what the mixer path a few
            // lines down does. The two agree today because setChannelPlugin
            // sizes lastAuto from getParameters() under the graph lock, but
            // the asymmetry is the risk, not the current behaviour.
            if (! juce::isPositiveAndBelow (lane.paramIndex, params.size())
                || (size_t) params.size() != slot.lastAuto.size()
                || lane.lane.points.empty())
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

    // Curves that live on the playlist rather than inside a pattern. They are
    // applied after the in-clip lanes, so that where both aim at the same
    // parameter the standalone curve is what is heard: it is the one drawn
    // against the arrangement, which is what a producer reaching for it means
    // by automating a section.
    for (const auto& curve : snapshot.automation)
    {
        if (beat < curve.start || beat >= curve.end || curve.points.empty())
            continue;
        if (recordingMidi && curve.target.kind == AutoTargetKind::channelParam
            && curve.target.channel == recordChannel)
            continue;   // don't fight the user while they record

        applyCurve (curve, beat);
    }
}

// A mixer insert is not a plugin in a channel slot, so it needs its own way in:
// a fader and a pan are controls of the mixer itself, and an effect's
// parameters are reached through the insert and the slot rather than through a
// channel. Hence a second resolution path beside the one above rather than a
// wider index.
void AudioEngine::applyCurve (const AutoCurveRT& curve, double beat)
{
    const auto& target = curve.target;
    const double reading = autoCurveValueAt (curve.points, beat);
    const float  wanted  = autoControlValue (target.kind, reading);

    // A parameter write has to be compared against what was written last
    // block, not merely stored: a plugin is free to quantise or ignore what it
    // is given, so reading the parameter back and comparing with that would
    // write every block forever. The controls below are plain atomics, so
    // comparing with the live value is enough for them.
    const auto writeParam = [&] (juce::AudioPluginInstance* plugin, std::vector<float>& last)
    {
        if (plugin == nullptr)
            return;

        const auto& params = plugin->getParameters();
        if (! juce::isPositiveAndBelow (target.paramIndex, params.size())
            || (size_t) params.size() != last.size())
            return;
        if (! params[target.paramIndex]->isAutomatable())
            return;

        auto& written = last[(size_t) target.paramIndex];
        if (std::abs (wanted - written) < 1.0e-4f)
            return;

        params[target.paramIndex]->setValue (juce::jlimit (0.0f, 1.0f, wanted));
        written = wanted;
    };

    switch (target.kind)
    {
        case AutoTargetKind::channelParam:
        {
            if (! juce::isPositiveAndBelow (target.channel, kNumChannels))
                return;
            auto& slot = channelSlots[(size_t) target.channel];
            writeParam (slot.plugin.get(), slot.lastAuto);
            return;
        }

        case AutoTargetKind::insertVolume:
        {
            if (! juce::isPositiveAndBelow (target.insert, kNumInserts))
                return;
            auto& volume = controls[(size_t) target.insert].volume;
            if (std::abs (volume.load() - wanted) > 1.0e-5f)
                volume.store (juce::jlimit ((float) FaderLaw::minPosition,
                                            (float) FaderLaw::maxPosition, wanted));
            return;
        }

        case AutoTargetKind::insertPan:
        {
            if (! juce::isPositiveAndBelow (target.insert, kNumInserts))
                return;
            auto& pan = controls[(size_t) target.insert].pan;
            if (std::abs (pan.load() - wanted) > 1.0e-5f)
                pan.store (juce::jlimit (-1.0f, 1.0f, wanted));
            return;
        }

        case AutoTargetKind::insertFxParam:
        {
            if (! juce::isPositiveAndBelow (target.insert, kNumInserts)
                || ! juce::isPositiveAndBelow (target.fxSlot, kNumFxSlots))
                return;

            // Bypassing an effect takes it out of the signal path, so moving
            // its parameters would be writing into something nothing is
            // hearing. Left alone rather than written, so unbypassing it finds
            // the automation where the curve says it should be.
            if (controls[(size_t) target.insert].bypass[(size_t) target.fxSlot].load())
                return;

            auto& slot = insertSlots[(size_t) target.insert].fx[(size_t) target.fxSlot];
            writeParam (slot.plugin.get(), slot.lastAuto);
            return;
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

        // Sized by setChannelPlugin, under the graph lock, because growing a
        // vector here would be allocating on the audio thread. If it somehow
        // disagrees, the block is skipped rather than resized: a plugin that
        // has lost its modulation for one buffer is a far smaller problem
        // than a malloc inside the device callback.
        if (slot.modBase.size() != count || slot.modWritten.size() != count
            || slot.modActive.size() != count)
            continue;

        // Gather this block's offset for every parameter this channel modulates.
        bool anyThisChannel = false;
        for (const auto& m : snapshot.modulators)
        {
            if (! m.enabled)
                continue;

            for (const auto& t : m.targets)
                if (t.kind == AutoTargetKind::channelParam && t.channel == channel
                    && juce::isPositiveAndBelow (t.paramIndex, (int) count))
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

                // Once per modulator rather than once per target per
                // parameter: the shape depends only on the modulator and the
                // beat, and this is the innermost loop in the callback. A
                // plugin with several hundred parameters was evaluating the
                // same sine that many times per block.
                const float v = m.valueAt (beat);

                for (const auto& t : m.targets)
                    if (t.kind == AutoTargetKind::channelParam
                        && t.channel == channel && (size_t) t.paramIndex == p)
                        offset += v * t.depth;
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

        // How far the read position advances per output sample, in source
        // samples. It is what decides the cutoff of the read, by Smith's
        // min{Fs,Fs'} rule: see ClipResampler.h. Three things make it anything
        // other than one, and the arithmetic collapses to a single product
        // because the block's beats are evenly spaced:
        // the file's own rate against the device's, the clip's stretch ratio,
        // and, for a warped clip, the slope of the segment being read, which
        // is why that one is taken inside the loop.
        const double plainStep = clip.rate * srcRate / sampleRate;

        // How far outside the file the widest read can reach, which is what
        // the range check below has to allow for. The old check refused any
        // position outside the buffer, which was right for a two sample read
        // and would now cut the last tens of samples off every clip: a
        // bandlimited read is tens of samples wide and treats what is beyond
        // the file as what it is, silence, which is also what stops the end of
        // a clip being a step.
        const int reach = clipKernel.crossings * (int) ClipResampler::Kernel::maxTrackedStep + 2;

        for (int i = 0; i < numSamples; ++i)
        {
            const double beat = blockBeats[(size_t) i] - shift;
            if (beat < clip.start || beat >= clipEnd)
                continue;

            const double seconds = warp != nullptr
                                 ? warpSourceAtSegment (warp, clip.warpCount, warpCursor, beat)
                                 : clip.readOffset + (beat - clip.start) * secondsPerBeat * clip.rate;
            const double frame   = seconds * srcRate;
            if (frame < -(double) reach || frame >= (double) (frames + reach))
                continue;

            // Taken after warpSourceAtSegment has moved the cursor, so it is
            // the slope of the segment this sample is actually inside. A warped
            // clip changes rate at every marker, and reading the cutoff off the
            // wrong segment is the same fault as not scaling the kernel at all,
            // only intermittent.
            const double step = warp != nullptr
                              ? warp[warpCursor].slope * beatsPerSample * srcRate
                              : plainStep;

            // A comped stretch is ramped at its joins, and the clip's own two
            // edges are ramped by whatever fade they carry. Four comparisons
            // for every clip with neither, and a quarter cycle of sine only
            // inside the few milliseconds of an actual join or edge.
            //
            // The two multiply rather than one replacing the other: a comp
            // join is inside the performance and a clip edge is where the
            // performance meets the arrangement, so a folder faded in at its
            // start and comped at its first join needs both. This is the only
            // place either is applied, and `renderOffline` calls this same
            // function over the same snapshot, so a bounce cannot disagree
            // with what was heard.
            const float gain = clip.gain
                             * (float) compFadeGain (clip.fade, beat)
                             * (float) ClipFade::gainAt (clip.edgeFade, beat);

            const auto sample = ClipResampler::read (clipKernel, srcL, srcR, frames, frame, step);

            left[i]  += sample.left  * gain;
            right[i] += sample.right * gain;
        }
    }
}

void AudioEngine::processInsert (int index, int numSamples, PanLaw::Law law)
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

    // THE PRE FADER TAP, which is here and nowhere else because here is the
    // only place it can be: after the effect slots and before the multiply
    // below. See SendTap.h for what the two taps are and where the names come
    // from.
    //
    // Both the latch and the copy are in this function rather than in
    // routeSends, and that is the point of them. routeSends runs later in the
    // same block, so if it read ctl.sendTap itself a click on the control could
    // land between the two reads: this function would decide no copy was
    // needed, and routeSends would then take a send out of whatever that buffer
    // last held, which is this insert's audio from an earlier block. That is a
    // few milliseconds of the right strip at the wrong time, which is not a
    // sound anybody could trace back to a mixer control.
    //
    // It is also what makes a bounce match what was heard without a second
    // copy of the decision, because renderOffline calls this function too.
    // CLAUDE.md's rule about the export path is the reason the tap is inside
    // the shared function instead of in either loop.
    if (index >= 1)
    {
        bool wantsPreFader = false;

        for (int s = 0; s < kNumSends; ++s)
        {
            const auto tap = ctl.sendTap[(size_t) s].load();

            slot.sendTapLatched[(size_t) s] = tap;

            // Three reasons to need the copy, and the second two are what make
            // the crossfade in routeSends possible at all.
            //
            // The send is on the pre fader tap; or it was on it as of the last
            // block, which is the block routeSends is about to notice the
            // change in; or it has left it and the fade off it is still
            // running, which is the case that would otherwise lose the signal
            // it is fading away from half way through.
            wantsPreFader = wantsPreFader
                         || tap == SendTap::Tap::preFader
                         || slot.sendActiveTap[(size_t) s] == SendTap::Tap::preFader
                         || (slot.sendTapFade[(size_t) s] > 0.0
                             && slot.sendFadeFrom[(size_t) s] == SendTap::Tap::preFader);
        }

        // Taken only when a send on this insert is asking for it, so an insert
        // with none pays nothing at all: the common case by a long way, and the
        // same argument the ramps make for their settled path.
        //
        // It is not narrowed any further than that. A send parked with no
        // destination, or with its level at nothing, still counts, and the
        // alternative would be repeating routeSends' own tests for those here:
        // a second opinion on a destination and a level that this function
        // would then have to keep in step with the function that acts on them.
        // The copy it costs is two channels of one block.
        //
        // The size test cannot fire as the engine stands, because
        // audioDeviceAboutToStart holds the graph lock across the resize and
        // sets capacity to zero while it runs, so the callback does not reach
        // the graph with a stale size. It is kept for what it would cost to be
        // wrong rather than for a path anybody can name: this function may not
        // allocate, so the only other answer available here is to read a buffer
        // shorter than the block, and routeSends already has somewhere sound to
        // put a send it cannot serve.
        slot.preFaderReady = wantsPreFader
                          && slot.preFaderScratch.getNumChannels() >= 2
                          && slot.preFaderScratch.getNumSamples() >= numSamples;

        if (slot.preFaderReady)
        {
            slot.preFaderScratch.copyFrom (0, 0, slot.buffer, 0, 0, numSamples);
            slot.preFaderScratch.copyFrom (1, 0, slot.buffer, 1, 0, numSamples);
        }
    }

    // The fader, the pan and solo, which between them are what an insert's
    // output is multiplied by.
    //
    // None of the three laws is here. What a fader position means is
    // FaderLaw.h's, which pair of gains a pan position means is PanLaw.h's,
    // and what solo adds up to across the whole mixer is SoloGraph's, and all
    // three are tested because none of them can announce that it is wrong: a
    // law a decibel out does not error, does not crackle and looks entirely
    // normal on a meter. They meet in insertTargets, which both this and the
    // latch read, so the mixer and a bounce cannot disagree about where a gain
    // is going.
    //
    // Mute is not read here either, and that is the point rather than a
    // refactor. What an insert's output is multiplied by depends on every
    // other insert's solo and on the send matrix, so it is worked out by
    // SoloGraph on the message thread and read as one number. Whether a mute
    // still wins over a solo on the same insert is part of that arithmetic,
    // which is why reading ctl.mute here as well would be a second opinion
    // rather than a safety net.
    //
    // That one multiply is also what carries solo into the sends, because
    // routeSends copies out of this same buffer afterwards. Without it an
    // insert silenced by somebody else's solo would still feed a bus that solo
    // is keeping audible, and would be heard through it.
    //
    // The targets are worked out once per block rather than cached anywhere.
    // In balance mode the pan is a comparison; in pan mode it is three sines
    // and two divisions, and three square roots on top under the -4.5 dB law,
    // and the fader is one std::pow. Next to a whole block of audio that is
    // nothing, and a cached copy would be a second version of the pan position
    // and the fader position, updated from the message thread, free to
    // disagree with the atomics they came from.
    //
    // The law arrives as an argument rather than being read here, so that one
    // block, and one whole bounce, is mixed under one law. See the call sites.
    //
    // RAMPED RATHER THAN STEPPED, which is not a polish. A gain that steps
    // between two blocks puts a discontinuity on the signal at whatever it
    // happened to be, and a step's spectrum falls at only 6 dB per octave, so
    // a mix sitting at -10 dBFS gives a broadband click at -10 dBFS into the
    // master chain. One press of solo does that to up to fifteen inserts at
    // once; one drag of a fader does it sixty times a second. Solo was ramped
    // and the fader two lines below it was not.
    //
    // Ardour does not apply its own figure flat either: every gain in it, the
    // fader, the sends and the mute alike, goes through `Amp::apply_gain`,
    // which runs a one pole toward the target with its state held across
    // blocks. An earlier version of this comment said `Delivery::run`
    // interpolated across the buffer. It does not, and the difference is the
    // whole of the design: interpolating across the buffer would make the
    // smoothing time a function of the buffer size, which is the one shape of
    // this fix that would make a bounce differ from what was heard.
    //
    // What the ramp is, what it costs, how much of the artifact it removes and
    // what it leaves behind are all in GainRamp.h. One thing worth knowing
    // from here: a change of pan mode or of pan law used to be a step of up to
    // 3 dB in the middle of whatever was playing, and now travels like
    // everything else, because both change where the two pan gains are going
    // rather than where they are.
    //
    // How far any of them may travel in one sample is worked out per block
    // rather than held somewhere, because the sample rate is a member that a
    // device change rewrites and a cached step would be the old rate's for one
    // block after it.
    const double levelStep = GainRamp::stepPerSample (sampleRate, GainRamp::levelSeconds);
    const double panStep   = GainRamp::stepPerSample (sampleRate, GainRamp::panSeconds);

    const auto targets = insertTargets (index, law);
    auto&      ramps   = stripRamps[(size_t) index];

    // Nothing is moving, so the block is one exact constant multiply per
    // channel and a steady mix comes out the same every time. This is the
    // common case by a long way: a mix is listened to far more often than it
    // is adjusted.
    if (ramps.settled (targets))
    {
        const auto gain = ramps.gains();

        slot.buffer.applyGain (0, 0, numSamples, (float) gain.left);
        slot.buffer.applyGain (1, 0, numSamples, (float) gain.right);
    }
    else
    {
        // Per sample, because the gain a sample gets is the product of four
        // ramps and the straight line between the ends of a block is not that
        // product. GainRamp.h has what goes wrong with the chord and with
        // spreading a move that arrives early across the whole buffer: both
        // are the buffer size reaching the sound.
        //
        // Both channels out of one call, rather than a ramp advanced per
        // channel, which is the shape CLAUDE.md warns about: it would give the
        // two channels different halves of every move, which is a pan on every
        // gain change. GainRamp::Strip is written so that cannot be spelled
        // here, and so that the measurement in GainRampTest drives the same
        // arithmetic this does.
        auto* left  = slot.buffer.getWritePointer (0);
        auto* right = slot.buffer.getWritePointer (1);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto gain = ramps.next (targets, levelStep, panStep);

            left [i] = (float) ((double) left [i] * gain.left);
            right[i] = (float) ((double) right[i] * gain.right);
        }
    }

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
/** Passes the interface's inputs through to the inserts they are wired to, so
    each player hears themselves through the studio's own effects.

    One input per insert and several inserts at once, which is what recording
    more than one player is. Each insert keeps its own fade, so arming a
    second microphone does not interrupt the first.

    Deliberately not delay compensated. That compensation lines internal paths
    up with each other; on a monitor path it would only add latency the player
    feels directly. Lining the take itself up with the arrangement is a
    separate job, done once per take at startAudioRecording with the figure
    RecordAlign.h works out. It used to be done in the window instead, which
    is why that header is worth reading before changing anything here.

    The level ramps rather than switching, because changing input or turning
    monitoring on or off mid-performance would otherwise click, and a click
    into a guitar amp simulator is loud.
*/
void AudioEngine::mixMonitorInput (const float* const* inputChannelData,
                                   int numInputChannels, int numSamples)
{
    const auto mode = monitorMode.load();
    const auto rule = monitorRule.load();

    // Latched once, before the loop, the way the pan law and the time
    // signature are: every insert in one block is monitored under one
    // transport state. Read per insert instead, a transport edge arriving
    // mid loop would open one strip's input and close another's inside the
    // same block, and the two would then be ramping in opposite directions
    // from a state neither of them was in.
    const MonitorRule::Transport transport { playing.load() };

    bool anyLive = false;

    // The same figure every other gain in the mixer travels at, from
    // GainRamp.h, which has the argument for it. One constant in one place:
    // this used to be its own literal, and the fader next door had none at
    // all.
    //
    // The same ramp the rest of the mixer uses, rather than a second copy of
    // the same arithmetic: this loop used to clamp its own float by hand.
    const double step = GainRamp::stepPerSample (sampleRate, GainRamp::levelSeconds);
    const float gain = monitorGain.load();

    for (int i = 1; i < kNumInserts; ++i)
    {
        const auto assignment = InputSource::assignmentFromStored (insertInput[(size_t) i].load());
        auto& ramp = monitorRamps[(size_t) i];
        const double rampValue = ramp.value;

        const auto pick = assignment.assigned
                            ? InputSource::resolve (assignment.choice, numInputChannels)
                            : InputSource::Resolved{};

        // The decision itself is in MonitorRule.h, with the sources for
        // every rule in it. Asked rather than decided here: it is a table
        // with a cell that two shipped behaviours differ by, and a table in
        // the device callback is a table nothing can test.
        //
        // This insert's own recorder, not the instrument one. Testing the
        // instrument recorder here meant that bouncing a plugin's output
        // opened the monitor on every wired insert, mixing live microphones
        // into the room during a take that had nothing to do with them.
        // It is also what answers "is this strip being recorded" for the
        // rules that ask, including through the count-in, since the
        // recorders are begun before the transport rolls.

        // The ladder rather than two flags handed over side by side: each
        // branch names its own state, so there are no two same typed values
        // here to exchange. MonitorRule::Capture carries what that was
        // protecting against, which an audit found by exchanging them.
        //
        // The recorder first, because it outranks the arm: a take running on
        // a strip whose arm was cleared under it is still being recorded.
        const auto capture = inputRecorders[(size_t) i].isActive()
                                 ? MonitorRule::Capture::recording
                                 : insertArmed[(size_t) i].load()
                                       ? MonitorRule::Capture::armed
                                       : MonitorRule::Capture::idle;

        const MonitorRule::Insert state { .hasInput = pick.valid, .capture = capture };

        const bool listening = MonitorRule::inputAudible (mode, rule, transport, state);

        // Changing input is as much of a discontinuity as switching monitoring
        // on and off, and the same answer applies: fade out, swap at silence,
        // fade back in. Cutting straight from one socket to another puts a
        // full scale step into whatever effects the player is monitoring
        // through, and a click into a guitar amp simulator is loud.
        // What this insert should be reading, which is nothing when it has no
        // input. Tracked even then, so that a fade to silence completes and
        // the old channels stop being read.
        const int wantLeft  = pick.valid ? pick.left  : -1;
        const int wantRight = pick.valid ? pick.right : -1;

        const bool changingSource = wantLeft  != monitorActiveLeft[(size_t) i]
                                 || wantRight != monitorActiveRight[(size_t) i];

        const float target = (listening && ! changingSource) ? 1.0f : 0.0f;

        if (changingSource && rampValue <= 0.0)
        {
            monitorActiveLeft[(size_t) i]  = wantLeft;
            monitorActiveRight[(size_t) i] = wantRight;
        }

        if (rampValue <= 0.0 && target <= 0.0f)
            continue;

        // Still reading the channels it was reading, because the ramp has not
        // reached silence yet. Setting an insert to "No input" has to fade
        // out like everything else: cutting the gain in one sample is the
        // click this whole mechanism exists to avoid, and clearing an input
        // is two clicks away in the mixer.
        const int readLeft  = monitorActiveLeft[(size_t) i];
        const int readRight = monitorActiveRight[(size_t) i];

        // Nothing adopted yet, or the device has shrunk under us. Either way
        // there is nothing safe to read, so drop the level rather than take a
        // channel of an array that does not have it.
        if (inputChannelData == nullptr
            || readLeft < 0 || readRight < 0
            || readLeft >= numInputChannels || readRight >= numInputChannels)
        {
            ramp.latch (0.0);
            continue;
        }

        const auto* left  = inputChannelData[readLeft];
        const auto* right = inputChannelData[readRight];

        if (left == nullptr)  left = right;
        if (right == nullptr) right = left;

        // Both null means the device handed over channels it has no data for.
        if (left == nullptr)
            continue;

        auto& dest = insertSlots[(size_t) i].buffer;
        auto* outL = dest.getWritePointer (0);
        auto* outR = dest.getWritePointer (1);

        for (int n = 0; n < numSamples; ++n)
        {
            const float scaled = (float) ramp.next ((double) target, step) * gain;
            outL[n] += left[n]  * scaled;
            outR[n] += right[n] * scaled;
        }

        if (ramp.value > 0.0)
            anyLive = true;
    }

    monitoring.store (anyLive);
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
        // reset() rather than prepare(): this is the audio thread, and
        // prepare() is where the meter's buffers are sized. Nothing it would
        // redo has changed, because the sample rate only moves in
        // audioDeviceAboutToStart, which prepares the meter itself.
        loudness.reset();
        corrLR = corrLL = corrRR = 0.0;
    }

    // The spectrum's tap, taken before the early return below and deliberately
    // not gated on the transport.
    //
    // The loudness has to be gated: an integrated figure that kept averaging
    // through an idle session would walk down towards silence and describe
    // nothing that was ever played. A band display has no integration over the
    // session at all, only its own ballistics, which read silence as silence
    // and recover in a time constant. And the master carries a live input
    // while somebody is monitoring a guitar through it, which is a spectrum
    // worth looking at and never a loudness worth integrating.
    if (spectrumListening.load() && numSamples > 0 && master.getNumChannels() >= 2)
    {
        // Interleaved into one ring rather than written to two, so that a
        // snapshot of it is one frame of both legs. AudioEngine.h carries why
        // two rings could not promise that.
        //
        // A block larger than the bound keeps only its newest samples, which
        // is what the ring itself would do with an oversized write, and the
        // scratch buffer is sized for the bound so nothing here allocates.
        const int n = juce::jmin (numSamples, Tuner::largestStudioBlockSamples);

        const auto* l = master.getReadPointer (0) + (numSamples - n);
        const auto* r = master.getReadPointer (1) + (numSamples - n);

        for (int i = 0; i < n; ++i)
        {
            spectrumInterleave[(std::size_t) (2 * i)]     = l[i];
            spectrumInterleave[(std::size_t) (2 * i + 1)] = r[i];
        }

        spectrumRing.write (spectrumInterleave.data(), 2 * n);
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

void AudioEngine::setSpectrumListening (bool shouldListen) noexcept
{
    // Marked before the flag goes up, so nothing can slip in between and be
    // counted as fresh when it is not. The other order would be harmless and
    // this one needs no argument.
    if (shouldListen && ! spectrumListening.load())
        spectrumFreshFrom.store (spectrumRing.totalWritten());

    spectrumListening.store (shouldListen);
}

bool AudioEngine::readSpectrumFrame (std::vector<float>& left, std::vector<float>& right)
{
    if (left.empty() || left.size() != right.size())
        return false;

    const std::size_t want       = left.size();
    const std::size_t interleaved = 2 * want;

    // Nothing written before the display was turned on. The counter is
    // cumulative and never reset, so without this the first frame after the
    // view comes back is whatever was playing when it was last closed, and the
    // analyser seeds on its first frame, so it would be drawn at full
    // confidence. The subtraction cannot wrap: the mark is taken from this
    // same counter, which only ever increases.
    if (spectrumRing.totalWritten() - spectrumFreshFrom.load() < (std::uint64_t) interleaved)
        return false;

    if (spectrumSnapshot.size() != interleaved)
        spectrumSnapshot.assign (interleaved, 0.0f);

    if (! spectrumRing.readLatest (spectrumSnapshot))
        return false;

    // The snapshot starts on a left sample, and that is by construction rather
    // than by luck: the ring is only ever written in whole blocks of two
    // channels, so its counter is always even, and an even number of samples
    // taken from an even position leaves the interleaving in step.
    for (std::size_t i = 0; i < want; ++i)
    {
        left[i]  = spectrumSnapshot[2 * i];
        right[i] = spectrumSnapshot[2 * i + 1];
    }

    return true;
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

    const double levelStep = GainRamp::stepPerSample (sampleRate, GainRamp::levelSeconds);

    for (int s = 0; s < kNumSends; ++s)
    {
        const int   to    = ctl.sendTo[(size_t) s].load();
        const float level = ctl.sendLevel[(size_t) s].load();

        // The tap as processInsert latched it, not as the atomic reads now.
        // Reading the atomic here is the one thing that would let this function
        // and processInsert disagree about which buffer a send is coming out of
        // inside one block: see the latch itself for what that sounds like.
        const auto  tap   = slot.sendTapLatched[(size_t) s];

        auto& ramp      = slot.sendRamp[(size_t) s];
        auto& activeTo  = slot.sendActiveTo[(size_t) s];
        auto& activeTap = slot.sendActiveTap[(size_t) s];

        // Forward only, so the destination has not been mixed yet this block.
        const bool valid = to > index && to < kNumInserts;

        // A destination change is a routing change rather than a level
        // change, and the two want different answers. The level is ramped
        // below; a destination is adopted at once and fed in from nothing, so
        // choosing a bus for a send whose level is already up fades it in over
        // the same few milliseconds as everything else instead of appearing at
        // full level in one sample.
        //
        // What is deliberately not done is fading the old destination out,
        // which is the half of this that cannot be done with one ramp. The
        // copy reaches its destination through sendAlign, a delay line holding
        // up to the compensation of the whole graph, so by the time a fade out
        // at this end emerged at the other the send would already have been
        // repointed, and the audio still in flight would arrive at the new
        // destination instead. Doing it properly means feeding the old
        // destination until its own line has drained, which is a second ramp
        // and a second destination held alongside this one. Left, with the
        // unlock named: today repointing a live send still truncates it at the
        // old bus, which is what it did before any of this, and choosing one
        // no longer clicks at the new one.
        auto& align = slot.sendAlign[(size_t) s];
        auto& flush = slot.sendFlush[(size_t) s];

        const bool destinationChanged = to != activeTo;

        if (destinationChanged)
        {
            activeTo  = to;

            // The tap comes along with it rather than being noticed separately
            // next block. A destination change throws the line away, so there
            // is nothing left of the old tap to fade away from.
            activeTap = tap;
            slot.sendTapFade[(size_t) s] = 0.0;

            ramp.latch (0.0);

            // What is in the line belongs to the destination it was taken for,
            // and that destination is no longer this one. Dropping it is a
            // truncation of a send that was already being truncated; letting
            // it drain would be the old bus's audio arriving at the new one,
            // a few milliseconds of the wrong material on the wrong strip.
            align.reset();
            flush = 0;
        }
        if (! valid)
        {
            // Nothing to feed and nowhere to feed it, so there is no fade to
            // be had: turning a destination off is the same truncation as
            // repointing it. Latched rather than left where it was so that
            // choosing a destination again starts from silence.
            //
            // The line is NOT emptied here, and that is deliberate rather than
            // an omission. The branch above already emptied it on the one
            // block where the destination changed, and nothing can write to it
            // afterwards because process is never reached while a send is off,
            // so there is nothing to empty. Doing it here instead costs a
            // memset of the whole line on every block of every switched off
            // send, which is two per insert by default: with a lookahead
            // plugin reporting 4096 samples and a 64 sample buffer that is
            // over a megabyte a block, walked through the cache, in a 1.3 ms
            // budget, paid by somebody who does not use sends at all.
            ramp.latch (0.0);
            flush = 0;

            // And no crossfade, because there is nothing flowing for one to be
            // between. A fade left armed here would be waiting on audio that
            // only arrives once a destination is chosen again, and by then it
            // would be a fade away from a tap the send left some time ago.
            slot.sendTapFade[(size_t) s] = 0.0;
            continue;
        }

        // What the cut does to a send is the tap's question, and SendTap.h is
        // where it is answered: a post fader send's cut is already in the
        // buffer it is copied from, and folding it in again here would arrive
        // at its square. A pre fader send is copied from a buffer nothing has
        // been applied to, so whatever cut it is supposed to carry has to be
        // folded in, and whether that is a cut at all is the one setting.
        const double target = SendTap::levelTarget (tap, (double) level,
                                                    (double) preFaderSendGain[(size_t) index].load());

        if (! destinationChanged && tap != activeTap)
        {
            // A TAP CHANGE WITH THE SAME DESTINATION IS NOT A DESTINATION
            // CHANGE, and treating it as one was wrong in both halves.
            //
            // The destination branch above throws the alignment line away,
            // which is right when the audio in it was taken for a bus that is
            // no longer this send's. Here the bus has not changed: those
            // samples were taken for exactly the destination they are still
            // going to, so emptying the line would punch a hole in the send of
            // however long the compensation happens to be. With a lookahead
            // plugin on the destination reporting 4096 samples that is 85
            // milliseconds of silence on a bus somebody is still sending to.
            //
            // And latching the ramp to nothing does not avoid a click, it
            // moves it. The line would keep handing out the old tap's audio at
            // full level while the new tap faded in behind it, so the step
            // would arrive at the far end a few milliseconds later, where
            // nothing connects it to the control that caused it.
            //
            // What makes this case different from every other discontinuity in
            // the mixer is that **both signals exist in the same block**:
            // processInsert leaves the pre fader copy beside the post fader
            // buffer. So the two are mixed instead, the line is left alone, and
            // the send travels between two steady states.
            //
            // Which is why the gain is **held** rather than ramped across the
            // fade, and why the ramp is put straight onto the new tap's target
            // instead of slewing toward it. Each side of the fade is then a
            // resolved steady state and the answer is a convex combination of
            // the two, which cannot exceed either. Ramping the gain while the
            // fade ran was two moves at once, and the product of the two is not
            // monotone: SendTap::fadeSample carries the measurement of what
            // that did on a muted strip, where both steady states are silence
            // and it produced a burst.
            //
            // The ramp being latched costs an in progress level move its
            // remaining travel, since a send whose amount was being dragged
            // arrives at the amount in one sample instead of over the rest of
            // the slew. That is bounded by the convex combination above, so it
            // cannot overshoot, and dragging an amount and changing a tap in
            // the same five milliseconds is not a gesture anybody makes.
            //
            // A second change while a fade is still running fades from the tap
            // it was heading for rather than from the blend, which is a
            // discontinuity of whatever that fade had left to travel. It is
            // only reachable by clicking the control twice inside five
            // milliseconds, and setSendTap has no caller but the button.
            slot.sendFadeFrom[(size_t) s] = activeTap;
            slot.sendFadeGain[(size_t) s] = ramp.value;
            slot.sendTapFade[(size_t) s]  = 1.0;

            ramp.latch (target);
            activeTap = tap;
        }

        // Zero level and a ramp already at zero is the only case that can be
        // skipped. Skipping on the level alone, which is what this did before
        // the ramp existed, would be the one way to make a send click on the
        // way down: the moment somebody pulled the level to nothing the send
        // would stop being mixed, with the ramp still at whatever it had
        // reached. Ardour states the rule as a comment on three of its own
        // classes, delivery.cc:325-327 and the same words in send.cc and
        // internal_send.cc: "we need to continue running until the gain has
        // gone to zero".
        const bool quiet = target <= 0.0 && ramp.settled (0.0);

        if (quiet && flush <= 0)
        {
            // Nothing is flowing, so there is nothing for a crossfade to be
            // between: a tap changed on a send whose level is already at
            // nothing is simply the new tap, and the fade is dropped rather
            // than left armed to run whenever the level next comes up. Raising
            // the level then arms a fade of its own from a held gain of zero,
            // which is a fade in.
            slot.sendTapFade[(size_t) s] = 0.0;
            continue;
        }

        // A pre fader send with no copy to read is in exactly the position of a
        // send with no scratch to copy through, so it is the same branch: the
        // alternative is reading a buffer processInsert did not fill this
        // block, which is older audio off the right strip.
        //
        // A fade away from the pre fader tap needs that copy too, for as long as
        // it is still mixing it in, which is why processInsert keeps taking it
        // while one is running. A fade only runs while audio is flowing: the
        // two paths above that feed nothing drop it rather than leave it armed.
        auto& fade = slot.sendTapFade[(size_t) s];

        const bool needsPreFaderCopy = tap == SendTap::Tap::preFader
                                    || (fade > 0.0 && slot.sendFadeFrom[(size_t) s] == SendTap::Tap::preFader);

        if (slot.sendScratch.getNumChannels() < 2 || slot.sendScratch.getNumSamples() < numSamples
            || (needsPreFaderCopy && ! slot.preFaderReady))
        {
            // No scratch to copy through, so this block cannot be sent at all.
            // A gap in a send is a gap whatever the ramp does, so it is put
            // back to silence rather than left mid fade to resume from an
            // arbitrary level once the buffer reappears.
            ramp.latch (0.0);
            continue;
        }

        // The whole of the difference between the two taps, in one choice of
        // buffer. slot.buffer has had the fader, the pan and the cut applied to
        // it by processInsert; preFaderScratch is the copy processInsert took
        // before that multiply.
        //
        // Written as a function of the tap rather than as a ternary spelled out
        // here, because inverting this one choice is the quietest possible
        // breakage of the whole feature: both buffers are the right strip, in
        // time, at a plausible level, and the only symptom is a send that
        // follows a fader it was told not to follow. See SendTap::sourceIsPreFader.
        const auto& source = SendTap::sourceIsPreFader (tap) ? slot.preFaderScratch
                                                             : slot.buffer;

        juce::AudioBuffer<float> copy (slot.sendScratch.getArrayOfWritePointers(), 2, numSamples);

        if (fade > 0.0)
        {
            // CROSSFADING OFF THE TAP THIS SEND HAS JUST LEFT, and the gains
            // are in here rather than being applied to the blend afterwards.
            // That is the whole correction: each side is resolved, so the
            // answer is a convex combination of two steady states.
            // SendTap::fadeSample is where that is written down and tested, and
            // it carries the measurement of what applying one gain to a blend
            // of two buffers did instead.
            const auto& from = SendTap::sourceIsPreFader (slot.sendFadeFrom[(size_t) s])
                                 ? slot.preFaderScratch : slot.buffer;

            const auto* fromL = from.getReadPointer (0);
            const auto* fromR = from.getReadPointer (1);
            const auto* toL   = source.getReadPointer (0);
            const auto* toR   = source.getReadPointer (1);

            auto* outL = copy.getWritePointer (0);
            auto* outR = copy.getWritePointer (1);

            const double held = slot.sendFadeGain[(size_t) s];

            for (int i = 0; i < numSamples; ++i)
            {
                // One figure for both channels, for the reason processInsert
                // gives about a level change: a crossfade running at different
                // rates on the two halves would move the image for as long as
                // it lasted.
                const double t = 1.0 - fade;

                // The ramp is read rather than assumed to be at the target,
                // because the amount can be moved during the fade and the send
                // must then travel to where it is now going rather than to
                // where it was going when the tap changed.
                const double g = ramp.next (target, levelStep);

                outL[i] = (float) SendTap::fadeSample (fromL[i], held, toL[i], g, t);
                outR[i] = (float) SendTap::fadeSample (fromR[i], held, toR[i], g, t);

                fade = std::max (0.0, fade - levelStep);
            }
        }
        else if (ramp.settled (target))
        {
            // Which includes every block of a flush, where the level has
            // reached zero and this is feeding silence in to push the last of
            // the fade out of the alignment line.
            copy.copyFrom (0, 0, source, 0, 0, numSamples);
            copy.copyFrom (1, 0, source, 1, 0, numSamples);
            copy.applyGain ((float) ramp.value);
        }
        else
        {
            copy.copyFrom (0, 0, source, 0, 0, numSamples);
            copy.copyFrom (1, 0, source, 1, 0, numSamples);

            // Per sample and both channels together, for the reasons
            // processInsert gives: a move that arrives part way through a block
            // must not be spread across the rest of it, and a send is not a
            // place to put a stereo image on a level change.
            auto* left  = copy.getWritePointer (0);
            auto* right = copy.getWritePointer (1);

            for (int i = 0; i < numSamples; ++i)
            {
                const float g = (float) ramp.next (target, levelStep);
                left [i] *= g;
                right[i] *= g;
            }
        }

        // A bus fed from here runs at least as late as this insert, so the send
        // waits for whatever else arrives there.
        align.process (copy, 2, numSamples);

        // Enough to empty the line once the level reaches silence, and reset to
        // the whole of it while anything is still being sent. Read from the
        // line rather than assumed, because the figure is whatever the
        // compensation graph made it this block and it changes whenever a
        // plugin is loaded anywhere.
        flush = quiet ? std::max (0, flush - numSamples) : align.getDelay();

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

    // The same bandlimited read the playlist gets, and for the same reason: an
    // audition is how somebody decides whether a file is the one they want, so
    // it should not be the one place in the studio that plays it through a
    // lowpass. A browser file at 44.1 kHz on a 48 kHz device is every sample
    // interpolated, which is the case ClipResampler.h measures.
    for (int i = 0; i < numSamples; ++i)
    {
        if (previewPos >= (double) (frames - 1))
        {
            previewData = nullptr;
            return;
        }

        const auto sample = ClipResampler::read (clipKernel, srcL, srcR, frames, previewPos, step);

        left[i] += sample.left * 0.8f;
        if (right != nullptr)
            right[i] += sample.right * 0.8f;
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

    // A bounce is fully compensated, whatever monitoring is doing.
    //
    // The exemption that keeps a player able to play is a deliberate
    // misalignment, and it is the right trade while tracking. It is the wrong
    // trade in a rendered file: with several inputs wired, most of the mixer
    // would be uncompensated in the export, and the person would have a
    // bounce that does not match what a compensated playback sounds like with
    // nothing to say why. The engine's own rule is that a bounce must not
    // sound different from what was heard, and the honest reading of that is
    // that it must not inherit a tracking compromise either.
    exportingFullyCompensated = true;

    // The law is latched for the whole render rather than read per block, for
    // the same reason the record alignment figure is latched when recording
    // starts: an export runs on its own thread while the mixer stays live, and
    // a law changed halfway through would put the join in the middle of the
    // bounce, with the first half mixed under one law and the second under
    // another. Taking the graph lock in setPanLaw would prevent it too and
    // would spin the message thread for the length of the export.
    const auto renderLaw = panLaw.load();
    updateLatency();

    // The ramps start where they are going rather than where live playback
    // left them: move a fader, or engage a solo, and export at once, and the
    // front of the file would otherwise be the mixer catching up with the mix.
    // A bounce has to be the same from its first sample to its last.
    //
    // Done on the first block of the loop below rather than here, because two
    // of the four gains a ramp follows are written by the automation inside
    // that loop. The comment there has the failure that ordering causes.
    //
    // Solo cannot change during a render, because this function holds the
    // graph lock for all of it.
    bool firstBlock = true;

    struct RestoreCompensation
    {
        AudioEngine& engine;
        ~RestoreCompensation()
        {
            engine.exportingFullyCompensated = false;
            engine.updateLatency();
        }
    } restoreCompensation { *this };

    // Remember the live transport so the session is undisturbed afterwards
    const double savedPosition = position;
    const bool   savedPlaying  = playing.load();
    const auto   savedLastClickIndex = lastClickIndex;

    playing.store (true);
    position = fromBeat;
    lastClickIndex = noClickYet;
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

        // The same signature the live path reports. A hosted plugin that
        // follows the bar has to hear the same bar in a bounce as it did
        // through the speakers, which is the rule that `mixChannelOutput` and
        // `routeSends` exist for applied to the play head.
        playHead.signature = getTimeSignature();
        playHead.ppq       = blockStart;
        playHead.seconds   = blockStart * 60.0 / playHead.bpm;
        playHead.samples   = (juce::int64) (playHead.seconds * sampleRate);
        playHead.playing   = sequencing;
        playHead.recording = false;
        playHead.looping   = false;

        for (auto& ins : insertSlots)
            ins.buffer.clear (0, n);

        // Nothing happens to the play head during a render: it runs from
        // fromBeat to the end without looping and without being moved, so
        // there is nothing to silence. The clean start is the panicRequest
        // set before this loop, which scheduleMidi still consumes.
        scheduleMidi (n, {}, sequencing);
        if (sequencing)
            applyAutomation (blockStart);
        applyModulation (blockStart);

        // The gain ramps are put where they are going HERE, on the first block
        // and after that block's automation has been written, rather than
        // before the loop.
        //
        // That ordering is the whole of it. applyAutomation stores a volume or
        // pan curve's value straight into the same two atomics the mixer reads
        // its targets from, so a latch taken before the loop captures whatever
        // the live transport or somebody's hand last left in them, and then
        // this block overwrites them with the curve's value at fromBeat and
        // the mixer slews from one to the other. Two consequences, both silent
        // and both exactly what a latch exists to prevent: a bounce of a range
        // where a curve sits at unity, taken after playing to a fade out, opens
        // with the first few milliseconds fading in from the fade's last
        // value, so a downbeat at sample zero loses its attack; and rendering
        // the same range twice gives two different files, because the first
        // render leaves the atomics where its own last block put them.
        //
        // Ardour reseeds from the first sample of the automation curve for the
        // same reason, and tells the two cases apart by whether the transport
        // is where it expected to be (amp.cc:409-428).
        if (firstBlock)
        {
            latchGainRamps (renderLaw);
            firstBlock = false;
        }

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
            processInsert (i, n, renderLaw);

            auto& slot = insertSlots[(size_t) i];
            routeSends (i, n);
            slot.align.process (slot.buffer, 2, n);

            master.addFrom (0, 0, slot.buffer, 0, 0, n);
            master.addFrom (1, 0, slot.buffer, 1, 0, n);

            if (std::find (stemInserts.begin(), stemInserts.end(), i) != stemInserts.end())
                sink (i, slot.buffer.getArrayOfReadPointers(), n);
        }
        processInsert (0, n, renderLaw);
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
    lastClickIndex = savedLastClickIndex;
    playing.store (savedPlaying);
    playHead.playing = savedPlaying;

    offlineActive.store (false);
    return ok;
}
