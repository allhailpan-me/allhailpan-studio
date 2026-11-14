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
    order 48, 4 phase FIR interpolator tabulated in Annex 2, and take the
    largest absolute value of the result. The coefficients are the standard's
    own, not a filter of our own design, because a true-peak reading is only
    meaningful if it agrees with every other meter that claims to measure one.

    **The reading also takes in the sample itself, and has to.** The waveform a
    converter reconstructs passes through the stored samples, so the true peak
    can never be below the sample peak: that is a lower bound, not an
    estimate. The standard's own example coefficients do not respect it. Their
    four phases land one eighth, three eighths, five eighths and seven eighths
    of the way between one input sample and the next, so the filter never
    reproduces a stored value, and two of the four phases are a further
    0.238 dB light at DC. A single full scale sample with silence around it,
    which is what a tight transient or a click looks like, came out at
    -0.245 dBTP, and a full scale 16 kHz tone sampled on its crests at
    -0.265 dBTP. Both are below a peak the file plainly contains.

    That is the shape of fault this meter has had before, and it reached the
    owner: `MixReportPanel` warns above 0 dBTP and cautions above minus one.
    Being worth 0.265 dB, the under-read flipped that verdict for a master
    whose samples peak between about -1.0 and -0.74 dBFS, which read as
    under minus one and so as having headroom it did not have. It never hid a
    master that was already over, which is the one mercy in it.
    Seeding the maximum with the sample's own
    magnitude cannot ever push the reading above the true peak, because the
    sample peak is below it by the same theorem, so it removes an under-read
    and risks no over-read.

    What is left is the standard's own: the four phase grid is 4x, so a crest
    between grid points is still missed by up to the bound Appendix 1 to
    Annex 2 gives, and the per phase ripple can read a steady tone up to
    0.222 dB high (measured at 12.14 kHz). EBU Tech 3341 allows +0.2 / -0.4
    dBTP and says plainly that the allowance has to cover both the passband
    ripple and that under-read, so neither is slack to spend: do not rescale
    the coefficients to flatten the ripple, because then they stop being the
    standard's.

    Verified against EBU Tech 3341 v4.0 compliance cases 15 to 19, which is
    what `Tests/TruePeakTest.cpp` runs. **Cases 20 to 23 are also true-peak
    cases and are not run**: they need the EBU's own published audio rather
    than a tone this file can generate, and a reconstruction from the prose
    would be checking our reading of the prose. Written down so the gap stays
    a gap rather than becoming a claim.

    Annex 2 states its figures for a 48 kHz sample rate and says a higher rate
    needs proportionately less oversampling, so an unconditional 4x is right
    at 44.1 and 48 kHz and more than is needed above that. It is kept
    unconditional because a branch on sample rate is a branch that can be
    wrong, and dropping to the 2x that would do above 48 kHz would save two
    phases of four, which is 24 multiply-accumulates a sample a channel.

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

    /** Feeds one sample and returns a value to fold into a running maximum.

        Not a reading aligned to this sample: it is the larger of this
        sample's own magnitude and what the reconstruction reaches around five
        and a half samples back, which is where this filter's phases sit. That
        is all a peak detector needs, since every sample's magnitude and every
        inter-sample position is offered exactly once, and it is why the only
        caller, `LoudnessMeter`, maxes the result rather than reading it. */
    float process (float x) noexcept
    {
        history[(size_t) writePos] = x;

        if (++writePos >= taps)
            writePos = 0;

        // The reconstruction passes through the stored samples, so the true
        // peak is never below the sample peak. The standard's own phases all
        // land strictly between input samples and so never see this value;
        // leaving it out measured 0.265 dB low on a full scale tone sampled
        // on its crests, which is the material a mastering engineer is
        // checking.
        float peak = std::abs (x);

        for (int phase = 0; phase < phases; ++phase)
        {
            // Accumulated in double, which makes the sum exact rather than
            // merely better: every coefficient is a multiple of 1/8192, so a
            // coefficient times a float sample needs 37 bits of the 53 there
            // are, and twelve of them at audio magnitudes still fit. In
            // single precision the same sum drifts by 3e-7, which is nothing
            // audible but is enough to stop the test comparing against the
            // published table to the last bit.
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

    //==========================================================================
    /** One coefficient, in the layout the recommendation prints: row 0 to 11
        down the page, phase 0 to 3 across it.

        Public so that the test can compare the stored table against the
        document in the document's own layout. What that catches, and nothing
        else does, is the rearrangement: each row reversed, or the four rows
        in another order, leaves every reading bit for bit identical for the
        symmetry reason given below, so the output cannot see it. The values
        themselves are pinned by the output comparison, which is exact, and
        catches a single coefficient moved by one float ulp.

        That makes this accessor the one check on the half of the table that
        is a convention rather than a number, which is worth having precisely
        because a convention nothing checks is where the next reader's
        rearrangement goes wrong.

        Rows run 0 to 11 and phases 0 to 3. Out of range is out of bounds:
        only the test calls this, and it walks the table. */
    static constexpr float published (int row, int phase) noexcept
    {
        return coefficients[(size_t) phase][(size_t) (taps - 1 - row)];
    }

    static constexpr int phases = 4;
    static constexpr int taps   = 12;

private:

    /** The coefficients of ITU-R BS.1770-4 Annex 2: the order 48, 4 phase FIR
        interpolating filter, stored one row per phase and reversed, so the
        inner loop above can walk the history forwards.

        The table is unnumbered and uncaptioned in the recommendation, named
        only by its four column headings. It is not Table 3, which is the
        channel weightings in Annex 1, and two places in this repository used
        to say it was.

        Storing it reversed, and walking the history forwards, are two halves
        of one convention and neither is load bearing on its own. The 48 taps
        interleave into a filter that is exactly symmetric about tap 23.5, so
        reversing each row, or reordering the four rows, turns phase p into
        phase 3 - p and leaves the maximum over the four phases untouched.
        Both were tried and neither moved a single reading. So this is written
        for a reader rather than for correctness, and the test checks it
        against the document's own layout value by value, because that is the
        only thing that can tell a mistyped digit from a convention.

        Every value is an exact multiple of 1/8192 because the standard
        intends the filter to be usable in integer arithmetic, which also
        makes all 48 exactly representable in a float. That intent is why the
        standard describes attenuating by 12.04 dB before the filter and
        amplifying by the same afterwards: it is headroom for a fixed point
        implementation, and the standard says plainly that the step is not
        needed in floating point. The pair is left out because minus 12.04 dB
        followed by plus 12.04 dB is identity here, which is true whatever the
        filter's gain. It is worth saying that it does not follow from the
        gain, because the gain is not one: phases 0 and 3 sum to
        1.0015869140625 at DC and phases 1 and 2 to 0.9730224609375, and that
        0.238 dB deficit on two phases of four is the largest single error in
        the meter. It belongs to the standard's own coefficients, not to this
        code, and correcting it would mean no longer measuring what every
        other meter measures.
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
