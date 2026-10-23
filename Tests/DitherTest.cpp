// ---------------------------------------------------------------------------
// Checks what the dither does to the error a word length reduction leaves
// behind, against the constants the published theory gives for it.
//
// This is arithmetic that cannot announce that it is wrong, and the ways it
// can be wrong are unusually close to right. Every one of the following
// produces a file that plays, meters correctly, and is wrong:
//
//   * No dither at all. The error is then a deterministic function of the
//     signal, so it is distortion rather than noise, and its level follows the
//     music. Measured here, swept across amplitude: the worst harmonic of a
//     low level sine stands at least 36.6 dB clear of the record's own noise
//     floor undithered and at most 4.1 dB clear of it dithered, and the
//     noise floor wanders by 3.5 dB as that sine fades.
//   * One uniform draw instead of two summed. The mean goes flat and the
//     variance does not, which the 1992 survey names noise modulation.
//   * Two draws differenced instead of summed. Identical density, identical
//     variance, identical flat moments, and a high pass spectrum. Only the
//     check on the spectrum tells these apart, which is why it is here.
//   * Half the amplitude, that is triangular over one LSB rather than two.
//     The variance is then a quarter of what it should be and the second
//     moment is not flattened at all.
//   * A quantiser that truncates rather than rounding. Floor is still
//     uniform, so the moments still flatten, and half an LSB of DC goes into
//     the file underneath them. This is not hypothetical: it is what JUCE's
//     own float writer does, and it is why the quantiser is in Dither.h.
//   * lrint in place of floor(x + 1/2), which reads as a simplification and
//     ties to even rather than up, following a floating point rounding mode
//     nothing here sets. The same mix then exports differently on different
//     platforms, and at 24 bit, which is never dithered, there is no draw to
//     carry the decision off the tie.
//   * The clamp applied after the conversion to int rather than before it.
//     Every code stays in range and every enormous positive sample comes out
//     at the bottom rail, because the conversion itself is undefined that far
//     out. GCC's -fsanitize=undefined does not include float-cast-overflow,
//     so nothing raises: only asserting which rail can see it.
//
// So the properties below are the theory's, measured, and not the header's
// formulas restated. The figures come from Lipshitz, Wannamaker and
// Vanderkooy, "Quantization and Dither: A Theoretical Survey", JAES vol 40,
// no 5, pp 355 to 375 (1992 May):
//
//   * Theorem 8: summing n independent 1 LSB rectangular draws makes the
//     first n moments of the total error independent of the input, with a
//     total error power of (n + 1) q^2 / 12. So two draws give q^2 / 4, flat.
//   * Equation (11): an undithered quantiser's error power is q^2 / 12.
//   * Section 5.3: one draw leaves the variance "clearly signal dependent".
//
// Randomised and swept rather than hand picked, because the whole claim is
// about what happens for every input rather than for some of them.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include "Dither.h"
#include "RealFFT.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <vector>

static int failures = 0;

static void fail (const std::string& what)
{
    ++failures;

    if (failures <= 20)
        std::printf ("  FAIL  %s\n", what.c_str());
    else if (failures == 21)
        std::printf ("  ... further failures not printed\n");
}

static void check (bool ok, const std::string& what)
{
    if (! ok)
        fail (what);
}

static void checkClose (double actual, double expected, double tolerance, const std::string& what)
{
    if (! (std::abs (actual - expected) <= tolerance))
        fail (what + ": got " + std::to_string (actual)
                   + ", expected " + std::to_string (expected)
                   + " within " + std::to_string (tolerance));
}

static void checkExact (double actual, double expected, const std::string& what)
{
    if (! (actual == expected))
        fail (what + ": got " + std::to_string (actual)
                   + ", expected exactly " + std::to_string (expected));
}

// A mean and a variance accumulated in one pass, which is all any of these
// need. Kept in double over a float signal, because the quantity being
// measured is a few tenths of one least significant bit.
struct Moments
{
    long   n   = 0;
    double sum = 0.0, sumSq = 0.0;

    void add (double v) noexcept { ++n; sum += v; sumSq += v * v; }

    double mean()     const noexcept { return n > 0 ? sum / (double) n : 0.0; }
    double power()    const noexcept { return n > 0 ? sumSq / (double) n : 0.0; }

    // Scaled by twelve throughout, because every figure the theory gives is a
    // multiple of q^2 / 12 and comparing 3.0 against 3.0 is easier to read,
    // and easier to get right, than comparing 0.25 against 0.25.
    double powerTimes12() const noexcept { return 12.0 * power(); }
};

// The error one quantiser leaves on one input, in units of a least
// significant bit, which is the unit every figure in the paper is in.
static double errorInSteps (Dither::Quantiser& q, double sampleInSteps, double scale)
{
    const int code = q.code ((float) (sampleInSteps / scale));
    return (double) code - sampleInSteps;
}

int main()
{
    using namespace Dither;

    // =====================================================================
    // The depth and mode tables, and the policy over them
    // =====================================================================

    // The two defaults, pinned, because they are decisions with consequences
    // rather than arbitrary starting points and because every other check in
    // this section is blind to them: the out of range checks below compare
    // what an unrecognised id gives against the default, which is satisfied
    // whatever the default is.
    //
    // 24 bit is what every export before this feature existed was written at,
    // and `Prefs::exportBitDepth` derives its own stored default from this
    // one, so moving it would quietly change what a project that has never
    // touched the setting writes. Triangular, because the policy below turns
    // dither off everywhere it would be wrong anyway, so the only thing a
    // default of none could do is leave a 16 bit master undithered with
    // nothing to say so.
    checkExact ((double) (int) defaultDepth, (double) (int) Depth::int24,
                "the default export depth should be 24 bit");
    checkExact ((double) (int) defaultMode, (double) (int) Mode::triangular,
                "the default dither should be triangular");

    // A stored id nobody recognises has to land on the default rather than
    // past the end of the list. It is the same discipline Prefs::choiceId
    // exists for, and the reason is the same: these come out of a settings
    // file people edit by hand.
    for (int stored : { -1000, -1, numDepths, numDepths + 1, 99999 })
        checkExact (depthToStored (depthFromStored (stored)), depthToStored (defaultDepth),
                    "an out of range stored depth should read as the default");

    for (int stored : { -1000, -1, numModes, 7 })
        checkExact (modeToStored (modeFromStored (stored)), modeToStored (defaultMode),
                    "an out of range stored mode should read as the default");

    // A known gap, written down so it cannot quietly become a lie, the way
    // VoiceTest writes down the destinations nothing reaches yet.
    //
    // The bound in modeFromStored is unobservable from outside while there
    // are exactly two modes and the default is the second of them: an out of
    // range id that fell through the guard would land on the ternary's else
    // branch, which is the default anyway. Removing the guard, or taking it
    // off by one, is a genuine no-op today and the mutation pass found it so.
    // It stops being one the moment a third mode is added or the default
    // moves to Mode::none, and this assertion is what fails on that day and
    // sends whoever did it back to this comment.
    static_assert (numModes == 2,
                   "a third mode makes modeFromStored's bound observable: give it a case");
    static_assert (modeToStored (defaultMode) == numModes - 1,
                   "a default that is not the ternary's else branch makes the bound observable");

    // Every id the list really has has to survive a round trip, or a setting
    // somebody chose turns into a different one on the next launch.
    for (int i = 0; i < numDepths; ++i)
        checkExact (depthToStored (depthFromStored (i)), i,
                    "a stored depth in range should round trip");

    for (int i = 0; i < numModes; ++i)
        checkExact (modeToStored (modeFromStored (i)), i,
                    "a stored mode in range should round trip");

    const Depth depths[] = { Depth::int16, Depth::int24, Depth::float32 };

    for (auto d : depths)
    {
        // The name is what the preferences menu shows and what the export's
        // own report says it wrote, so a swapped pair is a studio telling
        // somebody it wrote a 24 bit file when it wrote a 16 bit one.
        // Asserted as a property rather than against the strings: a name has
        // to carry its own word length, which no other entry's does.
        check (name (d) != nullptr && name (d)[0] != '\0', "a depth with no name");

        check (std::string (name (d)).find (std::to_string (bitsFor (d))) != std::string::npos,
               std::string ("a depth's name should say its word length: ") + name (d));

        // The policy, asserted as a property rather than as the expression
        // that implements it: only a reduction to 16 bit is dithered. A
        // greyed control in the window is not this, because the engine would
        // still be free to disagree with it.
        checkExact ((double) (int) modeFor (d, Mode::triangular),
                    (double) (int) (d == Depth::int16 ? Mode::triangular : Mode::none),
                    std::string ("only 16 bit should be dithered, and ") + name (d) + " disagrees");

        // Asking for none never turns dither on, which is the direction
        // somebody would notice least.
        checkExact ((double) (int) modeFor (d, Mode::none), (double) (int) Mode::none,
                    std::string ("asking for no dither should give none at ") + name (d));
    }

    // The dither mode's name has to identify the mode for the same reason,
    // and distinctness is not enough on its own: a swapped pair is still two
    // different strings. So the dithered one names the density it is and the
    // undithered one does not.
    check (std::string (name (Mode::triangular)).find ("triangular") != std::string::npos,
           std::string ("the dithered mode should name its density: ") + name (Mode::triangular));
    check (std::string (name (Mode::none)).find ("triangular") == std::string::npos,
           std::string ("the undithered mode should not name a density: ") + name (Mode::none));

    // Every entry the enum has must be reachable from some stored id, which
    // is a different claim from the round trips above and from the out of
    // range checks, because both of those are bounded by the counts
    // themselves and so cannot see a count that is one short.
    //
    // The counts are also what builds the menu: PreferencesWindow fills its
    // two boxes with depthAt (i) and modeFromStored (i) for i below them. A
    // count one short therefore drops an entry from the menu and turns the
    // setting it had saved into the default on the next launch, with nothing
    // anywhere to say that the choice has gone.
    for (auto d : depths)
    {
        bool reachable = false;

        for (int i = 0; i < numDepths; ++i)
            if (depthAt (i) == d)
                reachable = true;

        check (reachable, std::string ("no stored id reaches ") + name (d));
    }

    for (auto m : { Mode::none, Mode::triangular })
    {
        bool reachable = false;

        for (int i = 0; i < numModes; ++i)
            if (modeFromStored (i) == m)
                reachable = true;

        check (reachable, std::string ("no stored id reaches ") + name (m));
    }

    checkExact ((double) bitsFor (Depth::int16),   16.0, "16 bit should be 16 bits");
    checkExact ((double) bitsFor (Depth::int24),   24.0, "24 bit should be 24 bits");
    checkExact ((double) bitsFor (Depth::float32), 32.0, "32 bit float should be 32 bits");

    check (  isFixedPoint (Depth::int16),   "16 bit is a word length reduction");
    check (  isFixedPoint (Depth::int24),   "24 bit is a word length reduction");
    check (! isFixedPoint (Depth::float32), "32 bit float is not a word length reduction");

    // The rails, which have to match what JUCE's own integer path produces or
    // moving the export onto this path moves them by a least significant bit.
    checkExact (scaleFor (Depth::int16),      32768.0,   "16 bit scale");
    checkExact (scaleFor (Depth::int24),    8388608.0,   "24 bit scale");
    checkExact ((double) lowestCode  (Depth::int16),  -32768.0, "16 bit lowest code");
    checkExact ((double) highestCode (Depth::int16),   32767.0, "16 bit highest code");
    checkExact ((double) lowestCode  (Depth::int24), -8388608.0, "24 bit lowest code");
    checkExact ((double) highestCode (Depth::int24),  8388607.0, "24 bit highest code");

    // =====================================================================
    // The contract with JUCE's shift
    // =====================================================================
    //
    // write (const int**, n) takes codes spanning the whole 32 bit range and
    // the WAV writer narrows them with an arithmetic right shift. Checked
    // exhaustively at 16 bit, where every code can be tried, and over the
    // rails and a randomised spread at 24, because an export that is off by
    // one shift is an export that is wrong by 48 dB and nothing says so.
    for (int code = lowestCode (Depth::int16); code <= highestCode (Depth::int16); ++code)
        if ((leftJustified (code, Depth::int16) >> 16) != code)
        {
            fail ("a 16 bit code should survive JUCE's shift: " + std::to_string (code));
            break;
        }

    {
        std::mt19937 rng (20261022u);
        std::uniform_int_distribution<int> any (lowestCode (Depth::int24), highestCode (Depth::int24));

        bool ok = true;
        for (int i = 0; i < 200000 && ok; ++i)
        {
            const int code = i == 0 ? lowestCode (Depth::int24)
                           : i == 1 ? highestCode (Depth::int24)
                           : i == 2 ? 0
                           : i == 3 ? -1
                                    : any (rng);

            if ((leftJustified (code, Depth::int24) >> 8) != code)
            {
                fail ("a 24 bit code should survive JUCE's shift: " + std::to_string (code));
                ok = false;
            }
        }
    }

    // =====================================================================
    // The dither's own density
    // =====================================================================
    //
    // Triangular over two least significant bits, so its variance is
    // q^2 / 6, which is 2.0 in units of q^2 / 12. This is the check that
    // catches the two amplitude mistakes that are invisible from the output
    // of the quantiser alone: a triangular draw half this wide reads 0.5, and
    // a uniform draw this wide reads 4.0.
    {
        std::uint64_t state = 0x1234'5678'9abc'def0ull;
        Moments m;
        double lowest = 1.0e9, highest = -1.0e9;

        const long count = 1 << 22;
        for (long i = 0; i < count; ++i)
        {
            const double v = triangularDraw (state);
            m.add (v);
            lowest  = std::min (lowest, v);
            highest = std::max (highest, v);
        }

        checkClose (m.mean(), 0.0, 0.003, "the dither should have no mean");
        checkClose (m.powerTimes12(), 2.0, 0.02, "the dither's variance should be q^2 / 6");

        check (lowest  >= -1.0, "the dither should not reach below one step below");
        check (highest <=  1.0, "the dither should not reach above one step above");

        // And it really does use the whole span, so a draw that was quietly
        // narrower would not pass by sitting in the middle.
        check (lowest  < -0.97, "the dither should reach most of the way down");
        check (highest >  0.97, "the dither should reach most of the way up");

        // Triangular rather than merely bounded: a uniform draw of the same
        // span would put a tenth of its samples in each tenth of the range,
        // while a triangular one puts far more in the middle. Checked as the
        // fraction landing within half a step of zero, which is 0.75 for a
        // triangle on plus or minus one and 0.5 for a uniform.
        std::uint64_t again = 0x1234'5678'9abc'def0ull;
        long inner = 0;
        for (long i = 0; i < count; ++i)
            if (std::abs (triangularDraw (again)) <= 0.5)
                ++inner;

        checkClose ((double) inner / (double) count, 0.75, 0.005,
                    "the dither's density should be triangular rather than uniform");
    }

    // =====================================================================
    // The moments of the total error, swept across one step
    // =====================================================================
    //
    // The sharpest form of the whole claim. A direct current input swept
    // across one quantisation step is the worst case for noise modulation,
    // because an undithered quantiser's error there goes from nothing at a
    // level to half a step between levels, and one uniform draw's variance
    // goes from nothing to q^2 / 4 over the same sweep.
    //
    // Theorem 8 says both the mean and the variance are flat. So: the mean is
    // zero at every one of these positions, and the variance is 3.0 in units
    // of q^2 / 12 at every one of them.
    {
        const int  positions = 32;
        const long perPos    = 1 << 18;
        const double scale   = scaleFor (Depth::int16);

        double worstMean = 0.0, lowestVar = 1.0e9, highestVar = -1.0e9;
        double undWorstMean = 0.0, undLowestVar = 1.0e9, undHighestVar = -1.0e9;
        double worstAbsError = 0.0;

        for (int p = 0; p < positions; ++p)
        {
            // Offset by a whole number of steps as well as a fraction, so the
            // check is not sitting at the bottom of the range where a
            // quantiser can look better behaved than it is.
            const double x = 1000.0 + (double) p / (double) positions;

            Quantiser dithered   (Depth::int16, Mode::triangular, 0xabcdef00ull + (std::uint64_t) p);
            Quantiser undithered (Depth::int16, Mode::none,       0xabcdef00ull + (std::uint64_t) p);

            Moments d, u;

            for (long i = 0; i < perPos; ++i)
            {
                d.add (errorInSteps (dithered,   x, scale));

                const double ue = errorInSteps (undithered, x, scale);
                u.add (ue);
                worstAbsError = std::max (worstAbsError, std::abs (ue));
            }

            worstMean  = std::max (worstMean, std::abs (d.mean()));
            lowestVar  = std::min (lowestVar,  d.powerTimes12());
            highestVar = std::max (highestVar, d.powerTimes12());

            undWorstMean  = std::max (undWorstMean, std::abs (u.mean()));
            undLowestVar  = std::min (undLowestVar,  u.powerTimes12());
            undHighestVar = std::max (undHighestVar, u.powerTimes12());
        }

        // The first moment, flat and zero. This is also the check that would
        // catch the quantiser being changed back to a truncation, which reads
        // about -0.4997 here.
        check (worstMean < 0.01,
               "the dithered error's mean should be zero at every position: worst was "
                   + std::to_string (worstMean));

        // The second moment, flat at the published figure. Both halves
        // matter: a wrong constant means the wrong amount of dither, and a
        // spread means noise modulation whatever the constant is.
        checkClose (0.5 * (lowestVar + highestVar), 3.0, 0.02,
                    "the dithered error's power should be q^2 / 4");

        // In decibels, because that is the unit the claim means anything in
        // and because it does not have to be rescaled when the sample count
        // changes. Measured at 0.07 dB over 32 positions of a quarter of a
        // million samples each.
        //
        // Deliberately not given a decibel figure to beat for the undithered
        // case, because it does not have one: across this same sweep the
        // undithered error power runs from exactly 0 to exactly q^2 / 4, a
        // ratio with a zero in the bottom of it. A direct current input
        // sitting on a code has no error at all and one sitting half a step
        // off has the most there is. The undithered comparison that does
        // have a figure is the fading sine below, at 3.5 dB.
        const double movedDb = 10.0 * std::log10 (highestVar / lowestVar);
        check (movedDb < 0.25,
               "the dithered error's power should not move across the sweep: it moved by "
                   + std::to_string (movedDb) + " dB");

        // And the undithered quantiser fails both, which is what says the
        // checks above are measuring something rather than passing anything.
        check (undWorstMean > 0.05,
               "an undithered quantiser's error should have a signal dependent mean");

        check (undHighestVar - undLowestVar > 0.5,
               "an undithered quantiser's error power should move across the sweep");

        // Round to nearest, asserted directly on the error rather than on the
        // expression. A floor has its error in (-1, 0] and a round to nearest
        // in [-0.5, 0.5], so this one line is the difference between the
        // quantiser the theory assumes and the one JUCE would have given.
        check (worstAbsError <= 0.5 + 1.0e-9,
               "an undithered error should never exceed half a step: worst was "
                   + std::to_string (worstAbsError));
    }

    // =====================================================================
    // An exact tie rounds up
    // =====================================================================
    //
    // Mid tread round to nearest leaves exactly one case to decide, and
    // Dither.h decides it: floor(x + 1/2) sends a sample sitting exactly half
    // a step between two codes to the upper one. The two alternatives that
    // read as simplifications, lrint and nearbyint, follow the current
    // floating point rounding mode, which ties to even by default, so the
    // same mix would export differently on a platform or inside a host that
    // had set that mode to anything else. Nothing in this studio sets it,
    // which is exactly why nothing would notice.
    //
    // Not a curiosity of measure zero either. A tie needs the scaled sample
    // to land on a half, which is any float that is an odd multiple of half a
    // step: 2^-16 at 16 bit and 2^-24 at 24 bit, both exactly representable
    // and both ordinary numbers for a quiet tail to pass through. And 24 bit
    // is never dithered, so there is no draw to carry the decision off the
    // tie and the whole file is written this way.
    //
    // Both signs and both parities of the code, because ties to even agrees
    // with ties up on half of them, and which half depends on the sign.
    {
        for (auto d : { Depth::int16, Depth::int24 })
        {
            Quantiser q (d, Mode::none, 0ull);
            const double scale = scaleFor (d);

            for (int code : { -4, -3, -2, -1, 0, 1, 2, 3 })
            {
                // An odd numerator over twice the scale, so the float is
                // exact and the scaled double is exactly a half.
                const float sample = (float) (((double) code + 0.5) / scale);

                checkExact ((double) q.code (sample), (double) (code + 1),
                            std::string ("a sample exactly half a step above code ")
                                + std::to_string (code) + " should round up at " + name (d));
            }
        }
    }

    // =====================================================================
    // Noise modulation under a fading sine
    // =====================================================================
    //
    // The same property as above in the form somebody actually hears it: the
    // noise floor of a fade out should not follow the fade. Checked at levels
    // down to the bottom of a 16 bit word, because that is where it matters
    // and where an undithered quantiser is worst.
    {
        const double scale = scaleFor (Depth::int16);
        // Starting below full scale rather than at it, and this is a real
        // property rather than a convenience. A sine peaking at exactly 0
        // dBFS reaches the top code, so the dither pushes it past the top and
        // the clamp engages on the peaks. That is the documented decision in
        // Dither.h, it is what both reference implementations do, and it does
        // add a little to the measured error power, which would otherwise read
        // here as the dither failing to be flat. Clipping is checked on its
        // own below, where it is the subject rather than a contaminant.
        const double dbs[] = { -6.0, -20.0, -40.0, -60.0, -80.0, -90.0, -96.0 };

        double lowestVar = 1.0e9, highestVar = -1.0e9;
        double undLowestVar = 1.0e9, undHighestVar = -1.0e9;

        for (double db : dbs)
        {
            const double amp = std::pow (10.0, db / 20.0) * scale;

            Quantiser dithered   (Depth::int16, Mode::triangular, 0x5eed'0001ull);
            Quantiser undithered (Depth::int16, Mode::none,       0x5eed'0001ull);

            Moments d, u;

            // An irrational number of samples per cycle, so the sine does not
            // land on the same few phases over and over: at an exact divisor
            // of the sample rate an undithered quantiser repeats one short
            // pattern and reads far better than it is.
            const long count = 1 << 18;
            const double w = 2.0 * 3.14159265358979323846 * 1000.0 / 48000.0;

            for (long i = 0; i < count; ++i)
            {
                const double x = amp * std::sin (w * (double) i);
                d.add (errorInSteps (dithered,   x, scale));
                u.add (errorInSteps (undithered, x, scale));
            }

            lowestVar  = std::min (lowestVar,  d.powerTimes12());
            highestVar = std::max (highestVar, d.powerTimes12());

            undLowestVar  = std::min (undLowestVar,  u.powerTimes12());
            undHighestVar = std::max (undHighestVar, u.powerTimes12());
        }

        const double movedDb = 10.0 * std::log10 (highestVar / lowestVar);

        check (movedDb < 0.25,
               "the dithered noise floor should not move as a sine fades: it moved by "
                   + std::to_string (movedDb) + " dB");

        checkClose (0.5 * (lowestVar + highestVar), 3.0, 0.05,
                    "the dithered noise floor should sit at q^2 / 4 at every level");

        // Measured at 3.5 dB of wander undithered against 0.03 dB dithered,
        // so a threshold of 1 dB separates them with room to spare.
        const double undWanderDb = 10.0 * std::log10 (undHighestVar / undLowestVar);
        check (undWanderDb > 1.0,
               "an undithered noise floor should move as a sine fades: it moved by "
                   + std::to_string (undWanderDb) + " dB");
    }

    // =====================================================================
    // A signal below one least significant bit
    // =====================================================================
    //
    // The property the thing exists for, rather than a statistic about it: a
    // sine a quarter of a step high is below the resolution of the word and
    // comes through anyway, because the dither moves the quantiser's decision
    // around it. Undithered the same sine quantises to a constant and is
    // gone.
    //
    // Recovered by correlating against the known frequency, which is what a
    // transform would do at that bin and is simpler to read.
    {
        const double scale = scaleFor (Depth::int16);
        const double amp   = 0.25;                    // a quarter of one step

        // A whole number of cycles, so that the sine and cosine sums over the
        // record are exactly zero and a constant code contributes exactly
        // nothing to the correlation. Without this the input's own offset
        // leaks into the reading: at 2097152 samples of a 48 sample cycle the
        // leftover part cycle put 0.05 of a step of apparent signal into the
        // undithered case, which is a fifth of the amplitude being measured.
        const long   cycles = 43690;                  // 48 samples each
        const long   count  = cycles * 48;
        const double w      = 2.0 * 3.14159265358979323846 * 1000.0 / 48000.0;

        Quantiser dithered   (Depth::int16, Mode::triangular, 0xfeed'0002ull);
        Quantiser undithered (Depth::int16, Mode::none,       0xfeed'0002ull);

        double dSin = 0.0, dCos = 0.0, uSin = 0.0, uCos = 0.0;
        bool undCodeVaried = false;
        int firstUndCode = 0;

        for (long i = 0; i < count; ++i)
        {
            const double phase = w * (double) i;
            const double x     = 1000.0 + amp * std::sin (phase);

            const int dCode = dithered.code ((float) (x / scale));
            const int uCode = undithered.code ((float) (x / scale));

            if (i == 0)
                firstUndCode = uCode;
            else if (uCode != firstUndCode)
                undCodeVaried = true;

            dSin += (double) dCode * std::sin (phase);
            dCos += (double) dCode * std::cos (phase);
            uSin += (double) uCode * std::sin (phase);
            uCos += (double) uCode * std::cos (phase);
        }

        const double got = 2.0 * std::hypot (dSin, dCos) / (double) count;
        const double und = 2.0 * std::hypot (uSin, uCos) / (double) count;

        // Within a quarter of a decibel of the amplitude that went in.
        checkClose (20.0 * std::log10 (got / amp), 0.0, 0.25,
                    "a sine a quarter of a step high should survive dithering");

        // Undithered it is not merely attenuated, the output is one constant
        // code, so there is nothing of it left at all.
        check (! undCodeVaried,
               "an undithered quantiser should flatten a sub step sine to one code");

        check (und < 0.02 * amp,
               "an undithered quantiser should leave nothing of a sub step sine: "
                   + std::to_string (und / amp) + " of it survived");
    }

    // =====================================================================
    // Harmonic distortion of a low level sine
    // =====================================================================
    //
    // The other half of what the undithered error being a function of the
    // signal means: its energy lands on harmonics of the note, so it arrives
    // as distortion rather than hiss and it follows the music.
    //
    // Swept across amplitude rather than measured at one, and that is a
    // correction rather than a flourish. The draft of this test took a sine
    // four steps high, called four steps "where it is worst", and asserted
    // the worst harmonic stood above -40 dB. Every part of that was wrong.
    // Undithered distortion depends on where the sine's peaks fall relative
    // to the codes, so it swings by up to 14.1 dB between neighbouring half
    // steps: measured, -25.4 dB at three and a half steps, -39.5 at four,
    // -28.6 at four and a half. Four steps is a local best case, and the
    // threshold sat 0.55 dB from red, so moving the amplitude to the more
    // representative five would have turned the suite red with nothing
    // wrong in Dither.h at all. Tests/DitherProbe.cpp has the sweep the
    // figures come from.
    //
    // So what is asserted is the property rather than a figure: at every
    // amplitude, the undithered worst harmonic stands clear of the record's
    // own noise floor and the dithered one does not. Measured over this
    // sweep the two do not come close to meeting, undithered clearing by at
    // least 36.6 dB and dithered by at most 4.1 dB.
    //
    // Against the record's own floor rather than a fixed number of decibels,
    // because a fixed number is a statement about this record length and
    // nothing else: the floor falls as the square root of the number of
    // samples, so a fixed threshold becomes unreachable or meaningless the
    // moment somebody changes n.
    //
    // Honestly, that makes this form less sensitive to n rather than
    // independent of it. The distortion sits at a fixed level and the floor
    // moves, so the clearance moves with n just as hard, in the other
    // direction. Measured, the worst clearance over this sweep is 13.3 dB at
    // n = 2^13, 24.4 at 2^15, 36.6 at 2^17 and 42.6 at 2^18, so halving n
    // twice to make this section faster, which is the obvious thing to try,
    // turns it red with nothing wrong. The same goes for the top of the
    // sweep: clearance falls with amplitude and crosses 20 dB between 96 and
    // 128 steps, so extending the sweep upward needs the threshold looked at
    // rather than assumed. What is asserted is a property of this quantiser
    // at these amplitudes and this record length, which is what a
    // measurement can carry.
    {
        const double scale = scaleFor (Depth::int16);
        const int    n     = 1 << 17;
        const int    bin   = 97;               // an odd bin, so harmonics are distinct
        const double w     = 2.0 * 3.14159265358979323846 * (double) bin / (double) n;

        RealFFT fft (n);
        const int bins = fft.getNumBins();

        std::vector<float> out ((size_t) n);
        std::vector<std::complex<double>> spectrum ((size_t) bins);

        // The worst harmonic and the level of everything that is not one,
        // both relative to the fundamental.
        auto measure = [&] (double amp, Mode mode)
        {
            Quantiser q (Depth::int16, mode, 0xbeef'0003ull);

            for (int i = 0; i < n; ++i)
                out[(size_t) i] = (float) q.code ((float) (amp * std::sin (w * (double) i) / scale));

            fft.forward (out.data(), spectrum.data());

            const double fundamental = std::abs (spectrum[(size_t) bin]);

            double worstHarmonic = 0.0, floorSum = 0.0;
            int    floorCount = 0;

            for (int k = 2; k < bins; ++k)
            {
                const double mag = std::abs (spectrum[(size_t) k]) / fundamental;

                // A harmonic of the fundamental, including the fundamental's
                // own bin and its immediate neighbours, which carry leakage
                // and are not distortion.
                const bool isHarmonic = (k % bin) == 0 || std::abs (k - bin) <= 2;

                if (isHarmonic)
                {
                    // A true harmonic, and only those. The condition above
                    // also keeps the fundamental's neighbours out of the
                    // floor, where they would be leakage rather than noise,
                    // but they are not distortion either and they must not
                    // reach this maximum: k / bin is 0 or 1 for all of them,
                    // so a test on k / bin alone lets every one of them
                    // through. With the sine on an exact bin they hold
                    // ordinary noise and nothing shows, and the first person
                    // to move it off the grid, which is the obvious way to
                    // make this more realistic, would see the dithered check
                    // fail at -0.04 dB and go looking at the dither.
                    if (k % bin == 0 && k / bin >= 2 && k / bin <= 7)
                        worstHarmonic = std::max (worstHarmonic, mag);
                }
                else
                {
                    floorSum += mag * mag;
                    ++floorCount;
                }
            }

            struct Result { double worstDb, floorDb; };
            return Result { 20.0 * std::log10 (worstHarmonic),
                            20.0 * std::log10 (std::sqrt (floorSum / (double) std::max (1, floorCount))) };
        };

        // From one and a half steps, which is about the quietest a sine can
        // be and still cross more than two codes, up to sixteen, by which
        // point the distortion is falling away on its own.
        for (double amp : { 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0, 6.0, 8.0, 12.0, 16.0 })
        {
            const auto und = measure (amp, Mode::none);
            const auto dit = measure (amp, Mode::triangular);

            const std::string at = " at " + std::to_string (amp) + " steps";

            // Twenty decibels of clearance, against a measured worst case of
            // 36.6, so this has sixteen decibels in hand at the tightest
            // amplitude in the sweep.
            check (und.worstDb > und.floorDb + 20.0,
                   "an undithered low level sine should show harmonic distortion" + at
                       + ": worst harmonic " + std::to_string (und.worstDb)
                       + " dB against a floor of " + std::to_string (und.floorDb) + " dB");

            // Dithered, the largest harmonic is one more sample of the same
            // noise the rest of the spectrum is made of. Measured, the worst
            // case over this sweep is 4.1 dB, against a threshold of 10.
            //
            // What sets that margin is the exponential tail of one noise
            // bin's power rather than how many bins are looked at, which is
            // six. Swept over 700 seeds against all twelve amplitudes, four
            // of the 8400 draws cleared more than 10 dB and the largest was
            // 10.9, so a different seed would redden this check roughly once
            // in 175 tries. The seed is fixed, so the test is deterministic
            // and cannot flake in CI, but changing it is not the free edit
            // it looks like: anybody who does should re-run that sweep
            // rather than just the one new seed. Rare event counts are
            // themselves noisy, so read it as an order of magnitude.
            check (dit.worstDb < dit.floorDb + 10.0,
                   "a dithered low level sine should show no harmonic above the noise" + at
                       + ": worst harmonic " + std::to_string (dit.worstDb)
                       + " dB against a floor of " + std::to_string (dit.floorDb) + " dB");
        }
    }

    // =====================================================================
    // The error spectrum is flat
    // =====================================================================
    //
    // The only check here that tells this dither from the one Ardour ships
    // under the same name. Differencing successive draws rather than summing
    // two fresh ones gives the same triangular density, the same variance of
    // q^2 / 6, the same flat first and second moments and the same total
    // error power, and a first order high pass spectrum: about q^2 / 12 at low
    // frequency rising to five times that at Nyquist. Every other check in
    // this file passes for both.
    //
    // Averaged over many blocks, because one periodogram of a noise process
    // has as much spread as it has size. That is the same lesson TiltTest
    // records for measuring a noise spectrum.
    {
        const double scale  = scaleFor (Depth::int16);
        const int    n      = 1 << 12;
        const int    blocks = 64;

        RealFFT fft (n);
        const int bins = fft.getNumBins();

        std::vector<double> power ((size_t) bins, 0.0);
        std::vector<float>  block ((size_t) n);
        std::vector<std::complex<double>> spectrum ((size_t) bins);

        Quantiser q (Depth::int16, Mode::triangular, 0xc0ff'ee04ull);

        // A slow sine under it rather than silence, so the quantiser is
        // actually working: with a constant input the error would be the
        // dither folded through one fixed decision and the test would be
        // measuring less than it looks.
        const double w = 2.0 * 3.14159265358979323846 * 220.0 / 48000.0;
        long sample = 0;

        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < n; ++i, ++sample)
            {
                const double x = 3000.0 * std::sin (w * (double) sample);
                block[(size_t) i] = (float) errorInSteps (q, x, scale);
            }

            fft.forward (block.data(), spectrum.data());

            for (int k = 0; k < bins; ++k)
            {
                const double mag = std::abs (spectrum[(size_t) k]);
                power[(size_t) k] += mag * mag;
            }
        }

        // Six bands from the bottom of the range to Nyquist, each averaged,
        // and each expressed in units of q^2 / 12 so the expected value is
        // 3.0 across all of them. Direct current and the first few bins are
        // left out: the sine under the test is at 220 Hz and its own
        // quantised fundamental lives down there.
        const double expected = 3.0;
        double lowest = 1.0e9, highest = -1.0e9;

        for (int band = 0; band < 6; ++band)
        {
            const int from = std::max (8, (int) ((double) bins * (double) band / 6.0));
            const int to   = (int) ((double) bins * (double) (band + 1) / 6.0);

            double sum = 0.0;
            int    count = 0;

            for (int k = from; k < to && k < bins; ++k)
            {
                ++count;
                sum += power[(size_t) k];
            }

            if (count == 0)
                continue;

            // Normalised so that a white error of variance v reads v.
            //
            // The transform is unscaled, so Parseval gives a total of n^2 v
            // across all n bins of a block. A real signal's bins mirror, so
            // the n/2 + 1 distinct bins each carry an expected n * v, not
            // n * v / 2: getting that factor of two wrong is why this read
            // 6.0 against an expected 3.0 on the first run, which is the
            // argument for the test asserting a figure the theory predicts
            // rather than whatever the first run happened to produce.
            const double density = 12.0 * (sum / (double) count)
                                        / ((double) blocks * (double) n);

            lowest  = std::min (lowest, density);
            highest = std::max (highest, density);

            checkClose (density, expected, 0.25,
                        "the error spectrum should be flat in band " + std::to_string (band));
        }

        // And flat across the bands rather than merely near the right level
        // in each, which is the form that catches a tilt. A first order high
        // pass would read about 1.0 at the bottom and 5.0 at the top, so a
        // ratio of 7 dB is far more than any plausible estimation spread and
        // far less than the 7 dB that error would show.
        const double tiltDb = 10.0 * std::log10 (highest / lowest);
        check (tiltDb < 1.0,
               "the error spectrum should not be tilted: it tilted by "
                   + std::to_string (tiltDb) + " dB");
    }

    // =====================================================================
    // Two channels get independent dither
    // =====================================================================
    //
    // Checked by cancellation, which is the method Synth/Lfo.h's test already
    // uses for the same question: two identical streams subtract to exactly
    // nothing, and two independent ones subtract to twice the power. Without
    // this, a stereo export would carry the same noise on both channels,
    // which images it in the centre rather than spreading it across the
    // field.
    {
        const double scale = scaleFor (Depth::int16);

        Quantiser left  (Depth::int16, Mode::triangular, 1);
        Quantiser right (Depth::int16, Mode::triangular, 2);
        Quantiser same  (Depth::int16, Mode::triangular, 1);

        Moments independent, identical;

        const long count = 1 << 20;
        const double w = 2.0 * 3.14159265358979323846 * 330.0 / 48000.0;

        for (long i = 0; i < count; ++i)
        {
            const double x = 500.0 * std::sin (w * (double) i);

            const double l = errorInSteps (left,  x, scale);
            const double r = errorInSteps (right, x, scale);
            const double s = errorInSteps (same,  x, scale);

            independent.add (l - r);
            identical.add (l - s);
        }

        // Two independent errors of power q^2 / 4 each, so the difference has
        // twice that: 6.0 in units of q^2 / 12.
        checkClose (independent.powerTimes12(), 6.0, 0.05,
                    "two channels' dither should be independent");

        // The same seed is the degenerate case, and it has to read as exactly
        // nothing or the check above is not measuring what it claims.
        checkExact (identical.power(), 0.0,
                    "two quantisers from one seed should produce identical output");
    }

    // =====================================================================
    // Clipping, nonsense, and the bounds of a code
    // =====================================================================
    {
        for (auto d : { Depth::int16, Depth::int24 })
        {
            Quantiser q (d, Mode::triangular, 0xdead'0005ull);

            // The rails are asserted undithered, because they are a claim
            // about the quantiser rather than about the dither. Dithered, full
            // scale negative legitimately comes back one code up when the
            // draw goes that way, which is the linearisation working and not
            // a rail moving.
            {
                Quantiser plain (d, Mode::none, 0ull);

                checkExact ((double) plain.code (-1.0f), (double) lowestCode (d),
                            std::string ("full scale negative should be the lowest code at ") + name (d));

                // Full scale positive is one code past the top, so it clamps.
                // The same thing JUCE's integer path does, where INT_MAX >> 16
                // is 32767 rather than 32768.
                checkExact ((double) plain.code (1.0f), (double) highestCode (d),
                            std::string ("full scale positive should clamp to the highest code at ") + name (d));

                // A NaN or an infinity has to be defined rather than merely
                // unlikely: converting either to an integer is undefined, and
                // carrying it into the file would put the extreme code there
                // with the loudest possible click on it. Read as silence,
                // which then goes through the ordinary path, so dithered it is
                // dithered silence rather than exactly zero.
                checkExact ((double) plain.code (std::numeric_limits<float>::quiet_NaN()), 0.0,
                            std::string ("a NaN sample should be silence at ") + name (d));
                checkExact ((double) plain.code (std::numeric_limits<float>::infinity()), 0.0,
                            std::string ("an infinite sample should be silence at ") + name (d));
                checkExact ((double) plain.code (-std::numeric_limits<float>::infinity()), 0.0,
                            std::string ("a negative infinite sample should be silence at ") + name (d));
            }

            for (float nonsense : { std::numeric_limits<float>::quiet_NaN(),
                                    std::numeric_limits<float>::infinity(),
                                    -std::numeric_limits<float>::infinity() })
                check (std::abs (q.code (nonsense)) <= 1,
                       std::string ("nonsense should be within a step of silence at ") + name (d));

            // Nothing, however absurd, may leave the range the file can hold.
            // A code outside it is not a loud sample, it is a wrapped one,
            // which is full scale noise of the opposite sign.
            std::mt19937 rng (7u);
            std::uniform_real_distribution<float> wild (-1000.0f, 1000.0f);

            for (int i = 0; i < 200000; ++i)
            {
                const float s = i == 0 ? 0.0f
                              : i == 1 ? -0.0f
                              : i == 2 ? std::numeric_limits<float>::denorm_min()
                              : i == 3 ? -std::numeric_limits<float>::max()
                              : i == 4 ? std::numeric_limits<float>::max()
                                       : wild (rng);

                const int code = q.code (s);

                if (code < lowestCode (d) || code > highestCode (d))
                {
                    fail (std::string ("a code left the range at ") + name (d) + ": "
                              + std::to_string (code) + " from " + std::to_string (s));
                    break;
                }
            }

            // Silence in is silence out, dither or no dither. The dither is
            // added before the quantiser, so a draw of more than half a step
            // on a silent input would round to a non zero code: a quarter of
            // the samples of a digital black file would be plus or minus one
            // least significant bit. That is correct, it is what makes the
            // quantiser linear, and it is also why a file exported from
            // silence is not bit for bit empty. Asserted in that direction so
            // nobody later "fixes" it.
            Quantiser quiet (d, Mode::triangular, 0x1111ull);
            bool anyNonZero = false;

            for (int i = 0; i < 1000; ++i)
                if (quiet.code (0.0f) != 0)
                    anyNonZero = true;

            if (modeFor (d, Mode::triangular) == Mode::triangular)
                check (anyNonZero, "dithered silence should not be bit for bit empty");
            else
                check (! anyNonZero, "undithered silence should be exactly empty");
        }
    }

    // =====================================================================
    // What clipping costs, and that the clamp is what does it
    // =====================================================================
    //
    // The decision in Dither.h is to clamp rather than to reserve headroom,
    // and the argument for it is that the dither can push a sample past full
    // scale by at most one step. These are the two halves of that argument,
    // measured.
    //
    // Measured on the error rather than on the code, and that distinction
    // cost two attempts at this check, both of which are worth recording
    // because both looked right.
    //
    // Reaching the highest code is not clipping: a mix that peaks at the
    // highest code is supposed to produce it. And a sine peaking at exactly
    // full scale almost never shows the clamp at all, because it would need a
    // sample landing within a hair of the peak and a dither draw near the top
    // of its range at that same sample, which over a quarter of a million
    // samples did not once happen. A property that needs two independent
    // coincidences is not a property, it is a lottery, and a check built on
    // one passes or fails by the seed.
    {
        const double scale = scaleFor (Depth::int16);
        const double w     = 2.0 * 3.14159265358979323846 * 997.0 / 48000.0;
        const long   count = 1 << 18;

        // Half of the argument: within full scale, and at it, the file is
        // never further from the mix than the rounding plus the dither, which
        // is half a step plus one step. At 16 bit that bound is -86.8 dBFS, so
        // no headroom is worth reserving against it.
        for (double peakSteps : { 1.0, 100.0, scale / 2.0, scale - 2.0, scale })
        {
            Quantiser q (Depth::int16, Mode::triangular, 0x9999ull);
            double worst = 0.0;

            for (long i = 0; i < count; ++i)
            {
                const double x = peakSteps * std::sin (w * (double) i);
                worst = std::max (worst, std::abs (errorInSteps (q, x, scale)));
            }

            check (worst <= 1.5 + 1.0e-9,
                   "a mix within full scale should never be further off than the rounding plus the "
                   "dither: at a peak of " + std::to_string (peakSteps)
                       + " steps the worst error was " + std::to_string (worst) + " steps");
        }

        // The other half: past full scale the clamp is what happens, and it
        // lands exactly on the rail rather than somewhere near it or wrapped
        // round to the other one. A wrapped code is not a loud sample, it is
        // full scale noise of the opposite sign.
        //
        // Carried out to the largest float there is, and at both depths,
        // because the thing that is not enough here is checking that the code
        // stayed in range. Clamping after the conversion to int rather than
        // before it reads as the tidier line and is the mistake the comment
        // in Dither.h is about: every code stays in range, and every large
        // positive sample lands on the bottom rail, because the conversion
        // itself is undefined past about 2^31 and gives the most negative
        // integer in practice, which the clamp then passes through as being
        // perfectly in bounds. Nothing raises, either: GCC's
        // -fsanitize=undefined does not include float-cast-overflow. The only
        // check that can see it is one that says which rail.
        {
            for (auto d : { Depth::int16, Depth::int24 })
            {
                Quantiser q (d, Mode::none, 0ull);

                for (float over : { 1.0001f, 1.5f, 2.0f, 1000.0f, 1.0e20f, 1.0e30f,
                                    std::numeric_limits<float>::max() })
                {
                    checkExact ((double) q.code (over), (double) highestCode (d),
                                std::string ("a sample above full scale should clamp to the highest "
                                             "code at ") + name (d) + ": " + std::to_string (over));
                    checkExact ((double) q.code (-over), (double) lowestCode (d),
                                std::string ("a sample below full scale should clamp to the lowest "
                                             "code at ") + name (d) + ": " + std::to_string (over));
                }
            }
        }

        // And with the dither on, so that a draw cannot carry a clamped sample
        // back off the rail in the wrong direction.
        {
            Quantiser q (Depth::int16, Mode::triangular, 0xaaaaull);
            long offRail = 0;

            for (long i = 0; i < count; ++i)
                if (q.code (2.0f) != highestCode (Depth::int16))
                    ++offRail;

            checkExact ((double) offRail, 0.0,
                        "a dithered sample well above full scale should still clamp to the rail");
        }
    }

    // =====================================================================
    // A float export is not quantised at all
    // =====================================================================
    //
    // There is no word length reduction, so there is nothing to linearise and
    // adding noise would be pure loss. Ardour makes this structural rather
    // than a default, hard coding no dither in its float converter, and
    // nothing read recommends dithering a float export.
    checkExact ((double) (int) modeFor (Depth::float32, Mode::triangular), (double) (int) Mode::none,
                "a float export should never be dithered");

    if (failures == 0)
        std::printf ("DitherTest: all checks passed\n");
    else
        std::printf ("DitherTest: %d check(s) failed\n", failures);

    return failures == 0 ? 0 : 1;
}
