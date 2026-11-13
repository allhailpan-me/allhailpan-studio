// ---------------------------------------------------------------------------
// Checks true-peak detection against the standard that defines it.
//
// A true-peak reading is only worth having if it agrees with every other
// meter that claims to measure one, so this does not check that the number
// looks sensible. It checks three things: that the coefficients are the ones
// ITU-R BS.1770-4 Annex 2 prints, digit for digit; that the reading obeys the
// one bound it can be held to without reference to any standard, which is
// that it is never below the largest sample in the signal; and that the
// detector returns what EBU Tech 3341 says a conforming meter must.
//
// Those test signals exist because this measurement has one failure mode that
// looks like success. A sample peak meter reports a number, the number is
// plausible, and it is three decibels low on exactly the material where that
// matters. Cases 16 and 19 are built to catch it: the samples sit either side
// of the crest, so the stored values never reach what the waveform does.
//
// The bound is the other half of the same idea and this file used not to
// check it. The waveform a converter reconstructs passes through the stored
// samples, so the true peak is at least the sample peak, always. The
// standard's example coefficients put all four of their phases strictly
// between input samples, so a meter built from them alone reads below that
// bound: a lone full scale sample came out at -0.245 dBTP. See
// `theReadingIsNeverBelowTheSamplePeak`, which is run against a
// reconstruction-only reading first and has to catch it.
//
//   ./Tests/run.sh
//
// Sources:
//   ITU-R BS.1770-4, Annex 2: the unnumbered "Phase 0..Phase 3" table of the
//     order 48, 4 phase FIR interpolating filter. Not Table 3, which is the
//     channel weightings in Annex 1.
//   EBU Tech 3341 v4.0 (November 2023), Table 1 in section 2.9. The true-peak
//     cases are 15 to 23, not 15 to 19 as this file used to say. 15 to 19 are
//     tones and are run here; 20 to 23 need the EBU's own published audio and
//     are not, which `Source/TruePeak.h` records as a known gap.
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
    so that comparing the two is a real check and not a copy of a copy.

    This reference can be mistyped too, and then it and a mistyped header
    would agree. So it is held to three structural properties of its own
    before it is used to judge anything: every value an exact multiple of
    1/8192 with the numerator pinned, the interleaved filter exactly
    symmetric, and the four phase sums exact. A wrong digit survives none of
    them. */
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

/** What the interpolator alone reaches at one sample: the largest of its four
    phase outputs there. Written from the document's layout, with no
    rearrangement in it at all. */
static double referenceReconstructionAt (const std::vector<float>& signal, size_t n)
{
    double peak = 0.0;

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

    return peak;
}

/** What a meter built from the interpolator alone would report over a whole
    signal. Kept because it is the behaviour this meter used to have, and
    `theReadingIsNeverBelowTheSamplePeak` needs something known to be wrong to
    point itself at. */
static double referenceReconstructionOnly (const std::vector<float>& signal, size_t upTo)
{
    double peak = 0.0;

    for (size_t n = 0; n < upTo; ++n)
        peak = std::max (peak, referenceReconstructionAt (signal, n));

    return peak;
}

/** What the detector must return for one sample: the reconstruction around it
    and the sample's own magnitude, because the reconstruction passes through
    the samples. */
static double referenceAt (const std::vector<float>& signal, size_t n)
{
    return std::max (referenceReconstructionAt (signal, n), (double) std::abs (signal[n]));
}

static void filterMatchesTheStandard()
{
    std::printf ("the detector computes what the published table says it should\n");

    // A signal with content everywhere, so every tap and every phase is
    // exercised and nothing cancels by luck. Deterministic, so a failure is
    // reproducible.
    std::vector<float> signal (200000);
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

    size_t mismatches = 0;
    size_t firstAt = 0;

    for (size_t n = 0; n < signal.size(); ++n)
    {
        // Compared sample by sample and not as a running maximum, and the
        // reason is a lesson about this change rather than about the old
        // test. A running maximum only moves when a record is set. Taking
        // the sample's own magnitude into the reading raised the floor of
        // both sides to the signal's sample peak, and on a full scale random
        // signal that is most of the way to the largest reconstruction
        // there is, so records that only one phase could set stopped being
        // records. Dropping one of the four phases then passed, where the
        // same comparison without the seed had caught all four by margins of
        // 0.02 to 0.13. The fix made the test weaker, and nothing about the
        // fix said so.
        //
        // Every sample is its own assertion now, and there are two hundred
        // thousand rather than four thousand. It also costs less, because the
        // reference is no longer recomputed from the start of the signal for
        // every sample.
        //
        // Exactly equal, not close. Every coefficient is a multiple of
        // 1/8192 and every sample is a float, so each product needs 37 of
        // double's 53 bits and is exact, and twelve of them at audio
        // magnitudes still are. The only rounding anywhere is the single cast
        // to float on the way out, so the detector's float has to be the
        // correctly rounded form of the reference's double. That is what
        // makes this a check on the arithmetic the header says it does
        // rather than a check with a tolerance somebody can widen:
        // accumulating in single precision disagrees on one sample in three,
        // and rounding only the products to single precision on one in six.
        // Under the old tolerance of a millionth both passed.
        //
        // What makes it exact is the cast, not the products. Exact products
        // do not make an exact sum, and for arbitrary float windows the two
        // summation orders here do differ in double about one time in twelve.
        // The samples below are multiples of 2^-31 and the numerators are
        // under 2^13, so every phase sum is a multiple of 2^-44 bounded by
        // two and needs 46 of the 53 bits: no rounding happens at all, and
        // the 29 bits of slack in the final cast to float would absorb it if
        // it did. So changing the signal generator to something that is not
        // a multiple of a power of two is safe, but only by that margin.
        if (detector.process (signal[n]) != (float) referenceAt (signal, n))
        {
            if (mismatches == 0)
                firstAt = n;

            ++mismatches;
        }
    }

    check (mismatches == 0,
           "the detector and the published table disagree on "
           + std::to_string (mismatches) + " of " + std::to_string (signal.size())
           + " samples, first at " + std::to_string (firstAt));
}

/** Properties of the published table that a transcription error breaks.

    What this used to assert, and could not: that interleaving the phases back
    into one 48 tap filter must give something symmetric, because "a phase
    column swapped or reversed during transcription breaks this and almost
    nothing else". It breaks neither. The table is symmetric in both
    directions at once, so reversing every row, or reversing the order of the
    four phases, leaves the interleaved filter exactly symmetric and turns
    phase p into phase 3 - p, which a maximum over all four phases cannot see.
    Both were tried against the whole of this file and neither moved a
    reading. The symmetry check is kept because it is a real property of the
    document's table, but what actually pins the values is
    `theHeaderHoldsThePublishedTable` below. */
static void theTableIsSelfConsistent()
{
    std::printf ("the published table has the shape the document's own does\n");

    // Every value is an exact multiple of 1/8192, because the standard means
    // the filter to be usable in integer arithmetic. The numerators are
    // pinned, which is the one check a single mistyped digit cannot pass:
    // nothing else here would notice 0.0017089843750 becoming
    // 0.0017098843750, a change of nine parts in ten million.
    static const int numerators[12][4] =
    {
        {   14,  -239,  -155,   -68 },
        {   90,   240,   271,   122 },
        { -161,  -424,  -477,  -218 },
        {  272,   730,   832,   390 },
        { -487, -1364, -1641,  -838 },
        { 1125,  3810,  6388,  7964 },
        { 7964,  6388,  3810,  1125 },
        { -838, -1641, -1364,  -487 },
        {  390,   832,   730,   272 },
        { -218,  -477,  -424,  -161 },
        {  122,   271,   240,    90 },
        {  -68,  -155,  -239,    14 }
    };

    for (int row = 0; row < 12; ++row)
        for (int phase = 0; phase < 4; ++phase)
            if (publishedTable[row][phase] * 8192.0 != (double) numerators[row][phase])
                fail ("row " + std::to_string (row) + " phase " + std::to_string (phase)
                      + " is " + std::to_string (publishedTable[row][phase] * 8192.0)
                      + "/8192, the document has " + std::to_string (numerators[row][phase]));

    // Each phase is a fractional delay of the same signal, so each ought to
    // pass a constant through unchanged, and two of the four do not. These
    // are pinned exactly rather than to a band. The old check allowed 0.03 of
    // slack around one, and phases 1 and 2 sit 0.027 away from it: a tenth of
    // a decibel of drift in the wrong direction and it would have gone red on
    // coefficients that were perfectly correct.
    const double expectedSum[4] =
    {
        1.0015869140625, 0.9730224609375, 0.9730224609375, 1.0015869140625
    };

    for (int phase = 0; phase < 4; ++phase)
    {
        double sum = 0.0;
        for (int row = 0; row < 12; ++row)
            sum += publishedTable[row][phase];

        if (sum != expectedSum[phase])
            fail ("phase " + std::to_string (phase) + " sums to " + std::to_string (sum)
                  + ", the document's coefficients sum to "
                  + std::to_string (expectedSum[phase]));
    }

    // That deficit is the meter's largest single error and it belongs to the
    // standard, so it is stated rather than tolerated: 0.9730224609375 is
    // 0.238 dB down, on two phases of the four.
    std::printf ("    phase gains at DC: %+.4f %+.4f %+.4f %+.4f dB\n",
                 20.0 * std::log10 (expectedSum[0]), 20.0 * std::log10 (expectedSum[1]),
                 20.0 * std::log10 (expectedSum[2]), 20.0 * std::log10 (expectedSum[3]));

    // Interleaved back into one 48 tap filter it is symmetric, which is what
    // makes it linear phase, and the centre of that symmetry is why no phase
    // of this filter ever lands on an input sample. An even number of taps
    // puts the centre at 23.5 rather than on a tap, so the four phases sit an
    // eighth, three eighths, five eighths and seven eighths of the way
    // between one input sample and the next, and the filter never reproduces
    // a stored value. That is the whole reason this meter has to take the
    // sample's own magnitude into the maximum.
    double h[48];
    for (int row = 0; row < 12; ++row)
        for (int phase = 0; phase < 4; ++phase)
            h[row * 4 + phase] = publishedTable[row][phase];

    for (int i = 0; i < 24; ++i)
        if (h[i] != h[47 - i])
            fail ("the interleaved filter is not symmetric at tap " + std::to_string (i));

    check (h[23] == h[24] && h[23] == 0.97216796875,
           "the centre of symmetry is not between taps 23 and 24, so the phase "
           "grid is not where this file assumes");
}

/** The header against the document, value by value.

    This is the only check that pins the table, and nothing that goes through
    the filter's output can replace it. A digit mistyped in the smallest
    coefficient moves a reading by under a millionth, which is below the floor
    of any comparison of readings, and the header stores the table rearranged,
    so the rearrangement has to be checked along with the values. */
static void theHeaderHoldsThePublishedTable()
{
    std::printf ("the header's table is the document's, digit for digit\n");

    for (int row = 0; row < 12; ++row)
        for (int phase = 0; phase < 4; ++phase)
        {
            const double stored = (double) TruePeakDetector::published (row, phase);

            if (stored != publishedTable[row][phase])
                fail ("row " + std::to_string (row) + " phase " + std::to_string (phase)
                      + ": the header has " + std::to_string (stored)
                      + ", the document has " + std::to_string (publishedTable[row][phase]));
        }
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
    std::printf ("EBU Tech 3341 cases 15 to 19 of the true-peak cases 15 to 23\n");

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

        // The margin is printed because one of these is tight and it is
        // worth seeing. Case 17's crest falls half an input sample from a
        // sample, so the nearest grid points are 7.5 degrees away on the two
        // phases that are 0.238 dB light, and the two together account for
        // almost all of the standard's 0.4 dB allowance. There is no slack
        // there to spend on an implementation shortcut.
        std::printf ("    %-30s sample peak %+6.2f dBFS   true peak %+6.2f dBTP   "
                     "%+.3f of %+0.1f/%+0.1f   %s\n",
                     c.name, samplePeakDb (signal), got, got - c.expected, 0.2, -0.4,
                     ok ? "ok" : "OUT OF TOLERANCE");

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

/** The one bound this reading can be held to without any standard at all.

    Reconstruction passes through the stored samples, so the true peak is at
    least the sample peak. Not approximately, not usually: a lower bound. A
    meter that reads below it is reporting less than the file plainly
    contains, and `MixReportPanel` turns that number into "no headroom
    problem" above minus one and a clipping warning above zero.

    This is where the standard's own coefficients let the meter down, so the
    check is run first against a reading built from the interpolator alone,
    and has to catch it. If that reading ever stops violating the bound then
    something about the filter has changed and this check has stopped meaning
    anything. */
static void theReadingIsNeverBelowTheSamplePeak()
{
    std::printf ("the reading is never below the largest sample\n");

    // A single full scale sample with silence around it: a click, or the tip
    // of a tight transient. The interpolator alone reads the centre tap,
    // 0.97216796875, which is 0.245 dB below a sample the file contains.
    {
        std::vector<float> lone (64, 0.0f);
        lone[20] = 1.0f;

        const double interpolatorOnly = toDb (referenceReconstructionOnly (lone, lone.size()));
        const double detector         = measure (lone);

        check (interpolatorOnly < -0.2,
               "the interpolator alone no longer under-reads a lone full scale sample, so "
               "this check can no longer fail: it read " + std::to_string (interpolatorOnly));

        check (std::abs (interpolatorOnly - toDb (0.97216796875)) < 1e-6,
               "the interpolator alone read " + std::to_string (interpolatorOnly)
               + " dBTP on a lone full scale sample, which should be the centre tap");

        check (detector >= -1e-6,
               "a lone full scale sample read " + std::to_string (detector)
               + " dBTP, below the 0.0 dBFS the signal actually contains");

        std::printf ("    lone full scale sample: interpolator alone %+.4f dBTP, "
                     "detector %+.4f dBTP\n", interpolatorOnly, detector);
    }

    // The same fault on a steady tone, and with the file's own taper on it,
    // because without one the reading is the tone's switch-on step rather
    // than the tone: untapered this read -0.236 and the tone itself is at
    // -0.265.
    //
    // 16 kHz at 48 kHz is three samples a cycle, and starting at 90 degrees
    // puts a sample exactly on every crest. The nearest grid points to a
    // sample instant are an eighth of a sample either side, which at 16 kHz
    // is 15 degrees of the wave, so cos 15 degrees is -0.30 dB and the
    // passband gain of the phases that land there gives +0.02 of it back.
    // Those are phases 0 and 3, the two whose gain is slightly over one: the
    // 0.238 dB deficit on the other two plays no part here.
    {
        const auto tone = taperedTone (1.0 / 3.0, 1.0, 90.0, 4800);

        const double interpolatorOnly = toDb (referenceReconstructionOnly (tone, tone.size()));
        const double detector         = measure (tone);

        check (interpolatorOnly < -0.2,
               "the interpolator alone no longer under-reads a 16 kHz tone sampled on its "
               "crests: it read " + std::to_string (interpolatorOnly));

        check (detector >= samplePeakDb (tone) - 1e-6,
               "a 16 kHz tone sampled on its crests read " + std::to_string (detector)
               + " dBTP against a sample peak of " + std::to_string (samplePeakDb (tone)));

        std::printf ("    16 kHz on its crests:   interpolator alone %+.4f dBTP, "
                     "detector %+.4f dBTP\n", interpolatorOnly, detector);
    }

    // And then across the band, because the two cases above are chosen and
    // the awkward ones are the ones nobody would think to write down. Every
    // frequency against every start phase, tapered for the same reason as
    // above: untapered, the switch-on step put these readings up to 0.64 dB
    // above the true peak and the sweep could say nothing about the upward
    // direction at all.
    //
    // Both directions are asserted, and only one of them can fail. Below the
    // sample peak is now arithmetically impossible, since the reading is a
    // maximum that the sample peak is one of the terms of, so that half is
    // here to fail if the seed is ever removed or damaged rather than to
    // discover anything. Above the true peak is the half that is real, and it
    // is the direction EBU Tech 3341 is strict in.
    {
        double worstLow  = 0.0;
        double worstHigh = -200.0;
        double atFrequency = 0.0, atPhase = 0.0;

        for (int f = 1; f <= 120; ++f)
        {
            const double frequency = f * 200.0;

            for (int k = 0; k < 16; ++k)
            {
                const double startPhase = k * 360.0 / 16.0;
                const auto tone = taperedTone (frequency / 48000.0, 1.0, startPhase, 1600);

                const double reading = measure (tone);

                // A full scale sine reaches exactly full scale between its
                // samples whatever its phase, so its true peak is 0 dBTP and
                // the reading against it is the error.
                if (reading > worstHigh)
                {
                    worstHigh   = reading;
                    atFrequency = frequency;
                    atPhase     = startPhase;
                }

                worstLow = std::min (worstLow, reading - samplePeakDb (tone));
            }
        }

        check (worstLow > -1e-6,
               "over 1920 tones the reading sat " + std::to_string (-worstLow)
               + " dB below the sample peak");

        // 0.222 dB is the standard's own passband ripple and nothing here may
        // quietly exceed it. It is worth knowing that this is more than EBU
        // Tech 3341's +0.2 dBTP allows, and that no test case of theirs lands
        // near the frequency where it happens, so the suite passes and a
        // steady tone at 12.2 kHz still reads high. That is a property of the
        // coefficients the recommendation prints. The answer is not to
        // rescale them, because then they stop being the ones every other
        // meter uses.
        check (worstHigh < 0.25,
               "a full scale tone read " + std::to_string (worstHigh)
               + " dBTP, above the 0.222 dB the standard's own ripple accounts for, at "
               + std::to_string (atFrequency) + " Hz starting at "
               + std::to_string (atPhase) + " degrees");

        std::printf ("    1920 tones across the band: worst %.6f dB below the sample peak, "
                     "worst %+.4f dBTP against a true peak of 0, at %.0f Hz\n",
                     -worstLow, worstHigh, atFrequency);
    }
}

/** Where that ripple comes from, computed from the table rather than heard
    through it.

    The sweep above walks a 200 Hz grid and so lands near the worst frequency
    without landing on it. This takes the magnitude response of each phase
    straight from the document's coefficients, which pins the figure the
    header quotes to the Hertz and would notice any change to the table that
    the readings happened to step over. */
static void thePerPhaseRippleIsTheStandardsOwn()
{
    std::printf ("the passband ripple is the one the published coefficients have\n");

    double worst = -200.0;
    int    atFrequency = 0;
    int    onPhase = -1;

    for (int f = 0; f <= 20000; ++f)
    {
        const double omega = 2.0 * pi * f / 48000.0;

        for (int phase = 0; phase < 4; ++phase)
        {
            double re = 0.0, im = 0.0;

            for (int k = 0; k < 12; ++k)
            {
                re += publishedTable[k][phase] * std::cos (-omega * k);
                im += publishedTable[k][phase] * std::sin (-omega * k);
            }

            const double gain = toDb (std::sqrt (re * re + im * im));

            if (gain > worst)
            {
                worst       = gain;
                atFrequency = f;
                onPhase     = phase;
            }
        }
    }

    check (std::abs (worst - 0.2220) < 0.0001 && atFrequency == 12146 && onPhase == 2,
           "the largest per phase gain is " + std::to_string (worst) + " dB at "
           + std::to_string (atFrequency) + " Hz on phase " + std::to_string (onPhase)
           + ", not 0.2220 dB at 12146 Hz on phase 2");

    std::printf ("    largest per phase gain %+.4f dB at %d Hz, on phase %d\n",
                 worst, atFrequency, onPhase);
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
    theHeaderHoldsThePublishedTable();
    ebuComplianceCases();
    findsWhatASamplePeakMeterCannot();
    theReadingIsNeverBelowTheSamplePeak();
    thePerPhaseRippleIsTheStandardsOwn();
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
