#pragma once
#include <juce_audio_basics/juce_audio_basics.h>

//==============================================================================
/** A fixed whole-sample delay, used to line up signal paths of different
    lengths.

    Plugins that look ahead (limiters, mastering processors, spectral effects)
    report how many samples they hold back. Without compensation a track
    carrying one of those plays late against the others, and the amount changes
    whenever a plugin is loaded, so timing drifts as a project grows. Delaying
    every shorter path by the difference puts them all back in line.
*/
class LatencyDelay
{
public:
    /** Makes room for a delay of up to maxDelaySamples, processed in blocks of
        at most maxBlockSamples. Safe to call again with larger values. */
    void prepare (int numChannels, int maxDelaySamples, int maxBlockSamples)
    {
        const int wanted = juce::jmax (1, maxDelaySamples + maxBlockSamples + 1);

        if (ring.getNumChannels() < numChannels || size < wanted)
        {
            size = wanted;
            ring.setSize (juce::jmax (numChannels, ring.getNumChannels()), size, false, true, true);
        }

        reset();
    }

    void reset() noexcept
    {
        ring.clear();
        writePos = 0;
    }

    /** Changing the delay invalidates what is already in flight, so the buffer
        is cleared rather than replaying samples from the wrong offset. */
    void setDelay (int samples) noexcept
    {
        const int wanted = juce::jlimit (0, juce::jmax (0, size - 1), samples);

        if (wanted != delay)
        {
            delay = wanted;
            reset();
        }
    }

    int getDelay() const noexcept { return delay; }

    /** Delays the first numChannels channels of the buffer in place. */
    void process (juce::AudioBuffer<float>& buffer, int numChannels, int numSamples) noexcept
    {
        if (delay <= 0 || size <= 0 || numSamples <= 0)
            return;

        const int channels = juce::jmin (numChannels, buffer.getNumChannels(), ring.getNumChannels());

        for (int ch = 0; ch < channels; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            auto* line = ring.getWritePointer (ch);
            int   w    = writePos;

            for (int i = 0; i < numSamples; ++i)
            {
                int readPos = w - delay;
                if (readPos < 0)
                    readPos += size;

                const float delayed = line[readPos];
                line[w] = data[i];
                data[i] = delayed;

                if (++w >= size)
                    w = 0;
            }
        }

        writePos += numSamples;
        while (writePos >= size)
            writePos -= size;
    }

private:
    juce::AudioBuffer<float> ring;
    int size = 0, writePos = 0, delay = 0;
};
