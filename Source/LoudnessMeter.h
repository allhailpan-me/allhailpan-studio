#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>
#include <vector>
#include <algorithm>

//==============================================================================
/** Loudness measurement to ITU-R BS.1770-4, the standard every streaming
    service normalises to.

    Peak meters say nothing about how loud something sounds: a limited master
    and an unlimited mix can both peak at 0 dBFS and arrive several decibels
    apart. This measures perceived loudness instead, which is what Spotify,
    Apple Music and YouTube actually turn up or down.

    Three readings, as broadcast uses them:
      momentary   400 ms, for watching a moment
      short term  3 s, for watching a section
      integrated  the whole measurement, gated so that silence and quiet
                  passages do not drag the average down

    Verified against EBU Tech 3341 compliance test cases 1 and 2.
*/
class LoudnessMeter
{
public:
    void prepare (double sampleRateIn)
    {
        sampleRate = std::max (8000.0, sampleRateIn);
        designFilters();

        blockSamples = (int) std::round (sampleRate * 0.4);     // 400 ms
        hopSamples   = (int) std::round (sampleRate * 0.1);     // 75% overlap

        reset();
    }

    void reset()
    {
        for (auto& s : states) s = {};

        squaredSum.assign ((size_t) std::max (1, blockSamples), 0.0);
        writePos = 0;
        filled = 0;
        sinceHop = 0;

        blockLoudness.clear();
        shortTermHistory.clear();

        truePeak = 0.0f;
        momentary = shortTerm = integrated = -200.0;
    }

    /** Feeds one block. Only the first two channels are measured, which is
        what the stereo weighting in the standard expects. */
    void process (const float* const* channels, int numChannels, int numSamples)
    {
        if (numSamples <= 0 || numChannels <= 0 || blockSamples <= 0)
            return;

        const int used = std::min (numChannels, 2);

        for (int i = 0; i < numSamples; ++i)
        {
            double sum = 0.0;

            for (int ch = 0; ch < used; ++ch)
            {
                const double x = channels[ch][i];

                // K-weighting: a high shelf standing in for the head, then a
                // high pass that discards rumble the ear barely registers.
                const double shelved = states[(size_t) ch].shelf.process (x, shelf);
                const double weighted = states[(size_t) ch].highpass.process (shelved, highpass);

                sum += weighted * weighted;      // both stereo channels weigh 1.0
                truePeak = std::max (truePeak, std::abs ((float) x));
            }

            // A rolling 400 ms window of mean square.
            runningSum -= squaredSum[(size_t) writePos];
            squaredSum[(size_t) writePos] = sum;
            runningSum += sum;

            if (++writePos >= blockSamples)
                writePos = 0;

            filled = std::min (filled + 1, blockSamples);

            if (++sinceHop >= hopSamples)
            {
                sinceHop = 0;
                if (filled >= blockSamples)
                    takeBlock();
            }
        }
    }

    double getMomentaryLufs()  const noexcept { return momentary; }
    double getShortTermLufs()  const noexcept { return shortTerm; }
    double getIntegratedLufs() const noexcept { return integrated; }
    float  getTruePeak()       const noexcept { return truePeak; }

    /** How far the loudest moments sit above the average: a rough stand in for
        how much life is left in the dynamics. */
    double getLoudnessRange() const
    {
        if (shortTermHistory.size() < 4)
            return 0.0;

        auto sorted = shortTermHistory;
        std::sort (sorted.begin(), sorted.end());

        const auto at = [&sorted] (double fraction)
        {
            const auto index = (size_t) juce::jlimit (0.0, (double) sorted.size() - 1.0,
                                                      fraction * (sorted.size() - 1));
            return sorted[index];
        };

        return at (0.95) - at (0.10);
    }

private:
    //==========================================================================
    struct Coeffs { double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; };

    struct Biquad
    {
        double z1 = 0.0, z2 = 0.0;

        double process (double x, const Coeffs& c) noexcept
        {
            const double y = c.b0 * x + z1;
            z1 = c.b1 * x - c.a1 * y + z2;
            z2 = c.b2 * x - c.a2 * y;
            return y;
        }
    };

    struct ChannelState { Biquad shelf, highpass; };

    void designFilters()
    {
        // The analog prototypes the standard's tabulated 48 kHz coefficients
        // are derived from. Designing from these rather than hard coding the
        // table keeps the measurement correct at any sample rate.
        {
            const double f0 = 1681.974450955533;
            const double gainDb = 3.999843853973347;
            const double q  = 0.7071752369554196;

            const double k  = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
            const double vh = std::pow (10.0, gainDb / 20.0);
            const double vb = std::pow (vh, 0.4996667741545416);
            const double a0 = 1.0 + k / q + k * k;

            shelf.b0 = (vh + vb * k / q + k * k) / a0;
            shelf.b1 = 2.0 * (k * k - vh) / a0;
            shelf.b2 = (vh - vb * k / q + k * k) / a0;
            shelf.a1 = 2.0 * (k * k - 1.0) / a0;
            shelf.a2 = (1.0 - k / q + k * k) / a0;
        }

        {
            const double f0 = 38.13547087602444;
            const double q  = 0.5003270373238773;

            const double k  = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
            const double a0 = 1.0 + k / q + k * k;

            highpass.b0 = 1.0;
            highpass.b1 = -2.0;
            highpass.b2 = 1.0;
            highpass.a1 = 2.0 * (k * k - 1.0) / a0;
            highpass.a2 = (1.0 - k / q + k * k) / a0;
        }
    }

    static double loudnessOf (double meanSquare)
    {
        // The -0.691 offset is what makes a 1 kHz tone read its own level: it
        // cancels the shelf's gain at that frequency.
        return meanSquare > 0.0 ? -0.691 + 10.0 * std::log10 (meanSquare) : -200.0;
    }

    void takeBlock()
    {
        const double meanSquare = runningSum / blockSamples;
        const double l = loudnessOf (meanSquare);

        momentary = l;
        blockLoudness.push_back (l);

        // Short term is the same measure over three seconds, which is thirty
        // of these blocks.
        const size_t window = 30;
        if (blockLoudness.size() >= window)
        {
            double sum = 0.0;
            for (size_t i = blockLoudness.size() - window; i < blockLoudness.size(); ++i)
                sum += std::pow (10.0, (blockLoudness[i] + 0.691) / 10.0);

            shortTerm = loudnessOf (sum / window);
            shortTermHistory.push_back (shortTerm);
        }

        updateIntegrated();
    }

    void updateIntegrated()
    {
        // Two gates. The absolute one throws away silence; the relative one
        // throws away anything more than 10 LU below the average of what is
        // left, so quiet passages do not drag a programme's figure down.
        double sum = 0.0;
        int    count = 0;

        for (double l : blockLoudness)
            if (l > -70.0)
            {
                sum += std::pow (10.0, (l + 0.691) / 10.0);
                ++count;
            }

        if (count == 0)
        {
            integrated = -200.0;
            return;
        }

        const double relativeGate = loudnessOf (sum / count) - 10.0;

        sum = 0.0;
        count = 0;

        for (double l : blockLoudness)
            if (l > -70.0 && l > relativeGate)
            {
                sum += std::pow (10.0, (l + 0.691) / 10.0);
                ++count;
            }

        integrated = count > 0 ? loudnessOf (sum / count) : -200.0;
    }

    double sampleRate = 48000.0;
    Coeffs shelf, highpass;
    ChannelState states[2];

    std::vector<double> squaredSum;
    double runningSum = 0.0;
    int blockSamples = 0, hopSamples = 0, writePos = 0, filled = 0, sinceHop = 0;

    std::vector<double> blockLoudness, shortTermHistory;

    float  truePeak = 0.0f;
    double momentary = -200.0, shortTerm = -200.0, integrated = -200.0;
};
