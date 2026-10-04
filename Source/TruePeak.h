#pragma once
#include <algorithm>
#include <array>
#include <cmath>

//==============================================================================
/** True-peak detection to ITU-R BS.1770-4 Annex 2.

    A sample peak meter only sees the points the file actually stores. The
    waveform a converter reconstructs between those points goes higher, and
    how much higher depends on where the samples happen to land on the wave.
    A full scale sine at a quarter of the sampling rate, sampled an eighth of
    a cycle off the crest, stores nothing above 0.707 and reconstructs to 1.0:
    three decibels that a sample peak meter cannot see.

    That gap is not academic. It is why a master that measures 0.0 dBFS
    distorts after an MP3 or AAC encode, and why delivery specifications are
    written in dBTP rather than dBFS. Measuring it needs the signal
    reconstructed between its samples, which is what this does.

    The method is the one the standard specifies: oversample by four with the
    48 tap, 4 phase FIR interpolator tabulated in Annex 2, and take the
    largest absolute value of the result. The coefficients are the standard's
    own, not a filter of our own design, because a true-peak reading is only
    meaningful if it agrees with every other meter that claims to measure one.

    Verified against EBU Tech 3341 compliance cases 15 to 19, which is what
    `Tests/TruePeakTest.cpp` runs.

    No allocation and no branching on sample rate, so this is safe to run from
    the device callback.
*/
class TruePeakDetector
{
public:
    void reset() noexcept
    {
        history.fill (0.0f);
        writePos = 0;
    }

    /** Feeds one sample and returns the largest absolute value the
        reconstructed waveform reaches around it. */
    float process (float x) noexcept
    {
        history[(size_t) writePos] = x;

        if (++writePos >= taps)
            writePos = 0;

        float peak = 0.0f;

        for (int phase = 0; phase < phases; ++phase)
        {
            // Accumulated in double: twelve products of similar magnitude
            // that largely cancel, which is where single precision loses
            // digits, and the cost is nothing next to the multiplies.
            double acc = 0.0;

            // writePos now points at the oldest sample, so walking forward
            // from it runs oldest to newest, which is the order the reversed
            // coefficients below expect.
            for (int j = 0; j < taps; ++j)
            {
                int idx = writePos + j;
                if (idx >= taps)
                    idx -= taps;

                acc += (double) coefficients[(size_t) phase][(size_t) j]
                     * (double) history[(size_t) idx];
            }

            peak = std::max (peak, (float) std::abs (acc));
        }

        return peak;
    }

private:
    static constexpr int phases = 4;
    static constexpr int taps   = 12;

    /** Table 3 of ITU-R BS.1770-4 Annex 2: the order 48, 4 phase FIR
        interpolating filter, stored one row per phase and reversed, so the
        inner loop above can walk the history forwards.

        Every value is an exact binary fraction because the standard intends
        the filter to be usable in integer arithmetic. That is also why the
        standard describes attenuating by 12.04 dB before the filter and
        amplifying by the same afterwards: it is headroom for a fixed point
        implementation, and the standard says plainly that the step is not
        needed in floating point. Each phase here already sums to about one
        at DC, so applying that pair would cancel out and is left out.
    */
    static constexpr std::array<std::array<float, taps>, phases> coefficients
    {{
        // phase 0, reversed
        {{ -0.0083007812500f,  0.0148925781250f, -0.0266113281250f,  0.0476074218750f,
           -0.1022949218750f,  0.9721679687500f,  0.1373291015625f, -0.0594482421875f,
            0.0332031250000f, -0.0196533203125f,  0.0109863281250f,  0.0017089843750f }},
        // phase 1, reversed
        {{ -0.0189208984375f,  0.0330810546875f, -0.0582275390625f,  0.1015625000000f,
           -0.2003173828125f,  0.7797851562500f,  0.4650878906250f, -0.1665039062500f,
            0.0891113281250f, -0.0517578125000f,  0.0292968750000f, -0.0291748046875f }},
        // phase 2, reversed
        {{ -0.0291748046875f,  0.0292968750000f, -0.0517578125000f,  0.0891113281250f,
           -0.1665039062500f,  0.4650878906250f,  0.7797851562500f, -0.2003173828125f,
            0.1015625000000f, -0.0582275390625f,  0.0330810546875f, -0.0189208984375f }},
        // phase 3, reversed
        {{  0.0017089843750f,  0.0109863281250f, -0.0196533203125f,  0.0332031250000f,
           -0.0594482421875f,  0.1373291015625f,  0.9721679687500f, -0.1022949218750f,
            0.0476074218750f, -0.0266113281250f,  0.0148925781250f, -0.0083007812500f }}
    }};

    std::array<float, taps> history {};
    int writePos = 0;
};
