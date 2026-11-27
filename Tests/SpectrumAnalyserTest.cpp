// ---------------------------------------------------------------------------
// Checks the spectrum analyser: the band geometry, the normalisation, the
// ballistics and the refusals.
//
// Every mistake available in SpectrumAnalyser.h produces a plausible looking
// spectrum. That is the whole reason this file is as long as it is. A band set
// built with the wrong formula has the right count, the right ratios and the
// right widths with every centre a semitone out. A missing normalisation
// constant is a fixed 1.76 dB on everything, so the shape is untouched. A
// double counted display tilt leans the picture the way a real mix already
// leans. None of them errors, none of them looks odd, and none of them can be
// caught by looking at music.
//
// So the properties below are the claims the header makes, asserted as claims
// about a signal or about the standard rather than by recomputing the header's
// own arithmetic:
//
//   * The band geometry is checked against IEC 61260-1:2014 from the outside:
//     the exact mid-band frequency is the geometric mean of the edges, the
//     edge ratio is G^(1/b), and the normalised bandwidth is the constant ANSI
//     S1.11-2004 clause 3.16 publishes. The odd and even formulas are pinned
//     by where 1 kHz lands, which is a centre at thirds and an EDGE at sixths,
//     and by the exact nesting of sixths inside thirds, which the wrong
//     formula also breaks. Base ten against base two is pinned at both ends of
//     the range, because the two agree at 1 kHz and a check written there says
//     nothing.
//   * The window constants are asserted as Harris's published figures, exactly
//     rather than nearly, at five transform sizes.
//   * The normalisation is checked by the two signals that disagree about it:
//     a full scale sine's band reads 0.0 dB, AND the band powers of a broadband
//     signal filling the display sum to that signal's own mean square. Only one
//     of the two candidate constants satisfies both, and the other one is 1.76
//     dB away, which is why both are here.
//   * The slope is measured over synthesised white, pink and brown noise and
//     fitted by least squares, the way Fx/Tilt.h measures its own response.
//     This is the one check that catches a double counted tilt or a spurious
//     bandwidth division, and the test deliberately applies the 4.5 dB per
//     octave that Voxengo and FabFilter document and shows that pink noise then
//     rises at the full 4.5, so the measurement is known to be able to fail.
//   * The decay is measured in dB per second and asserted against
//     IEC 61672-1 clause 5.7.2's acceptance windows rather than against the
//     header's own expression for it, and it is measured from two very
//     different starting levels, because a smoother in the wrong domain gives
//     a rate that depends on the level.
//   * The channel combination is checked on anti-correlated content, which is
//     where a time domain sum would silently delete the signal.
//   * The refusals are checked by feeding the thing being refused: a 20 Hz
//     sine at a transform size that cannot resolve a 20 Hz band must leave
//     that band blank rather than reading something, and one NaN sample must
//     not be able to kill the display for the rest of the session.
//
// Noise is synthesised in the frequency domain, by giving every bin inside a
// chosen range a known magnitude and a random phase and transforming back. That
// makes the spectrum of each frame exactly what the test intends rather than
// approximately, so a slope can be asserted to a hundredth of a decibel per
// octave instead of to the thirty per cent a handful of sine readings would
// give. Fx/Tilt.h learned that lesson the other way round, by failing a correct
// filter with a measurement that was too noisy to judge it.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include "SpectrumAnalyser.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace Spectrum;

static int failures = 0;

// Capped, because a property checked over every band at several sizes can fail
// hundreds of times and only the first few lines are ever read. The count is
// complete and is what the exit code comes from.
static void fail (const std::string& what)
{
    ++failures;

    if (failures <= 25)
        std::printf ("  FAIL  %s\n", what.c_str());
    else if (failures == 26)
        std::printf ("  ... further failures not printed\n");
}

static void check (bool ok, const std::string& what)
{
    if (! ok)
        fail (what);
}

static void checkNear (double actual, double expected, double tolerance,
                       const std::string& what)
{
    if (! (std::abs (actual - expected) <= tolerance))
        fail (what + " (expected " + std::to_string (expected) + ", got "
              + std::to_string (actual) + ")");
}

//==============================================================================
// A reproducible generator. The project's own splitmix64 rather than <random>,
// whose distributions are not specified exactly and give different sequences on
// MSVC, which would make a tolerance that passes here fail in CI on Windows.

struct Rng
{
    std::uint64_t state = 0x9e3779b97f4a7c15ull;

    std::uint64_t next() noexcept
    {
        state += 0x9e3779b97f4a7c15ull;
        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    double uniform() noexcept
    {
        return (double) (next() >> 11) * (1.0 / 9007199254740992.0);
    }
};

static constexpr double twoPi = 6.283185307179586476925286766559;

//==============================================================================

static void makeSine (std::vector<float>& out, int n, double sampleRate,
                      double frequency, double amplitude, double phase = 0.0)
{
    out.assign ((std::size_t) n, 0.0f);

    for (int i = 0; i < n; ++i)
        out[(std::size_t) i] = (float) (amplitude
            * std::sin (twoPi * frequency * (double) i / sampleRate + phase));
}

/** Noise with a power density proportional to `f^(-alpha)`, non-zero only
    between `lowHz` and `highHz`, built in the frequency domain so that the
    spectrum of the frame is exactly the one asked for.

    alpha of 0 is white, 1 is pink and 2 is brown. */
static void makeSlopedNoise (std::vector<float>& out, RealFFT& fft, int n,
                             double sampleRate, double alpha,
                             double lowHz, double highHz, Rng& rng)
{
    const int    numBins = n / 2 + 1;
    const double df      = sampleRate / (double) n;

    std::vector<std::complex<double>> bins ((std::size_t) numBins, { 0.0, 0.0 });

    for (int k = 1; k < numBins - 1; ++k)
    {
        const double f = (double) k * df;

        if (f < lowHz || f > highHz)
            continue;

        // Density proportional to f^(-alpha) means amplitude proportional to
        // f^(-alpha/2), one bin holding one bin width of it.
        const double magnitude = std::pow (f, -0.5 * alpha);
        const double phase     = twoPi * rng.uniform();

        bins[(std::size_t) k] = std::polar (magnitude, phase);
    }

    out.assign ((std::size_t) n, 0.0f);
    fft.inverse (bins.data(), out.data());
}

static double meanSquare (const std::vector<float>& x)
{
    double sum = 0.0;

    for (const auto v : x)
        sum += (double) v * (double) v;

    return x.empty() ? 0.0 : sum / (double) x.size();
}

/** A least squares line through the band levels against log2 of the centre
    frequency, which gives the display's slope in dB per octave. The same
    measurement Fx/Tilt.h makes of its own response, for the same reason: a
    slope is a property of the whole curve and reading two points cannot tell a
    slope from a kink. */
static double fitSlopeDbPerOctave (const Analyser& analyser,
                                   double fromHz, double toHz)
{
    double sumX = 0.0, sumY = 0.0, sumXX = 0.0, sumXY = 0.0;
    int    count = 0;

    for (int i = 0; i < analyser.getNumBands(); ++i)
    {
        if (! analyser.resolves (i))
            continue;

        const double centre = analyser.getBands()[(std::size_t) i].centre;

        if (centre < fromHz || centre > toHz)
            continue;

        const double x = std::log2 (centre);
        const double y = analyser.levelDb (i);

        sumX += x; sumY += y; sumXX += x * x; sumXY += x * y;
        ++count;
    }

    if (count < 3)
        return 0.0;

    const double nd        = (double) count;
    const double denominator = nd * sumXX - sumX * sumX;

    return denominator != 0.0 ? (nd * sumXY - sumX * sumY) / denominator : 0.0;
}

/** The band's power, undoing the display conventions, so that the test can add
    powers up and compare them against a mean square. */
static double bandPower (const Analyser& analyser, int band)
{
    const double db = analyser.levelDb (band);

    if (db <= floorDb)
        return 0.0;

    return std::pow (10.0, (db - fullScaleSineOffsetDb) / 10.0);
}

//==============================================================================
static void testBandGeometry()
{
    const double G = octaveRatio();

    // G itself, IEC 61260-1:2014 clause 5.2.2 gives it as 1.99526 to six
    // figures, and it is emphatically not 2.
    checkNear (G, 1.9952623149688795, 1.0e-15, "the octave ratio is 10^(3/10)");
    check (std::abs (G - 2.0) > 0.004, "the octave ratio is not base two");

    for (const int b : { 1, 2, 3, 6, 12 })
    {
        const double expectedEdgeRatio = std::pow (G, 1.0 / (2.0 * (double) b));
        const double expectedWidth     = expectedEdgeRatio - 1.0 / expectedEdgeRatio;

        checkNear (normalisedBandwidth (b), expectedWidth, 1.0e-15,
                   "the normalised bandwidth at 1/" + std::to_string (b) + " octave");

        for (int x = -24; x <= 24; ++x)
        {
            const double centre = midBandFrequency (x, b);
            const double lower  = lowerBandEdge (centre, b);
            const double upper  = upperBandEdge (centre, b);

            const std::string where = " at 1/" + std::to_string (b) + " octave band x = "
                                    + std::to_string (x);

            // IEC 61260-1:2014 clause 3.8: the exact mid-band frequency is the
            // geometric mean of the band edges. Asserted rather than taken
            // from the clause 5.6 formula, whose body is paywalled and was
            // read second hand.
            checkNear (std::sqrt (lower * upper) / centre, 1.0, 1.0e-14,
                       "the centre is the geometric mean of the edges" + where);

            // The whole band spans one bandwidth, so the edges are a factor
            // G^(1/b) apart.
            checkNear (upper / lower, std::pow (G, 1.0 / (double) b), 1.0e-13,
                       "the edges span G^(1/b)" + where);

            checkNear ((upper - lower) / centre, expectedWidth, 1.0e-13,
                       "the width is the published fraction of the centre" + where);
        }
    }

    // ---- the odd and even formulas, pinned by where 1 kHz lands ----
    //
    // b = 3 is odd, so IEC 61260-1:2014 clause 5.4.1 puts a band centre at the
    // reference frequency. b = 6 is even, so clause 5.4.2 cannot: (2x+1)/12 is
    // never zero, and 1 kHz is the edge shared by two bands.
    checkNear (midBandFrequency (0, 3), 1000.0, 1.0e-10,
               "1 kHz is a band centre at one third octave");

    bool sixthCentredOnReference = false, sixthEdgeOnReference = false;

    for (int x = -4; x <= 4; ++x)
    {
        const double centre = midBandFrequency (x, 6);

        if (std::abs (centre - 1000.0) < 1.0)
            sixthCentredOnReference = true;

        if (std::abs (upperBandEdge (centre, 6) - 1000.0) < 1.0e-9)
            sixthEdgeOnReference = true;
    }

    check (! sixthCentredOnReference,
           "no one sixth octave band is centred on 1 kHz, because b is even");
    check (sixthEdgeOnReference,
           "a one sixth octave band edge falls exactly on 1 kHz");

    // The two bands either side of it, which is the arithmetic the odd formula
    // gets wrong by G^(1/12), or 99.66 cents.
    checkNear (midBandFrequency (-1, 6), 944.0608762859236, 1.0e-9,
               "the one sixth octave band below the reference edge");
    checkNear (midBandFrequency (0, 6), 1059.2537251772889, 1.0e-9,
               "the one sixth octave band above the reference edge");

    // ---- sixths nest exactly in pairs inside thirds ----
    //
    // Sanctioned by ANSI S1.11-2004 clause 3.5 note 1, which allows narrow
    // fractional octave outputs to be combined into a wider band with an exact
    // mid-band frequency and exact edges. It holds only with the even formula,
    // so it is the second, independent check on the same thing.
    for (int x3 = -17; x3 <= 13; ++x3)
    {
        const double third      = midBandFrequency (x3, 3);
        const double thirdLower = lowerBandEdge (third, 3);
        const double thirdUpper = upperBandEdge (third, 3);

        const double lowerSixth = midBandFrequency (2 * x3 - 1, 6);
        const double upperSixth = midBandFrequency (2 * x3, 6);

        const std::string where = " for the third octave band x = " + std::to_string (x3);

        checkNear (lowerBandEdge (lowerSixth, 6) / thirdLower, 1.0, 1.0e-13,
                   "the lower sixth shares the third's lower edge" + where);
        checkNear (upperBandEdge (upperSixth, 6) / thirdUpper, 1.0, 1.0e-13,
                   "the upper sixth shares the third's upper edge" + where);

        // And the join between the two sixths is the third's own centre.
        checkNear (upperBandEdge (lowerSixth, 6) / third, 1.0, 1.0e-13,
                   "the two sixths meet at the third's centre" + where);
        checkNear (lowerBandEdge (upperSixth, 6) / third, 1.0, 1.0e-13,
                   "the two sixths meet at the third's centre, from above" + where);
    }

    // ---- base ten against base two, at both ends ----
    //
    // The two conventions are identical at 1 kHz and differ by -23.27 cents at
    // the bottom band and +17.79 cents at the top, so this is the check that
    // says which one is implemented. A check at 1 kHz passes under either.
    checkNear (midBandFrequency (-17, 3), 19.952623149688795, 1.0e-9,
               "the bottom third octave band is at the base ten centre");
    checkNear (midBandFrequency (13, 3), 19952.623149688795, 1.0e-6,
               "the top third octave band is at the base ten centre");

    check (std::abs (midBandFrequency (-17, 3) - 1000.0 * std::pow (2.0, -17.0 / 3.0)) > 0.25,
           "the bottom band is not where base two would put it");
    check (std::abs (midBandFrequency (13, 3) - 1000.0 * std::pow (2.0, 13.0 / 3.0)) > 200.0,
           "the top band is not where base two would put it");

    // ---- the nominal labels, from ISO 266:1997 Table 1 ----
    static const double iso266[31] = { 20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160,
                                       200, 250, 315, 400, 500, 630, 800, 1000, 1250,
                                       1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000,
                                       10000, 12500, 16000, 20000 };

    for (int i = 0; i < 31; ++i)
    {
        const int x = i - 17;

        checkNear (nominalThirdOctaveFrequency (x, 3), iso266[i],
                   1.0e-9 * iso266[i],
                   "the ISO 266 label for third octave band x = " + std::to_string (x));

        // And it is the nominal for that band rather than for its neighbour,
        // which is what a decade index off by one would give. Every preferred
        // frequency is within 1.5 per cent of its exact centre and the bands
        // are 26 per cent apart, so this is a wide margin on a sharp claim.
        const double centre = midBandFrequency (x, 3);
        check (std::abs (nominalThirdOctaveFrequency (x, 3) / centre - 1.0) < 0.015,
               "the ISO 266 label matches its own band x = " + std::to_string (x));
    }

    // No invented series for anything else: Annex E.3 of IEC 61260-1:2014
    // specifies those labels, it is normative, and it was not read.
    for (const int b : { 1, 2, 6, 12 })
        checkNear (nominalThirdOctaveFrequency (0, b), 0.0, 0.0,
                   "no nominal label is invented at 1/" + std::to_string (b) + " octave");
}

//==============================================================================
static void testWindowConstants()
{
    // Harris, NUC TP 532 Table I, reprinted as Proc. IEEE 66(1) 1978 Table I:
    // the Hann window has a coherent gain of 0.50 and an equivalent noise
    // bandwidth of 1.50 bins. Exactly, and at every size, which is the property
    // that says the periodic form is in use. The symmetric form gives 0.499878
    // and 1.500366 at 4096, so the tolerance here is far tighter than the
    // difference between the two.
    for (const int n : { 256, 2048, 4096, 8192, 16384 })
    {
        std::vector<double> window;
        buildHannWindow (window, n);

        check ((int) window.size() == n, "the window is the size asked for");

        checkNear (coherentGain (window), 0.5, 1.0e-12,
                   "Harris's coherent gain for the periodic Hann at " + std::to_string (n));
        checkNear (equivalentNoiseBandwidth (window), 1.5, 1.0e-12,
                   "Harris's noise bandwidth for the periodic Hann at " + std::to_string (n));

        // The window starts at zero and is symmetric about its middle sample,
        // which is what the periodic form means and what distinguishes it from
        // a window that merely looks like a raised cosine.
        checkNear (window[0], 0.0, 0.0, "the periodic Hann starts at zero");
        checkNear (window[(std::size_t) (n / 2)], 1.0, 1.0e-12,
                   "the periodic Hann peaks at one in the middle");
    }
}

//==============================================================================
static void testDisplayRange()
{
    // 48 kHz: Nyquist is 24 kHz, above the 20 kHz band's upper edge of
    // 22387.2 Hz, so the whole audio range fits.
    const auto thirds48 = bands (3, 48000.0);
    check ((int) thirds48.size() == 31, "31 third octave bands at 48 kHz");

    if (! thirds48.empty())
    {
        checkNear (nominalThirdOctaveFrequency (thirds48.front().x, 3), 20.0, 1.0e-9,
                   "the lowest band at 48 kHz is the 20 Hz one");
        checkNear (nominalThirdOctaveFrequency (thirds48.back().x, 3), 20000.0, 1.0e-6,
                   "the highest band at 48 kHz is the 20 kHz one");
    }

    // 44.1 kHz: Nyquist is 22050, below that upper edge, so the 20 kHz band is
    // left out rather than shown truncated and reading 0.74 dB low.
    const auto thirds441 = bands (3, 44100.0);
    check ((int) thirds441.size() == 30, "30 third octave bands at 44.1 kHz");

    if (! thirds441.empty())
        checkNear (nominalThirdOctaveFrequency (thirds441.back().x, 3), 16000.0, 1.0e-6,
                   "the highest band at 44.1 kHz is the 16 kHz one");

    for (const double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
        for (const int b : { 1, 3, 6, 12 })
        {
            const auto list = bands (b, rate);

            check (! list.empty(), "there is a display at " + std::to_string ((int) rate)
                                   + " Hz and 1/" + std::to_string (b) + " octave");

            for (std::size_t i = 0; i < list.size(); ++i)
            {
                check (list[i].upper <= 0.5 * rate * (1.0 + 1.0e-9),
                       "no band crosses Nyquist");
                check (list[i].lower >= audioRangeLowerEdge() * (1.0 - 1.0e-9),
                       "no band falls below the audio range");

                // Contiguous and ascending, with each band's upper edge the
                // next one's lower edge. A gap or an overlap would mean energy
                // counted twice or not at all.
                if (i > 0)
                    checkNear (list[i].lower / list[i - 1].upper, 1.0, 1.0e-12,
                               "the bands are contiguous");
            }
        }

    // Sixths cover exactly the same span as thirds, in twice as many bands,
    // which is the nesting property seen from the display's end.
    const auto sixths48 = bands (6, 48000.0);
    check ((int) sixths48.size() == 62, "62 sixth octave bands at 48 kHz");

    if (! sixths48.empty() && ! thirds48.empty())
    {
        checkNear (sixths48.front().lower / thirds48.front().lower, 1.0, 1.0e-12,
                   "sixths and thirds start at the same edge");
        checkNear (sixths48.back().upper / thirds48.back().upper, 1.0, 1.0e-12,
                   "sixths and thirds end at the same edge");
    }

    // Nonsense in, nothing out, rather than something plausible.
    check (bands (0, 48000.0).empty(), "no display at a bandwidth of zero");
    check (bands (-3, 48000.0).empty(), "no display at a negative bandwidth");
    check (bands (3, 0.0).empty(), "no display at a sample rate of zero");
    // Nyquist below the bottom band's lower edge of 17.78 Hz, so nothing fits.
    check (bands (3, 30.0).empty(), "no display at a rate that cannot carry a band");
    check (! bands (3, 100.0).empty(), "a rate that carries a few bands has them");
}

//==============================================================================
static void testBinWeights()
{
    for (const int n : { 2048, 8192, 16384 })
        for (const int b : { 3, 6 })
        {
            Analyser analyser;
            analyser.configure (n, 48000.0, b);

            check (analyser.isConfigured(), "the analyser configures at "
                                            + std::to_string (n));

            const double df = analyser.binWidth();

            // A band's total weight is exactly its width in bins. That is what
            // makes `resolves` and the weighting the same arithmetic rather
            // than two rules that could drift apart, and it is what a hard
            // include or exclude on the bin centre would break.
            for (int i = 0; i < analyser.getNumBands(); ++i)
            {
                const auto& band = analyser.getBands()[(std::size_t) i];

                checkNear (analyser.bandWeight (i), (band.upper - band.lower) / df,
                           1.0e-9 * std::max (1.0, (band.upper - band.lower) / df),
                           "the band's weight is its width in bins, band "
                               + std::to_string (i));

                check (analyser.resolves (i) == (analyser.bandWeight (i) >= 1.0),
                       "a band resolves exactly when it is a bin wide, band "
                           + std::to_string (i));
            }

            // Any one bin's weights across every band sum to one wherever the
            // bin lies wholly inside the display, so nothing is counted twice
            // and nothing is dropped at a boundary. Bins straddling the two
            // ends of the display carry less, which is correct, so they are
            // asked a weaker question.
            const double lowEdge  = analyser.getBands().front().lower;
            const double highEdge = analyser.getBands().back().upper;

            for (int k = 0; k <= n / 2; ++k)
            {
                const double binLower = ((double) k - 0.5) * df;
                const double binUpper = ((double) k + 0.5) * df;

                double total = 0.0;

                for (int i = 0; i < analyser.getNumBands(); ++i)
                {
                    const auto& band = analyser.getBands()[(std::size_t) i];
                    const double overlap = std::min (band.upper, binUpper)
                                         - std::max (band.lower, binLower);

                    if (overlap > 0.0)
                        total += overlap / df;
                }

                // A part in a billion rather than in a thousand billion: the
                // overlaps are differences of frequencies of order ten
                // thousand divided by a bin width of order one, so the
                // rounding of the edges themselves is amplified by that ratio.
                // Still some millions of times tighter than any error that
                // would matter.
                if (binLower >= lowEdge && binUpper <= highEdge)
                    checkNear (total, 1.0, 1.0e-9,
                               "bin " + std::to_string (k) + " is shared out exactly once");
                else
                    check (total <= 1.0 + 1.0e-9,
                           "bin " + std::to_string (k) + " is never counted more than once");
            }
        }
}

//==============================================================================
static void testSineReadsItsOwnLevel()
{
    // Voxengo documents SPAN's "Align 0 dB" as aligning a stationary 1 kHz sine
    // at 0 dBFS to 0 dB visually, "regardless of Block Size". That is the
    // property here, and the size independence is half of it: a normalisation
    // error that is a function of the transform length would pass at one size.
    //
    // The two candidate normalisations differ by 10*log10(1.5), which is
    // 1.7609 dB, so a tolerance of a twentieth of a decibel is nowhere near
    // either of the wrong answers.
    for (const int n : { 2048, 4096, 8192, 16384 })
    {
        Analyser analyser;
        analyser.configure (n, 48000.0, 3);

        const double df = analyser.binWidth();

        for (const double nominal : { 125.0, 1000.0, 3150.0, 10000.0 })
        {
            const int band = analyser.bandAt (nominal);

            if (band < 0 || ! analyser.resolves (band))
                continue;

            // Only asked of a band wide enough to hold the window's whole main
            // lobe. Narrower than that a tone reads low by a bounded amount,
            // which is the subject of its own check below rather than a
            // tolerance loosened until this one passes.
            if (analyser.bandWeight (band) < 24.0)
                continue;

            // Exactly on a bin, and then half a bin off it, which is the worst
            // case for a per bin reading: Harris's Table I gives the Hann
            // window 1.42 dB of scallop loss there. A band sum is a power
            // integral over many bins, so it collects the main lobe wherever
            // the tone sits between bins, and the reading must not move. That
            // immunity is a property of summing and is worth pinning, because
            // a reading that moved with the note would be read as the mix
            // changing.
            for (const double offset : { 0.0, 0.5 })
            {
                const double k = std::floor (nominal / df) + offset;
                const double frequency = k * df;

                std::vector<float> sine;
                makeSine (sine, n, 48000.0, frequency, 1.0);

                analyser.clear();
                check (analyser.push (sine.data(), nullptr, n, 1.0 / 30.0),
                       "a frame of sine is accepted");

                checkNear (analyser.levelDb (band), 0.0, 0.05,
                           "a full scale sine reads 0 dB at " + std::to_string ((int) nominal)
                               + " Hz, size " + std::to_string (n)
                               + ", bin offset " + std::to_string (offset));
            }
        }
    }

    // Half amplitude is 6.02 dB down, which says the scale is a power scale
    // rather than something that happens to pass through zero.
    Analyser analyser;
    analyser.configure (8192, 48000.0, 3);

    const int band = analyser.bandAt (1000.0);
    const double frequency = std::floor (1000.0 / analyser.binWidth()) * analyser.binWidth();

    for (const double amplitude : { 1.0, 0.5, 0.25, 0.01 })
    {
        std::vector<float> sine;
        makeSine (sine, 8192, 48000.0, frequency, amplitude);

        analyser.clear();
        analyser.push (sine.data(), nullptr, 8192, 1.0 / 30.0);

        checkNear (analyser.levelDb (band), 20.0 * std::log10 (amplitude), 0.05,
                   "a sine at amplitude " + std::to_string (amplitude) + " reads its own level");
    }
}

//==============================================================================
static void testNarrowBandsBlurRatherThanLose()
{
    // The bound the header publishes, and the reason the refusal threshold is
    // one bin rather than four.
    //
    // A band only a couple of bins wide cannot hold the whole main lobe of an
    // isolated tone, so it reads low. The claim is that the shortfall is
    // bounded at three decibels, that it falls as the band widens, and above
    // all that it is a blur across the join rather than a loss: the energy
    // appears in the neighbouring band instead, and the total over the whole
    // display is still the tone's own power.
    //
    // That last part is what makes the threshold defensible. A bass note is
    // never missing from the display, only shared between two bars.
    const int    n    = 16384;
    const double rate = 48000.0;

    Analyser analyser;
    analyser.configure (n, rate, 3);

    double worstNarrow = 0.0;

    for (int band = 0; band < analyser.getNumBands(); ++band)
    {
        if (! analyser.resolves (band))
            continue;

        const auto& b = analyser.getBands()[(std::size_t) band];

        // Across the band, including very close to both joins, which is where
        // the worst case lives.
        for (const double fraction : { 0.02, 0.25, 0.5, 0.75, 0.98 })
        {
            const double frequency = b.lower + fraction * (b.upper - b.lower);

            std::vector<float> sine;
            makeSine (sine, n, rate, frequency, 1.0);

            analyser.clear();
            analyser.push (sine.data(), nullptr, n, 1.0 / 30.0);

            const std::string where = " at " + std::to_string ((int) frequency)
                                    + " Hz in a band " + std::to_string (analyser.bandWeight (band))
                                    + " bins wide";

            // Never above its own level, which would be energy invented
            // rather than mislaid.
            check (analyser.levelDb (band) <= 0.05, "a tone never reads above full scale" + where);

            // The published bound, which is the join case: half the power
            // either side of the line. Asserted for every resolved band
            // rather than for the narrow ones only, because it is the width
            // independent half of the behaviour and a change that broke a wide
            // band would show up here.
            check (analyser.levelDb (band) >= -3.4,
                   "a tone in its own band reads within 3.4 dB of its level" + where);

            worstNarrow = std::min (worstNarrow, analyser.levelDb (band));

            // And the conservation claim: summed over the whole display, the
            // tone's power is all still there. The two bands either side of a
            // join hold it between them.
            //
            // Not asked of the bottom and top bands, where part of the lobe
            // genuinely falls off the end of the display rather than into a
            // neighbour.
            if (band > 0 && band + 1 < analyser.getNumBands())
            {
                double summed = 0.0;

                for (int i = 0; i < analyser.getNumBands(); ++i)
                    if (analyser.resolves (i))
                        summed += bandPower (analyser, i);

                checkNear (10.0 * std::log10 (summed) + fullScaleSineOffsetDb, 0.0, 0.02,
                           "the tone's power is all still on the display" + where);
            }
        }
    }

    // And the bound is tight rather than generous, which is what says the
    // measurement is of the real thing rather than of a tolerance picked to
    // pass: somewhere in the display a tone really does read about three
    // decibels down, so a check at 3.4 dB is not passing by a mile.
    check (worstNarrow < -2.5,
           "the worst case really is near the published bound (measured "
               + std::to_string (worstNarrow) + " dB)");

    // ---- the width dependent half, measured at the band's own centre ----
    //
    // This is the effect a larger transform fixes, and it is a different claim
    // from the one above: a band of about the window's main lobe width cannot
    // hold a centred tone, and from four bins up it can. The table in the
    // header is this measurement.
    auto errorAtCentre = [&] (int band)
    {
        const auto& b = analyser.getBands()[(std::size_t) band];

        std::vector<float> sine;
        makeSine (sine, n, rate, b.centre, 1.0);

        analyser.clear();
        analyser.push (sine.data(), nullptr, n, 1.0 / 30.0);

        return analyser.levelDb (band);
    };

    int narrowest = -1;

    for (int i = 0; i < analyser.getNumBands(); ++i)
    {
        if (! analyser.resolves (i))
            continue;

        if (narrowest < 0)
            narrowest = i;

        const double width = analyser.bandWeight (i);
        const double error = errorAtCentre (i);

        const std::string where = " in a band " + std::to_string (width) + " bins wide";

        // From four bins up a centred tone is exact, which is the claim the
        // header's table ends on.
        if (width >= 4.0)
            checkNear (error, 0.0, 0.05, "a centred tone is exact" + where);

        // And below that it is low, but by at most the couple of decibels the
        // table publishes, rather than by an unbounded amount.
        check (error >= -2.5, "a centred tone in a narrow band is only slightly low" + where);
        check (error <= 0.05, "a centred tone never reads above its level" + where);
    }

    // The narrowest band the display shows is measurably worse at its centre
    // than a wide one, which is what makes the table a measurement rather than
    // a guess, and what would catch a weighting that had stopped depending on
    // the band's width at all.
    check (narrowest >= 0, "there is a narrowest resolved band");

    if (narrowest >= 0)
    {
        check (analyser.bandWeight (narrowest) < 2.0,
               "the narrowest resolved band really is under two bins wide");
        check (errorAtCentre (narrowest) < -0.5,
               "the narrowest band reads a centred tone measurably low (measured "
                   + std::to_string (errorAtCentre (narrowest)) + " dB)");
    }
}

//==============================================================================
static void testBandPowersSumToTheSignal()
{
    // The other half of the normalisation check, and the half the sine cannot
    // give: the band powers of a broadband signal filling the display must add
    // up to that signal's own mean square.
    //
    // Only one of the two candidate constants satisfies this AND the sine
    // check above. The window sum normalisation passes the sine when it is read
    // off one bin and then reads 1.5 times the power here; this one passes
    // both. That is what makes the pair of tests evidence rather than a
    // restatement of the code.
    const int    n    = 16384;
    const double rate = 48000.0;

    RealFFT  fft (n);
    Analyser analyser;
    analyser.configure (n, rate, 3);

    Rng rng { 0x5eed1cedull };

    double totalRatio = 0.0;
    int    frames      = 0;

    for (int frame = 0; frame < 12; ++frame)
    {
        std::vector<float> noise;
        makeSlopedNoise (noise, fft, n, rate, 0.0,
                         analyser.getBands().front().lower,
                         analyser.getBands().back().upper, rng);

        const double ms = meanSquare (noise);

        if (! (ms > 0.0))
            continue;

        analyser.clear();
        analyser.push (noise.data(), nullptr, n, 1.0 / 30.0);

        double summed = 0.0;

        for (int i = 0; i < analyser.getNumBands(); ++i)
            if (analyser.resolves (i))
                summed += bandPower (analyser, i);

        totalRatio += summed / ms;
        ++frames;
    }

    check (frames > 0, "the broadband frames were usable");

    if (frames > 0)
        checkNear (totalRatio / (double) frames, 1.0, 0.01,
                   "the band powers sum to the signal's own mean square");
}

//==============================================================================
static void testSlopes()
{
    // The one check that catches a double counted tilt or a spurious bandwidth
    // division. Both leave a spectrum that looks entirely reasonable on music,
    // and both move this number by three decibels an octave or more.
    const int    n    = 16384;
    const double rate = 48000.0;

    RealFFT fft (n);

    struct Case { double alpha; double expected; const char* name; };

    const Case cases[3] = { { 0.0, 3.0102999566398120, "white" },
                            { 1.0, 0.0,                "pink"  },
                            { 2.0, -3.0102999566398120, "brown" } };

    for (const auto& c : cases)
    {
        Analyser analyser;
        analyser.configure (n, rate, 3);
        analyser.setBallistics (Ballistics::slow);

        Rng rng { 0xabcdef0123ull };

        // Averaged over frames by the ballistics themselves, which is also a
        // check that the smoother converges on the mean rather than drifting.
        for (int frame = 0; frame < 40; ++frame)
        {
            std::vector<float> noise;
            makeSlopedNoise (noise, fft, n, rate, c.alpha, 15.0, 23000.0, rng);

            analyser.push (noise.data(), nullptr, n, 1.0 / 30.0);
        }

        const double slope = fitSlopeDbPerOctave (analyser, 100.0, 10000.0);

        checkNear (slope, c.expected, 0.05,
                   std::string (c.name) + " noise reads the right slope in bands");

        // ---- and the measurement is shown to be able to fail ----
        //
        // The tilt Voxengo SPAN and FabFilter Pro-Q both document as their
        // default is 4.5 dB per octave. It is right for their per bin displays
        // and here it is a second application of a correction the band summing
        // has already made. Applied, pink noise rises at the full 4.5, which is
        // 45 dB across the ten octaves of this display. So the default is zero,
        // and this is the check that says so rather than a comment claiming it.
        analyser.setTiltDbPerOctave (documentedPerBinTiltDbPerOctave);

        checkNear (fitSlopeDbPerOctave (analyser, 100.0, 10000.0),
                   c.expected + documentedPerBinTiltDbPerOctave, 0.05,
                   std::string ("the documented per bin tilt double counts on ")
                       + c.name + " noise");

        // The honest translation of that convention: what is left of the 4.5
        // once the band summing has done the pink flattening part.
        analyser.setTiltDbPerOctave (preferenceTiltDbPerOctave);

        checkNear (fitSlopeDbPerOctave (analyser, 100.0, 10000.0),
                   c.expected + 1.4897000433601880, 0.05,
                   std::string ("the residual preference tilt on ") + c.name + " noise");

        analyser.setTiltDbPerOctave (0.0);

        checkNear (fitSlopeDbPerOctave (analyser, 100.0, 10000.0), c.expected, 0.05,
                   std::string ("the tilt returns to pink flat on ") + c.name + " noise");
    }

    // The tilt is exactly a line in log frequency through the pivot, asserted
    // per band rather than through the fit, so that a tilt which happened to
    // give the right overall slope with the wrong pivot still fails.
    Analyser analyser;
    analyser.configure (n, rate, 6);

    Rng rng { 0x13579bdfull };
    std::vector<float> noise;
    makeSlopedNoise (noise, fft, n, rate, 1.0, 15.0, 23000.0, rng);
    analyser.push (noise.data(), nullptr, n, 1.0 / 30.0);

    std::vector<double> flat ((std::size_t) analyser.getNumBands(), 0.0);

    for (int i = 0; i < analyser.getNumBands(); ++i)
        flat[(std::size_t) i] = analyser.levelDb (i);

    for (const double t : { -6.0, -1.5, 1.4897000433601880, 3.0, 4.5 })
    {
        analyser.setTiltDbPerOctave (t);

        for (int i = 0; i < analyser.getNumBands(); ++i)
        {
            if (! analyser.resolves (i))
                continue;

            const double centre = analyser.getBands()[(std::size_t) i].centre;
            const double expected = flat[(std::size_t) i]
                                  + t * std::log2 (centre / tiltPivotFrequency);

            checkNear (analyser.levelDb (i), expected, 1.0e-9,
                       "the tilt is a line through 1 kHz, band " + std::to_string (i));
        }
    }
}

//==============================================================================
static void testBallistics()
{
    // IEC 61672-1 clause 5.7.2, measured after a steady tone stops: the level
    // must decay at no less than 25 dB/s for time weighting F, and at between
    // 3.4 and 5.3 dB/s for S. Those windows are the specification, so they are
    // what this asserts, rather than the ideal 10/ln(10)/tau that the header
    // also carries.
    //
    // The rate is also what tells the right smoothing domain from the wrong
    // one. IEC 61672-1 clause 3.4 puts the exponential weighting on the SQUARE
    // of the signal. Smoothing the magnitude instead doubles the rate, to 69.5
    // dB/s for F, which clause 5.7.2's lower bound cannot catch and this check
    // on the ideal figure can.
    const int    n    = 4096;
    const double rate = 48000.0;
    const double step = 1.0 / 200.0;

    struct Case { Ballistics ballistics; double tau; double low; double high; const char* name; };

    const Case cases[2] = { { Ballistics::fast, fastTimeConstantSeconds, 25.0, 1.0e9, "F" },
                            { Ballistics::slow, slowTimeConstantSeconds, 3.4, 5.3, "S" } };

    for (const auto& c : cases)
        for (const double amplitude : { 1.0, 0.01 })
        {
            Analyser analyser;
            analyser.configure (n, rate, 3);
            analyser.setBallistics (c.ballistics);

            check (analyser.getBallistics() == c.ballistics, "the ballistics are as set");

            const int    band      = analyser.bandAt (1000.0);
            const double frequency = std::floor (1000.0 / analyser.binWidth())
                                   * analyser.binWidth();

            std::vector<float> sine, silence ((std::size_t) n, 0.0f);
            makeSine (sine, n, rate, frequency, amplitude);

            // Settled first, so the decay starts from the steady reading
            // rather than from part way up.
            for (int i = 0; i < 1200; ++i)
                analyser.push (sine.data(), nullptr, n, step);

            checkNear (analyser.levelDb (band), 20.0 * std::log10 (amplitude), 0.05,
                       std::string ("the steady reading before the decay, ") + c.name);

            // Measured across the middle of the decay, clear of both the
            // steady level and the floor.
            const double firstSeconds = 0.4 * c.tau * 10.0;
            const double spanSeconds  = 0.8 * c.tau * 10.0;

            for (double t = 0.0; t < firstSeconds; t += step)
                analyser.push (silence.data(), nullptr, n, step);

            const double startDb = analyser.levelDb (band);

            double elapsed = 0.0;

            for (double t = 0.0; t < spanSeconds; t += step)
            {
                analyser.push (silence.data(), nullptr, n, step);
                elapsed += step;
            }

            const double endDb = analyser.levelDb (band);
            const double ratePerSecond = (startDb - endDb) / elapsed;

            check (ratePerSecond >= c.low && ratePerSecond <= c.high,
                   std::string ("the decay is inside IEC 61672-1 clause 5.7.2's window for ")
                       + c.name + " from " + std::to_string (amplitude)
                       + " (measured " + std::to_string (ratePerSecond) + " dB/s)");

            // And it is the ideal rate the time constant implies, to a per
            // cent, which is the half of the check that catches a smoother in
            // the magnitude or the decibel domain: the first doubles this and
            // the second makes it depend on how far the reading is from its
            // target.
            checkNear (ratePerSecond, idealDecayDbPerSecond (c.tau),
                       0.01 * idealDecayDbPerSecond (c.tau),
                       std::string ("the decay matches the time constant for ") + c.name
                           + " from " + std::to_string (amplitude));
        }

    // The first frame seeds rather than ramps, so a display is right as soon as
    // it appears. A smoother started from silence would read a time constant's
    // worth of climb, which on the slow weighting is a second of a mix looking
    // quieter than it is.
    for (const auto ballistics : { Ballistics::fast, Ballistics::slow })
    {
        Analyser analyser;
        analyser.configure (n, rate, 3);
        analyser.setBallistics (ballistics);

        const int    band      = analyser.bandAt (1000.0);
        const double frequency = std::floor (1000.0 / analyser.binWidth())
                               * analyser.binWidth();

        std::vector<float> sine;
        makeSine (sine, n, rate, frequency, 1.0);

        analyser.push (sine.data(), nullptr, n, step);

        checkNear (analyser.levelDb (band), 0.0, 0.05,
                   "the first frame reads its own level rather than ramping up");
    }
}

//==============================================================================
static void testChannelCombination()
{
    // Four defensible ways to combine two channels span 6 dB, and two of them
    // delete anti-correlated content. This is the sum of the channel POWERS,
    // which reads 3.01 dB above one channel on correlated material and reads
    // the same 3.01 dB on anti-correlated material rather than nothing.
    //
    // The second half is the one that matters. Logic's MultiMeter offers a
    // "Mono" that is the time domain sum, where an out of phase bass guitar
    // vanishes from the display, which sends somebody looking at the bass. It
    // is the same shape of fault as the one in InputSource.h, where a mono
    // instrument came out of one speaker: the symptom points away from the
    // cause.
    const int    n    = 8192;
    const double rate = 48000.0;

    Analyser analyser;
    analyser.configure (n, rate, 3);

    const int    band      = analyser.bandAt (1000.0);
    const double frequency = std::floor (1000.0 / analyser.binWidth()) * analyser.binWidth();

    std::vector<float> sine, inverted, silence ((std::size_t) n, 0.0f);
    makeSine (sine, n, rate, frequency, 1.0);

    inverted.assign ((std::size_t) n, 0.0f);

    for (int i = 0; i < n; ++i)
        inverted[(std::size_t) i] = -sine[(std::size_t) i];

    auto read = [&] (const float* left, const float* right)
    {
        analyser.clear();
        analyser.push (left, right, n, 1.0 / 30.0);
        return analyser.levelDb (band);
    };

    const double oneChannel = read (sine.data(), nullptr);
    checkNear (oneChannel, 0.0, 0.05, "one channel of full scale sine reads 0 dB");

    checkNear (read (sine.data(), sine.data()), oneChannel + fullScaleSineOffsetDb, 0.02,
               "two correlated channels read 3.01 dB above one");

    checkNear (read (sine.data(), inverted.data()), oneChannel + fullScaleSineOffsetDb, 0.02,
               "two anti-correlated channels read 3.01 dB above one, and do not cancel");

    checkNear (read (sine.data(), silence.data()), oneChannel, 0.02,
               "a silent second channel adds nothing");

    // A quarter turn of phase, which a mid or a time domain sum would also read
    // differently, and which the power sum reads the same as any other pair.
    std::vector<float> quarter;
    makeSine (quarter, n, rate, frequency, 1.0, 1.5707963267948966);

    checkNear (read (sine.data(), quarter.data()), oneChannel + fullScaleSineOffsetDb, 0.02,
               "two channels in quadrature read 3.01 dB above one");
}

//==============================================================================
static void testRefusals()
{
    const double rate = 48000.0;

    // ---- a band narrower than a bin is refused, not approximated ----
    //
    // Fed the very thing it cannot measure: a loud 20 Hz sine at a transform
    // size whose bins are 23.4 Hz wide, against a 20 Hz band that is 4.6 Hz
    // wide. Reading "low" would pass a weaker check; the requirement is that
    // it reads nothing at all.
    {
        Analyser analyser;
        analyser.configure (2048, rate, 3);

        const double df = analyser.binWidth();
        int refused = 0;

        for (int i = 0; i < analyser.getNumBands(); ++i)
        {
            const auto& band = analyser.getBands()[(std::size_t) i];
            const bool narrow = (band.upper - band.lower) < df;

            check (analyser.resolves (i) == ! narrow,
                   "a band narrower than a bin does not resolve, band " + std::to_string (i));

            if (narrow)
                ++refused;
        }

        check (refused >= 4, "the bottom bands are refused at a transform size of 2048 "
                             "(refused " + std::to_string (refused) + ")");

        std::vector<float> sine;
        makeSine (sine, 2048, rate, 19.95262, 1.0);
        analyser.push (sine.data(), nullptr, 2048, 1.0 / 30.0);

        const int band = analyser.bandAt (19.95262);
        check (band >= 0, "the 20 Hz band is in the display");

        if (band >= 0)
        {
            check (! analyser.resolves (band), "the 20 Hz band does not resolve at 2048");
            checkNear (analyser.levelDb (band), noReadingDb, 0.0,
                       "a loud tone in an unresolvable band still reads nothing");
            checkNear (analyser.holdDb (band), noReadingDb, 0.0,
                       "an unresolvable band holds nothing either");
        }
    }

    // And at a size that can resolve it, the same band reads the tone.
    {
        Analyser analyser;
        analyser.configure (16384, rate, 3);

        const int band = analyser.bandAt (19.95262);
        check (band >= 0 && analyser.resolves (band),
               "the 20 Hz band resolves at a transform size of 16384");

        if (band >= 0 && analyser.resolves (band))
        {
            checkNear (analyser.bandWeight (band), 1.5714, 0.01,
                       "the 20 Hz band is 1.57 bins wide at 16384 and 48 kHz");

            // Not asserted to 0 dB: a band a bin and a half wide cannot hold a
            // Hann window's whole main lobe, so a tone in it reads low, and by
            // how much depends on where it sits. The claim is only that it
            // reads the tone rather than nothing, which is what resolving
            // means.
            std::vector<float> sine;
            makeSine (sine, 16384, rate, 19.95262, 1.0);
            analyser.push (sine.data(), nullptr, 16384, 1.0 / 30.0);

            check (analyser.levelDb (band) > -12.0,
                   "a 20 Hz tone reads in the 20 Hz band at 16384");
        }
    }

    // ---- one bad sample cannot kill the display ----
    //
    // A NaN would make every band NaN, and because the smoother feeds on itself
    // the display would stay NaN for the rest of the session. One bad block
    // from a misbehaving plugin would be permanent, with nothing to say why.
    {
        Analyser analyser;
        analyser.configure (4096, rate, 3);

        const int    band      = analyser.bandAt (1000.0);
        const double frequency = std::floor (1000.0 / analyser.binWidth())
                               * analyser.binWidth();

        std::vector<float> sine;
        makeSine (sine, 4096, rate, frequency, 1.0);

        analyser.push (sine.data(), nullptr, 4096, 1.0 / 30.0);
        const double before = analyser.levelDb (band);
        checkNear (before, 0.0, 0.05, "the good frame read its own level");

        for (const float bad : { std::numeric_limits<float>::quiet_NaN(),
                                 std::numeric_limits<float>::infinity(),
                                 -std::numeric_limits<float>::infinity() })
        {
            auto poisoned = sine;
            poisoned[1234] = bad;

            check (! analyser.push (poisoned.data(), nullptr, 4096, 1.0 / 30.0),
                   "a frame with a non-finite sample is refused");
            checkNear (analyser.levelDb (band), before, 0.0,
                       "a refused frame changes nothing");

            // And on the other channel, which is a separate path through
            // `push` and so a separate chance to let one through.
            check (! analyser.push (sine.data(), poisoned.data(), 4096, 1.0 / 30.0),
                   "a frame with a non-finite sample on the right is refused");
        }

        check (analyser.push (sine.data(), nullptr, 4096, 1.0 / 30.0),
               "the analyser still works after a refused frame");
        checkNear (analyser.levelDb (band), before, 0.05,
                   "and reads the same level it did before");

        // The frame interval has to be a usable number, since it is the
        // caller's and not the audio clock's.
        check (! analyser.push (sine.data(), nullptr, 4096, 0.0),
               "a frame interval of zero is refused");
        check (! analyser.push (sine.data(), nullptr, 4096, -0.1),
               "a negative frame interval is refused");
        check (! analyser.push (sine.data(), nullptr, 4096,
                                std::numeric_limits<double>::quiet_NaN()),
               "a non-finite frame interval is refused");

        // A partial window is refused rather than zero padded, which would read
        // as a quieter mix for as long as it lasted.
        check (! analyser.push (sine.data(), nullptr, 2048, 1.0 / 30.0),
               "a short frame is refused");
        check (! analyser.push (sine.data(), nullptr, 0, 1.0 / 30.0),
               "an empty frame is refused");
        check (! analyser.push (nullptr, nullptr, 4096, 1.0 / 30.0),
               "a frame with no left channel is refused");

        // A tilt that is not a number would otherwise reach every band.
        analyser.setTiltDbPerOctave (std::numeric_limits<double>::quiet_NaN());
        checkNear (analyser.getTiltDbPerOctave(), 0.0, 0.0,
                   "a non-finite tilt falls back to none");
    }

    // ---- an unusable configuration leaves nothing usable ----
    for (const int size : { 0, 1, 32, 1000, 3, -4096 })
    {
        Analyser analyser;
        analyser.configure (size, rate, 3);

        check (! analyser.isConfigured(),
               "a transform size of " + std::to_string (size) + " is refused");
        check (analyser.getNumBands() == 0, "a refused configuration has no bands");
        checkNear (analyser.levelDb (0), noReadingDb, 0.0,
                   "a refused configuration reads nothing");
        check (! analyser.resolves (0), "a refused configuration resolves nothing");

        std::vector<float> frame (4096, 0.0f);
        check (! analyser.push (frame.data(), nullptr, 4096, 1.0 / 30.0),
               "a refused configuration accepts no frames");
    }

    for (const double badRate : { 0.0, -48000.0 })
    {
        Analyser analyser;
        analyser.configure (4096, badRate, 3);
        check (! analyser.isConfigured(), "a sample rate of "
                                          + std::to_string (badRate) + " is refused");
    }

    for (const int badBandwidth : { 0, -1 })
    {
        Analyser analyser;
        analyser.configure (4096, rate, badBandwidth);
        check (! analyser.isConfigured(), "a bandwidth of "
                                          + std::to_string (badBandwidth) + " is refused");
    }

    // Reconfiguring forgets everything, so a sample rate change cannot leave a
    // band reading a level that was measured against different bins.
    {
        Analyser analyser;
        analyser.configure (4096, rate, 3);

        std::vector<float> sine;
        makeSine (sine, 4096, rate, 1000.0, 1.0);
        analyser.push (sine.data(), nullptr, 4096, 1.0 / 30.0);

        check (analyser.getFrameCount() == 1, "the frame count counts frames");

        analyser.configure (4096, 44100.0, 3);

        check (analyser.getFrameCount() == 0, "reconfiguring forgets the frames");
        checkNear (analyser.getSampleRate(), 44100.0, 0.0, "and takes the new rate");

        for (int i = 0; i < analyser.getNumBands(); ++i)
            if (analyser.resolves (i))
                checkNear (analyser.levelDb (i), floorDb, 0.0,
                           "reconfiguring forgets the readings");
    }
}

//==============================================================================
static void testHold()
{
    const int    n    = 4096;
    const double rate = 48000.0;

    Analyser analyser;
    analyser.configure (n, rate, 3);
    analyser.setBallistics (Ballistics::fast);

    const int    band      = analyser.bandAt (1000.0);
    const double frequency = std::floor (1000.0 / analyser.binWidth()) * analyser.binWidth();

    std::vector<float> loud, quiet;
    makeSine (loud,  n, rate, frequency, 1.0);
    makeSine (quiet, n, rate, frequency, 0.01);

    analyser.push (loud.data(), nullptr, n, 1.0 / 200.0);

    const double peak = analyser.levelDb (band);

    for (int i = 0; i < 2000; ++i)
        analyser.push (quiet.data(), nullptr, n, 1.0 / 200.0);

    checkNear (analyser.levelDb (band), -40.0, 0.1, "the level followed the signal down");
    checkNear (analyser.holdDb (band), peak, 0.01, "the hold kept the peak");

    for (int i = 0; i < analyser.getNumBands(); ++i)
        if (analyser.resolves (i))
            check (analyser.holdDb (i) >= analyser.levelDb (i) - 1.0e-12,
                   "the hold is never below the level, band " + std::to_string (i));

    analyser.resetHold();

    // Reset drops it to the floor rather than to the current level, which is
    // what "start measuring again from here" means and is what every analyser
    // read does on a click.
    checkNear (analyser.holdDb (band), floorDb, 0.0, "resetting the hold clears it");

    analyser.push (quiet.data(), nullptr, n, 1.0 / 200.0);
    checkNear (analyser.holdDb (band), analyser.levelDb (band), 0.01,
               "the hold rebuilds from the next frame");

    // The tilt reaches the hold as well as the level, so the two cannot be read
    // against different scales on the same display.
    analyser.setTiltDbPerOctave (3.0);

    for (int i = 0; i < analyser.getNumBands(); ++i)
    {
        if (! analyser.resolves (i))
            continue;

        const double centre = analyser.getBands()[(std::size_t) i].centre;

        if (analyser.holdDb (i) > floorDb + 1.0)
            checkNear (analyser.holdDb (i) - analyser.levelDb (i), 0.0, 0.01,
                       "the tilt moves the hold with the level, band " + std::to_string (i)
                           + " at " + std::to_string ((int) centre) + " Hz");
    }
}

//==============================================================================
static void testRingCapacity()
{
    // `Tuner::InputRing`'s stated requirement is `capacity >= snapshot +
    // 2 * block`, and the snapshot the display asks for is the largest
    // transform size. Checked here rather than taken on trust, because the
    // tuner's own version of these two numbers met in `AudioEngine.cpp`, where
    // nothing can compile them together, and an audit pointed out that halving
    // the capacity to a figure that still looked generous would have started
    // accepting torn snapshots with nothing to fail.
    //
    // A torn frame is a discontinuity in the middle of the window, which a
    // transform reads as a broadband click: a spectrum that is flat and
    // plausible and describes nothing that was played.
    for (const int block : { 64, 128, 256, 512, 1024, 2048, 4096, 8192 })
    {
        const int capacity = ringCapacity (block);

        check (capacity >= largestTransformSize + 2 * block,
               "the ring clears InputRing's requirement at a block of "
                   + std::to_string (block));

        // And by a margin, since the requirement is the point at which a
        // snapshot can be served at all rather than the point at which it is
        // comfortable.
        check (capacity >= largestTransformSize + 3 * block,
               "the ring clears the requirement with room to spare at a block of "
                   + std::to_string (block));
    }

    // Every size the display offers can be served, not only the largest.
    for (const int size : transformSizes)
    {
        check (size <= largestTransformSize,
               "every offered transform size fits the ring");
        check (size >= 64 && (size & (size - 1)) == 0,
               "every offered transform size is a power of two the analyser accepts");

        Analyser analyser;
        analyser.configure (size, 48000.0, 3);
        check (analyser.isConfigured(),
               "the analyser configures at every offered size, " + std::to_string (size));
    }
}

//==============================================================================
static void testBandLookup()
{
    Analyser analyser;
    analyser.configure (16384, 48000.0, 3);

    for (int i = 0; i < analyser.getNumBands(); ++i)
    {
        const auto& band = analyser.getBands()[(std::size_t) i];

        check (analyser.bandAt (band.centre) == i, "a centre finds its own band");
        check (analyser.bandAt (band.lower) == i, "a lower edge belongs to the band above it");

        // Half open at the top, so an edge belongs to exactly one band and no
        // energy is counted twice.
        if (i + 1 < analyser.getNumBands())
            check (analyser.bandAt (band.upper) == i + 1,
                   "an upper edge belongs to the next band up");
    }

    check (analyser.bandAt (1.0) < 0, "nothing below the display is in a band");
    check (analyser.bandAt (30000.0) < 0, "nothing above the display is in a band");
    check (analyser.bandAt (-100.0) < 0, "a negative frequency is in no band");
}

//==============================================================================
int main()
{
    testBandGeometry();
    testWindowConstants();
    testDisplayRange();
    testBinWeights();
    testSineReadsItsOwnLevel();
    testNarrowBandsBlurRatherThanLose();
    testBandPowersSumToTheSignal();
    testSlopes();
    testBallistics();
    testChannelCombination();
    testRefusals();
    testHold();
    testRingCapacity();
    testBandLookup();

    if (failures == 0)
        std::printf ("SpectrumAnalyserTest: all checks passed\n");
    else
        std::printf ("SpectrumAnalyserTest: %d check(s) failed\n", failures);

    return failures == 0 ? 0 : 1;
}
