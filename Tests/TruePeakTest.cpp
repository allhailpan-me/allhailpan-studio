// ---------------------------------------------------------------------------
// Checks true-peak detection against the standard that defines it.
//
// A true-peak reading is only worth having if it agrees with every other
// meter that claims to measure one, so this does not check that the number
// looks sensible. It checks the two things that make it a true-peak reading
// at all: that the filter is the one ITU-R BS.1770-4 Annex 2 tabulates,
// coefficient for coefficient, and that the detector returns what EBU Tech
// 3341 says a conforming meter must return for its test signals.
//
// Those test signals exist because this measurement has one failure mode that
// looks like success. A sample peak meter reports a number, the number is
// plausible, and it is three decibels low on exactly the material where that
// matters. Cases 16 and 19 are built to catch it: the samples sit either side
// of the crest, so the stored values never reach what the waveform does.
//
//   ./Tests/run.sh
//
// Sources:
//   ITU-R BS.1770-4, Annex 2, Table 3 (the 48 tap, 4 phase interpolator)
//   EBU Tech 3341, Table 1, cases 15 to 19
// ---------------------------------------------------------------------------

#include "TruePeak.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;

static void fail (const std::string& what)
{
    std::printf ("  FAIL  %s\n", what.c_str());
    ++failures;
}

static void check (bool ok, const std::string& what)
{
    if (! ok)
        fail (what);
}

static constexpr double pi = 3.14159265358979323846;

static double toDb (double linear)
{
    return linear > 0.0 ? 20.0 * std::log10 (linear) : -200.0;
}

//==============================================================================
/** The table as ITU-R BS.1770-4 Annex 2 prints it: twelve rows, one column per
    phase. Written out in the document's own layout rather than the header's,
    so that comparing the two is a real check and not a copy of a copy. */
static const double publishedTable[12][4] =
{
    {  0.0017089843750, -0.0291748046875, -0.0189208984375, -0.0083007812500 },
    {  0.0109863281250,  0.0292968750000,  0.0330810546875,  0.0148925781250 },
    { -0.0196533203125, -0.0517578125000, -0.0582275390625, -0.0266113281250 },
    {  0.0332031250000,  0.0891113281250,  0.1015625000000,  0.0476074218750 },
    { -0.0594482421875, -0.1665039062500, -0.2003173828125, -0.1022949218750 },
    {  0.1373291015625,  0.4650878906250,  0.7797851562500,  0.9721679687500 },
    {  0.9721679687500,  0.7797851562500,  0.4650878906250,  0.1373291015625 },
    { -0.1022949218750, -0.2003173828125, -0.1665039062500, -0.0594482421875 },
    {  0.0476074218750,  0.1015625000000,  0.0891113281250,  0.0332031250000 },
    { -0.0266113281250, -0.0582275390625, -0.0517578125000, -0.0196533203125 },
    {  0.0148925781250,  0.0330810546875,  0.0292968750000,  0.0109863281250 },
    { -0.0083007812500, -0.0189208984375, -0.0291748046875,  0.0017089843750 }
};

/** A direct reading of the published table: the obvious, slow implementation,
    written straight from the document's layout rather than from the header.

    The header stores the same numbers reversed and grouped by phase, because
    that is what makes its inner loop a forward walk. That rearrangement is
    exactly the kind of thing that is easy to get subtly wrong and impossible
    to see by reading, so it is checked against this, which has no
    rearrangement in it at all. */
static double referenceTruePeak (const std::vector<float>& signal, size_t upTo)
{
    double peak = 0.0;

    for (size_t n = 0; n < upTo; ++n)
    {
        for (int phase = 0; phase < 4; ++phase)
        {
            double acc = 0.0;

            // y_p[n] = sum over k of h_p[k] * x[n - k], with h_p[k] the
            // table's row k, column p.
            for (int k = 0; k < 12; ++k)
                if (n >= (size_t) k)
                    acc += publishedTable[k][phase] * (double) signal[n - (size_t) k];

            peak = std::max (peak, std::abs (acc));
        }
    }

    return peak;
}

static void filterMatchesTheStandard()
{
    std::printf ("the detector computes what the published table says it should\n");

    // A signal with content everywhere, so every tap and every phase is
    // exercised and nothing cancels by luck. Deterministic, so a failure is
    // reproducible.
    std::vector<float> signal (4096);
    unsigned state = 2463534242u;

    for (auto& s : signal)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        s = (float) ((double) state / 2147483648.0 - 1.0);
    }

    TruePeakDetector detector;
    detector.reset();

    double running = 0.0;
    size_t worstAt = 0;
    double worstGap = 0.0;

    for (size_t n = 0; n < signal.size(); ++n)
    {
        running = std::max (running, (double) detector.process (signal[n]));

        const double want = referenceTruePeak (signal, n + 1);
        const double gap  = std::abs (running - want);

        if (gap > worstGap)
        {
            worstGap = gap;
            worstAt  = n;
        }
    }

    // Single precision history against double precision reference, so this is
    // a tight tolerance rather than an exact one.
    check (worstGap < 1e-6,
           "the detector and the published table disagree by " + std::to_string (worstGap)
           + ", first worst at sample " + std::to_string (worstAt));
}

/** Properties of the published table that a transcription error breaks. */
static void theTableIsSelfConsistent()
{
    std::printf ("the published table has the shape a linear phase interpolator must\n");

    // Each phase is a fractional delay of the same signal, so each must pass
    // a constant through unchanged. The standard's values are quantised for
    // integer arithmetic, so this is near one rather than exactly one, and
    // that small ripple is the meter's own noise floor.
    for (int phase = 0; phase < 4; ++phase)
    {
        double sum = 0.0;
        for (int row = 0; row < 12; ++row)
            sum += publishedTable[row][phase];

        if (std::abs (sum - 1.0) > 0.03)
            fail ("phase " + std::to_string (phase) + " sums to " + std::to_string (sum)
                  + ", which is not a unity gain interpolator");
    }

    // Interleaved back into one 48 tap filter it must be symmetric, which is
    // what makes it linear phase. A phase column swapped or reversed during
    // transcription breaks this and almost nothing else.
    double h[48];
    for (int row = 0; row < 12; ++row)
        for (int phase = 0; phase < 4; ++phase)
            h[row * 4 + phase] = publishedTable[row][phase];

    for (int i = 0; i < 24; ++i)
        if (std::abs (h[i] - h[47 - i]) > 1e-12)
            fail ("the interleaved filter is not symmetric at tap " + std::to_string (i));
}

//==============================================================================
/** EBU Tech 3341 asks for a 10 ms fade in and fade out on these tones, and the
    reason is worth stating: without it the tone begins as a step, a step has
    energy at every frequency, and the meter correctly reports the overshoot
    that step produces. That reading would be real but it would be measuring
    the test signal's edge rather than the tone. */
static std::vector<float> taperedTone (double cyclesPerSample, double amplitude,
                                       double phaseDegrees, int numSamples,
                                       double sampleRate = 48000.0)
{
    std::vector<float> out ((size_t) numSamples);

    const double phase = phaseDegrees * pi / 180.0;
    for (int i = 0; i < numSamples; ++i)
        out[(size_t) i] = (float) (amplitude * std::sin (2.0 * pi * cyclesPerSample * i + phase));

    const int fade = (int) (sampleRate * 0.010);

    for (int i = 0; i < fade && i < numSamples / 2; ++i)
    {
        const float w = (float) (0.5 - 0.5 * std::cos (pi * i / fade));
        out[(size_t) i] *= w;
        out[(size_t) (numSamples - 1 - i)] *= w;
    }

    return out;
}

static double measure (const std::vector<float>& signal)
{
    TruePeakDetector detector;
    detector.reset();

    float peak = 0.0f;
    for (float s : signal)
        peak = std::max (peak, detector.process (s));

    return toDb (peak);
}

static double samplePeakDb (const std::vector<float>& signal)
{
    float peak = 0.0f;
    for (float s : signal)
        peak = std::max (peak, std::abs (s));

    return toDb (peak);
}

static void ebuComplianceCases()
{
    std::printf ("EBU Tech 3341 cases 15 to 19\n");

    struct Case
    {
        const char* name;
        double      divisor;        // fs / divisor
        double      amplitude;
        double      phaseDegrees;
        double      expected;       // dBTP
    };

    // Tolerance is asymmetric in the standard: +0.2 / -0.4 dBTP. Reading a
    // little low is tolerated because four times oversampling cannot find
    // every peak; reading high is not, because an over-read would have a
    // mastering engineer pulling a mix down for no reason.
    const Case cases[] =
    {
        { "15  fs/4, 0.50, 0 degrees",     4.0, 0.50, 0.0,  -6.0 },
        { "16  fs/4, 0.50, 45 degrees",    4.0, 0.50, 45.0, -6.0 },
        { "17  fs/6, 0.50, 60 degrees",    6.0, 0.50, 60.0, -6.0 },
        { "18  fs/8, 0.50, 67.5 degrees",  8.0, 0.50, 67.5, -6.0 },
        { "19  fs/4, 1.41, 45 degrees",    4.0, 1.41, 45.0,  3.0 }
    };

    for (const auto& c : cases)
    {
        const auto signal = taperedTone (1.0 / c.divisor, c.amplitude, c.phaseDegrees, 48000);
        const double got = measure (signal);

        const bool ok = got <= c.expected + 0.2 && got >= c.expected - 0.4;

        std::printf ("    %-30s sample peak %+6.2f dBFS   true peak %+6.2f dBTP   %s\n",
                     c.name, samplePeakDb (signal), got, ok ? "ok" : "OUT OF TOLERANCE");

        if (! ok)
            fail (std::string (c.name) + ": read " + std::to_string (got)
                  + " dBTP, the standard requires " + std::to_string (c.expected)
                  + " +0.2 / -0.4");
    }
}

static void findsWhatASamplePeakMeterCannot()
{
    std::printf ("the peaks a sample peak meter misses are actually found\n");

    // Case 19 is the one that matters in a studio. The signal genuinely
    // reaches +3 dBTP and will distort in any converter or lossy encoder, and
    // a sample peak meter reports it as sitting just under full scale, which
    // reads as safe. If this ever stops being true the mix report goes back
    // to telling people a clipping master is fine.
    const auto signal = taperedTone (0.25, 1.41, 45.0, 48000);

    const double sampled = samplePeakDb (signal);
    const double actual  = measure (signal);

    check (sampled < 0.0, "the test signal should not reach full scale in its samples");
    check (actual > 2.5, "a signal that reaches +3 dBTP was not detected above 2.5 dBTP");
    check (actual - sampled > 2.5,
           "true peak should sit about 3 dB above sample peak here, the gap was "
           + std::to_string (actual - sampled));
}

static void steadySignalsAreNotInflated()
{
    std::printf ("signals with nothing between the samples read their own level\n");

    // The opposite failure. A meter that reports every signal as hotter than
    // it is would have people leaving headroom they do not need. The ripple
    // allowed here is the standard's own: its coefficients are quantised to
    // binary fractions, so the interpolator's gain is not exactly one.
    {
        std::vector<float> dc (48000, 1.0f);
        for (int i = 0; i < 480; ++i)
        {
            const float w = (float) (0.5 - 0.5 * std::cos (pi * i / 480));
            dc[(size_t) i] *= w;
            dc[(size_t) (dc.size() - 1 - (size_t) i)] *= w;
        }

        const double got = measure (dc);
        check (std::abs (got) < 0.2, "full scale DC read " + std::to_string (got) + " dBTP");
    }

    {
        // A slow sine is sampled densely enough that there is nothing hiding
        // between its samples.
        const auto slow = taperedTone (100.0 / 48000.0, 1.0, 0.0, 48000);
        const double got = measure (slow);
        check (std::abs (got) < 0.2,
               "a full scale 100 Hz sine read " + std::to_string (got) + " dBTP");
    }

    {
        const auto quiet = taperedTone (1000.0 / 48000.0, 0.001, 0.0, 48000);
        const double got = measure (quiet);
        check (std::abs (got - (-60.0)) < 0.3,
               "a -60 dBFS tone read " + std::to_string (got) + " dBTP");
    }
}

static void silenceAndResetBehave()
{
    std::printf ("silence reads as silence, and reset forgets\n");

    TruePeakDetector detector;
    detector.reset();

    float peak = 0.0f;
    for (int i = 0; i < 1000; ++i)
        peak = std::max (peak, detector.process (0.0f));

    check (peak == 0.0f, "silence produced a peak");

    for (int i = 0; i < 100; ++i)
        (void) detector.process (1.0f);

    detector.reset();

    float after = 0.0f;
    for (int i = 0; i < 100; ++i)
        after = std::max (after, detector.process (0.0f));

    check (after == 0.0f, "reset left audio in the filter");
}

int main()
{
    std::printf ("true peak\n");

    filterMatchesTheStandard();
    theTableIsSelfConsistent();
    ebuComplianceCases();
    findsWhatASamplePeakMeterCannot();
    steadySignalsAreNotInflated();
    silenceAndResetBehave();

    if (failures > 0)
    {
        std::printf ("\n%d true peak check(s) failed\n", failures);
        return 1;
    }

    std::printf ("all true peak checks passed\n");
    return 0;
}
