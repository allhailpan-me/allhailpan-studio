#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include "TruePeak.h"
#include <cmath>
#include <array>
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

    Peak is measured as true peak, to BS.1770-4 Annex 2, not as the largest
    stored sample. See TruePeak.h for why those are not the same number.

    Loudness range follows EBU Tech 3342: the spread between the 95th and the
    10th percentile of the short term readings, gated absolutely at -70 LUFS
    and then relatively at 20 LU below the mean of what is left. The gates are
    the whole point of the measure: without them a fade in or a silent bar at
    the top of a song reads as enormous dynamic range.

    Everything here runs on the audio thread, which cannot allocate, so the
    history is kept as fixed histograms rather than as growing lists. That is
    not only about allocation: a plain list means the integrated reading
    rescans an hour of blocks every hundred milliseconds and the percentiles
    copy and sort that list on every single block, so the cost of metering
    would grow for as long as the session stayed open. Histograms make both
    bounded, and are how the reference implementations do it for the same
    reason.

    Verified against EBU Tech 3341 compliance test cases 1 and 2, and cases 15
    to 19 for the peak reading.
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

        // Allocated here, on the message thread. reset() is reachable from
        // the audio thread, so it may only clear these, never resize them.
        squaredSum.assign ((size_t) std::max (1, blockSamples), 0.0);
        blockEnergy.assign ((size_t) numBins, 0.0);
        blockCount.assign ((size_t) numBins, 0);
        shortCount.assign ((size_t) numBins, 0);

        reset();
    }

    void reset()
    {
        for (auto& s : states) s = {};
        for (auto& d : peakDetectors) d.reset();

        std::fill (squaredSum.begin(), squaredSum.end(), 0.0);
        runningSum = 0.0;
        writePos = 0;
        filled = 0;
        sinceHop = 0;

        std::fill (blockEnergy.begin(), blockEnergy.end(), 0.0);
        std::fill (blockCount.begin(), blockCount.end(), 0);
        std::fill (shortCount.begin(), shortCount.end(), 0);
        blockRing.fill (0.0);
        blockRingPos = 0;
        blockRingSum = 0.0;
        blocksSeen = 0;

        gatedEnergy = 0.0;
        gatedBlocks = 0;
        shortGatedEnergy = 0.0;
        shortGatedCount = 0;

        truePeak = 0.0f;
        momentary = shortTerm = integrated = -200.0;
        range = 0.0;
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

                // Measured from the unweighted signal: K-weighting is about
                // how loud something sounds, and a converter clips on what
                // the waveform actually does.
                truePeak = std::max (truePeak, peakDetectors[(size_t) ch].process ((float) x));
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
    /** The highest level the reconstructed waveform reaches, as a linear
        amplitude. Can exceed 1.0, and routinely does on a limited master:
        that is the point of measuring it. */
    float  getTruePeak()       const noexcept { return truePeak; }

    /** Loudness range to EBU Tech 3342, in LU: how far the loudest moments sit
        above the quiet ones once silence and fades have been gated out. A
        rough stand in for how much life is left in the dynamics.

        Computed when a new short term reading appears rather than when this
        is called, because this is called from the audio callback and the work
        is a histogram scan. */
    double getLoudnessRange() const noexcept { return range; }

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

    TruePeakDetector peakDetectors[2];

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

    // ---- the history, as histograms -------------------------------------
    //
    // A gated measurement cannot be made incremental: the relative gate
    // depends on the average of everything measured so far, so a block that
    // was included an hour ago can fall below the gate now. What can be
    // bounded is how the distribution is stored. A hundredth of an LU per bin
    // is finer than the tolerance the standard is specified to by a factor of
    // ten, and the bins hold the exact energies rather than the bin's own
    // value, so the only approximation left is which side of a gate the one
    // bin containing it falls on.
    //
    // The range runs from -70, which is the absolute gate and therefore the
    // quietest value that can ever count, to +30, which no mix reaches
    // because a converter would have clipped forty decibels earlier.
    static constexpr double binsPerLu = 100.0;
    static constexpr double lowestLufs = -70.0;
    static constexpr double highestLufs = 30.0;
    static constexpr int    numBins = (int) ((highestLufs - lowestLufs) * binsPerLu);

    static int binFor (double loudness) noexcept
    {
        const int bin = (int) std::floor ((loudness - lowestLufs) * binsPerLu);
        return bin < 0 ? 0 : (bin >= numBins ? numBins - 1 : bin);
    }

    static double loudnessAtBin (int bin) noexcept
    {
        return lowestLufs + (bin + 0.5) / binsPerLu;
    }

    void takeBlock()
    {
        const double meanSquare = runningSum / blockSamples;
        const double l = loudnessOf (meanSquare);

        momentary = l;

        // Short term is the same measure over three seconds, which is thirty
        // of these blocks. Kept as a ring of the mean squares rather than of
        // the loudnesses, which is the same number: converting each one to a
        // level and back again only costs two logarithms and a little
        // accuracy.
        blockRingSum -= blockRing[blockRingPos];
        blockRing[blockRingPos] = meanSquare;
        blockRingSum += meanSquare;
        if (++blockRingPos >= blockRing.size())
            blockRingPos = 0;
        ++blocksSeen;

        // The absolute gate throws silence away. It is applied as the block
        // goes in, so the running total below is the ungated mean the
        // relative gate is measured from.
        if (l > -70.0)
        {
            const int bin = binFor (l);
            blockEnergy[(size_t) bin] += meanSquare;
            ++blockCount[(size_t) bin];
            gatedEnergy += meanSquare;
            ++gatedBlocks;
        }

        updateIntegrated();

        if (blocksSeen >= blockRing.size())
        {
            const double shortMeanSquare = blockRingSum / (double) blockRing.size();
            shortTerm = loudnessOf (shortMeanSquare);

            if (shortTerm > -70.0)
            {
                ++shortCount[(size_t) binFor (shortTerm)];
                shortGatedEnergy += shortMeanSquare;
                ++shortGatedCount;
            }

            updateRange();
        }
    }

    void updateIntegrated()
    {
        if (gatedBlocks == 0)
        {
            integrated = -200.0;
            return;
        }

        // The relative gate throws away anything more than 10 LU below the
        // average of what the absolute gate left, so quiet passages do not
        // drag a programme's figure down.
        const double relativeGate = loudnessOf (gatedEnergy / gatedBlocks) - 10.0;
        const int    from = binFor (relativeGate);

        double sum = 0.0;
        int    count = 0;

        for (int bin = from; bin < numBins; ++bin)
        {
            // The bin holding the gate is taken as in or out by its centre.
            // At a hundredth of an LU per bin that decides the fate of blocks
            // within five thousandths of the threshold.
            if (bin == from && loudnessAtBin (bin) <= relativeGate)
                continue;
            sum   += blockEnergy[(size_t) bin];
            count += blockCount[(size_t) bin];
        }

        integrated = count > 0 ? loudnessOf (sum / count) : -200.0;
    }

    void updateRange()
    {
        // Tech 3342: gate the short term readings absolutely at -70 LUFS and
        // then at 20 LU below the mean of what is left, and take the spread
        // between the 95th and the 10th percentile of the rest.
        if (shortGatedCount < 4)
        {
            range = 0.0;
            return;
        }

        const double relativeGate = loudnessOf (shortGatedEnergy / shortGatedCount) - 20.0;
        const int    from = binFor (relativeGate);

        int total = 0;
        for (int bin = from; bin < numBins; ++bin)
        {
            if (bin == from && loudnessAtBin (bin) <= relativeGate)
                continue;
            total += shortCount[(size_t) bin];
        }

        if (total < 4)
        {
            range = 0.0;
            return;
        }

        const auto percentile = [this, from, relativeGate, total] (double fraction)
        {
            // The rank the fraction asks for, counted from the quiet end.
            const int wanted = (int) std::floor (fraction * (total - 1)) + 1;
            int seen = 0;

            for (int bin = from; bin < numBins; ++bin)
            {
                if (bin == from && loudnessAtBin (bin) <= relativeGate)
                    continue;
                seen += shortCount[(size_t) bin];
                if (seen >= wanted)
                    return loudnessAtBin (bin);
            }
            return loudnessAtBin (numBins - 1);
        };

        range = std::max (0.0, percentile (0.95) - percentile (0.10));
    }

    double sampleRate = 48000.0;
    Coeffs shelf, highpass;
    ChannelState states[2];

    std::vector<double> squaredSum;
    double runningSum = 0.0;
    int blockSamples = 0, hopSamples = 0, writePos = 0, filled = 0, sinceHop = 0;

    // The thirty 400 ms blocks the three second reading spans.
    std::array<double, 30> blockRing {};
    size_t blockRingPos = 0;
    double blockRingSum = 0.0;
    size_t blocksSeen = 0;

    std::vector<double> blockEnergy;      // exact energy per bin, blocks past the absolute gate
    std::vector<int>    blockCount;
    std::vector<int>    shortCount;       // short term readings past the absolute gate
    double gatedEnergy = 0.0;
    int    gatedBlocks = 0;
    double shortGatedEnergy = 0.0;
    int    shortGatedCount = 0;

    float  truePeak = 0.0f;
    double momentary = -200.0, shortTerm = -200.0, integrated = -200.0;
    double range = 0.0;
};
