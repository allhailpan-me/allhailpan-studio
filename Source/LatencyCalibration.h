#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include "LatencyProbe.h"

#include <atomic>

//==============================================================================
/** The JUCE side of the loopback measurement: a device callback, and nothing
    else.

    Everything that could be wrong about the arithmetic is in LatencyProbe.h,
    where there is no JUCE and a test can drive it. What is left here is the
    part that genuinely needs a device: taking a place in the callback chain,
    writing the probe into one output channel, reading one input channel, and
    handing the answer across to the window.

    WHY IT IS A CALLBACK OF ITS OWN AND NOT PART OF THE ENGINE

    Because the thing being measured is the path the engine is not on. The
    round trip this corrects is the converters and the driver, out of one
    socket and back into another, and the mixer is not in it: a probe that went
    through the mixer would arrive carrying the mixer's own compensation and
    the measurement would be of the wrong thing. Ardour takes the same view
    from the other end, running its calibration through a special callback that
    silences every port and writes only the two being measured.

    It also means the measurement needs nothing from the engine and the engine
    needs nothing from it.

    THE ONE THING ABOUT THIS THAT IS EASY TO GET WRONG

    A second callback added to an AudioDeviceManager does not get the device's
    output buffer. Read from juce_AudioDeviceManager.cpp at 9.0.3 rather than
    assumed: the first callback in the list is handed the real buffer, and
    every one after it is handed a `tempBuffer` whose contents are then summed
    into the real one. That buffer is sized with `setSize (..., false, false,
    true)`, which keeps neither its contents nor clears what it grows into, so
    what arrives in it is whatever was last in that memory.

    JUCE says the same thing about the first callback's buffer, in
    juce_AudioIODevice.h's own documentation of the method: "The initial
    contents of the array is undefined, so the callback function must fill all
    the channels with zeros if its output is silence. Failing to do this could
    cause quite an unpleasant noise!"

    So every output channel has to be written, not just the one the probe is
    played down. A channel left alone is not silence, it is stale audio summed
    into somebody's mix at full scale, and it would present as a burst of noise
    out of the other speaker the moment anybody opened the preferences window.
    LatencyProbe::Probe::process clears the whole block for the same reason on
    the paths where it returns early, and its test asserts that it does.

    HANDING THE ANSWER ACROSS

    The probe is resolved on the audio thread, because that is where it lives,
    and published as one 64 bit word. LatencyProbe::pack is why that is enough:
    a whole reading fits in 59 bits, so the window either sees the last
    complete reading or the one before it, never half of each, and no lock goes
    anywhere near the audio thread. Resolving costs thirteen arctangents and is
    done eight times a second rather than every block.

    WHAT THIS DOES NOT HANDLE

    Input monitoring. If the insert fed by the input the cable is plugged into
    is being monitored, the probe comes back out of the same output it went
    into and round again, which is a feedback loop and will measure something
    that is not a round trip. Nothing here can detect that, because an
    arriving tone looks the same whatever brought it. The panel says to
    disarm inputs before measuring, which is the honest answer and is also
    what the references do: Ardour silences everything it can reach and still
    tells the user to turn their monitoring volume down.
*/
class LatencyCalibration : private juce::AudioIODeviceCallback
{
public:
    explicit LatencyCalibration (juce::AudioDeviceManager& manager) : devices (manager) {}

    ~LatencyCalibration() override { stop(); }

    /** Starts playing the probe down `outputChannel` and listening on
        `inputChannel`, both being indices into the device's active channels,
        which is what a callback is handed.

        Taking it again is how a measurement is retaken after the cable has
        moved: everything the demodulator has learned is dropped.
    */
    void start (int outputChannel, int inputChannel)
    {
        stop();

        auto* device = devices.getCurrentAudioDevice();

        if (device == nullptr)
            return;

        // Written before the callback is registered and not touched after, so
        // the audio thread never reads one of them being changed.
        outputIndex = outputChannel;
        inputIndex  = inputChannel;

        published.store (LatencyProbe::pack ({}), std::memory_order_relaxed);
        sinceResolve = 0;

        running = true;
        devices.addAudioCallback (this);
    }

    void stop()
    {
        if (! running)
            return;

        running = false;
        devices.removeAudioCallback (this);
    }

    bool isRunning() const noexcept { return running; }

    /** The last complete reading, or a reading saying nothing came back if
        there has not been one yet. */
    LatencyProbe::Reading latest() const noexcept
    {
        return LatencyProbe::unpack (published.load (std::memory_order_relaxed));
    }

    /** How long the measurement has been going, which the panel needs to tell
        "no cable" from "not settled yet". Approximate on purpose: it is
        counted in whole blocks on the audio thread and read without
        synchronising, because nothing is decided from it. */
    double elapsedSeconds() const noexcept
    {
        const double rate = sampleRate.load (std::memory_order_relaxed);
        return rate > 0.0 ? (double) elapsed.load (std::memory_order_relaxed) / rate : 0.0;
    }

    /** The active channels of the open device, as the names it gives them,
        paired with the index a callback would find them at.

        Here rather than in the panel because the pairing is the part that is
        easy to get wrong: a device can have sixteen inputs with four of them
        enabled, and the callback is handed the four. Offering somebody the
        name of the twelfth and then measuring the second is a measurement of
        something they did not patch.

        And the pairing cannot be taken from the documentation, which is why
        every backend was read instead. juce_AudioIODevice.h says the channel
        data "is arranged with the same array indices as the channel name array
        returned by AudioIODevice::getOutputChannelNames(), but those channels
        that aren't specified in AudioIODevice::open() will have a null pointer
        for their associated channel", which would mean indexing by the name
        array and checking for nulls. Its very next paragraph says there is one
        channel of data per enabled channel, which is the opposite. Every
        backend shipped with 9.0.3 does the second: ALSA appends a pointer only
        where `inputChannels[i]` is set, ASIO passes `numActiveInputChans`
        buffers, and WASAPI and CoreAudio both pass a buffer sized to the
        active count. So the array is the active channels, compacted, in
        ascending order, with no gaps, which is what this returns.
    */
    struct Channel
    {
        int           index = 0;      /**< as the callback indexes it */
        juce::String  name;
    };

    static juce::Array<Channel> activeChannels (juce::AudioIODevice& device, bool input)
    {
        juce::Array<Channel> result;

        const auto names  = input ? device.getInputChannelNames()  : device.getOutputChannelNames();
        const auto active = input ? device.getActiveInputChannels() : device.getActiveOutputChannels();

        int callbackIndex = 0;

        for (int i = 0; i < names.size(); ++i)
            if (active[i])
                result.add ({ callbackIndex++, names[i] });

        return result;
    }

private:
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override
    {
        const double rate = device != nullptr ? device->getCurrentSampleRate() : 0.0;

        sampleRate.store (rate, std::memory_order_relaxed);
        elapsed.store (0, std::memory_order_relaxed);

        probe.reset (rate);
        sinceResolve = 0;

        // The resolve interval, in samples: eight times a second, which is
        // often enough that the panel reads as live and rare enough that
        // thirteen arctangents on the audio thread do not matter. Never zero,
        // because a device can report no rate at all and a zero interval
        // would resolve every sample.
        resolveInterval = rate > 0.0 ? (int) (rate / 8.0) : 1 << 20;
    }

    void audioDeviceStopped() override
    {
        sampleRate.store (0.0, std::memory_order_relaxed);
    }

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        // Every channel, because this callback is handed a scratch buffer that
        // nothing cleared and whatever is left in it is summed into the mix.
        // See the note at the top of this file.
        for (int channel = 0; channel < numOutputChannels; ++channel)
            if (auto* out = outputChannelData[channel])
                juce::FloatVectorOperations::clear (out, numSamples);

        if (numSamples <= 0)
            return;

        const float* in = (inputIndex >= 0 && inputIndex < numInputChannels)
                              ? inputChannelData[inputIndex]
                              : nullptr;

        float* out = (outputIndex >= 0 && outputIndex < numOutputChannels)
                         ? outputChannelData[outputIndex]
                         : nullptr;

        // Deliberately called even when there is no output channel to play
        // down, so that a device with nothing patchable still reports no
        // signal rather than sitting there looking busy.
        probe.process (in, out, numSamples);

        elapsed.fetch_add (numSamples, std::memory_order_relaxed);
        sinceResolve += numSamples;

        if (sinceResolve >= resolveInterval)
        {
            sinceResolve = 0;
            published.store (LatencyProbe::pack (probe.read()), std::memory_order_relaxed);
        }
    }

    juce::AudioDeviceManager& devices;
    LatencyProbe::Probe       probe;

    std::atomic<std::uint64_t> published { 0 };
    std::atomic<long long>     elapsed   { 0 };
    std::atomic<double>        sampleRate { 0.0 };

    bool running         = false;
    int  outputIndex     = -1;
    int  inputIndex      = -1;
    int  sinceResolve    = 0;
    int  resolveInterval = 1 << 20;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LatencyCalibration)
};
