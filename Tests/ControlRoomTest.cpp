// ---------------------------------------------------------------------------
// Checks what the control room section does to the speaker feed.
//
// Everything in here is a gain, so nothing it gets wrong can announce itself.
// A fold at the wrong coefficient does not error, does not crackle and does
// not look wrong on a meter: it puts the centre of the mix 3 dB away from
// where it was, so pressing Mono changes the thing it exists to let you
// judge. A dim at the wrong exponent is just a quieter dim. The per leg stage
// landing on the wrong side of the fold is invisible from the UI entirely.
//
// So the properties below are the claims ControlRoom.h's header comment
// makes, asserted as claims about the signal rather than by recomputing the
// header's arithmetic:
//
//   * The identity case is checked for bit equality, because an idle speaker
//     feed is one multiply by 1.0 and one by 0.0 and there is no reason for
//     it to be anything else.
//   * The fold's three justifications are each asserted directly: unity on
//     centred content, exactly 1/2 of a hard panned element, exact
//     cancellation of anti-correlated content, 3.01 dB down on two
//     uncorrelated legs (measured over randomised noise as well as derived),
//     and the textbook sum and difference halves M/S pair.
//   * The dim is read back as a number of decibels with a logarithm, which is
//     the opposite direction to the header's `pow`, so the two disagree if
//     either is wrong.
//   * `isNeutral` is checked against the matrix in both directions over every
//     combination of the six buttons crossed with a set of dim amounts, so
//     the indicator cannot drift from the signal.
//   * The overflow bound is checked at rest and, which is the strongest check
//     here, at every intermediate sample of a slew between thousands of
//     randomly generated pairs of states. Four coefficients ramped
//     independently do not travel along the straight line between the two
//     matrices, so the bound at both ends does not imply the bound in
//     between, and that is exactly the kind of gap this file is for.
//
// Two measurements in here are pointed at something known to be wrong first
// and required to notice it, because a measurement that cannot fail is not
// evidence: the overflow bound is shown to reject a 0.7071 fold, and the
// uncorrelated noise measurement is shown to tell 0.5 and 0.7071 apart.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include "ControlRoom.h"
#include "GainRamp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace ControlRoom;

static int failures = 0;

// Capped, because a property checked over thousands of states can fail
// thousands of times and only the first few lines are ever read. The count is
// still complete and is what the exit code comes from.
static void fail (const std::string& what)
{
    ++failures;

    if (failures <= 25)
        std::printf ("  FAIL  %s\n", what.c_str());
    else if (failures == 26)
        std::printf ("  ... further failures not printed\n");
}

// std::to_string gives six decimal places, which prints a bound of
// 1 + 5e-15 as "1.000000" and so hides the only thing a failure here needs to
// say: whether the excess is rounding or structural.
static std::string allDigits (double v)
{
    char buffer[64];
    std::snprintf (buffer, sizeof (buffer), "%.17g", v);
    return buffer;
}

static void check (bool ok, const std::string& what)
{
    if (! ok)
        fail (what);
}

static void checkExact (double actual, double expected, const std::string& what)
{
    // Written as a negated comparison so a NaN fails rather than passes.
    if (! (actual == expected))
        fail (what + ": got " + std::to_string (actual)
                   + ", expected exactly " + std::to_string (expected));
}

static void checkClose (double actual, double expected, double tolerance, const std::string& what)
{
    if (! (std::abs (actual - expected) <= tolerance))
        fail (what + ": got " + std::to_string (actual)
                   + ", expected " + std::to_string (expected)
                   + " within " + std::to_string (tolerance));
}

// ---------------------------------------------------------------------------
// The state space the properties are checked over.
//
// Six booleans is 64 states, which is small enough to enumerate completely, so
// there is no reason to pick cases. The dim amounts crossed with them include
// the two that are interesting for different reasons: exactly 0 dB, which the
// header says is engaged and neutral on purpose, and values outside the range
// and values that are not numbers, which are what a settings file written by
// another version hands over.
// ---------------------------------------------------------------------------
static std::vector<double> dimAmounts()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    return { 0.0, -0.0, -1.0, -6.0, -9.0, -12.0, -17.0, defaultDimDb,
             -24.5, -29.999, dimFloorDb,
             -45.0, -60.0, 3.0, 1.0e9, nan, inf, -inf };
}

static State stateFromBits (int bits, double db)
{
    State s;
    s.mono        = (bits & 1)  != 0;
    s.dim         = (bits & 2)  != 0;
    s.cutLeft     = (bits & 4)  != 0;
    s.cutRight    = (bits & 8)  != 0;
    s.invertLeft  = (bits & 16) != 0;
    s.invertRight = (bits & 32) != 0;
    s.dimDb       = db;
    return s;
}

static std::string describeState (const State& s)
{
    std::string out = "{";
    out += s.mono        ? " mono"   : "";
    out += s.dim         ? " dim"    : "";
    out += s.cutLeft     ? " cutL"   : "";
    out += s.cutRight    ? " cutR"   : "";
    out += s.invertLeft  ? " invL"   : "";
    out += s.invertRight ? " invR"   : "";
    out += " " + std::to_string (s.dimDb) + " }";
    return out;
}

static bool isIdentity (const Matrix& m)
{
    return m.leftFromLeft  == 1.0 && m.leftFromRight  == 0.0
        && m.rightFromLeft == 0.0 && m.rightFromRight == 1.0;
}

static bool isSilent (const Matrix& m)
{
    return m.leftFromLeft  == 0.0 && m.leftFromRight  == 0.0
        && m.rightFromLeft == 0.0 && m.rightFromRight == 0.0;
}

struct Out
{
    double left = 0.0, right = 0.0;
};

static Out apply (const Matrix& m, double L, double R)
{
    return { m.leftFromLeft  * L + m.leftFromRight  * R,
             m.rightFromLeft * L + m.rightFromRight * R };
}

// The section as the header describes it in words: a per leg stage of cut,
// polarity and dim, and then the fold, written identically to both legs. Over
// signals rather than over coefficients, so it is a different expression of
// the same contract and not a copy of `matrixFor`. The dim factor is taken
// from the header's own `dimGain` on purpose: how many decibels that is worth
// is checked separately and with a logarithm, and what this model is for is
// the structure, the order and the signs.
static Out reference (const State& s, double L, double R)
{
    const double dim = dimGain (s);
    const double l = s.cutLeft  ? 0.0 : (s.invertLeft  ? -L : L) * dim;
    const double r = s.cutRight ? 0.0 : (s.invertRight ? -R : R) * dim;

    if (s.mono)
    {
        const double m = 0.5 * (l + r);
        return { m, m };
    }

    return { l, r };
}

// splitmix64 and a uniform off it, rather than <random>, for the reason
// PanOneBreed.h gives: the distributions in <random> are not specified
// exactly, so the same seed gives a different sequence on MSVC and a failure
// would not reproduce from the seed printed beside it.
static unsigned long long rngState = 0x9E3779B97F4A7C15ull;

static unsigned long long nextBits()
{
    unsigned long long z = (rngState += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static double uniform (double lo, double hi)
{
    const double u = (double) (nextBits() >> 11) * (1.0 / 9007199254740992.0);
    return lo + u * (hi - lo);
}

static bool coin() { return (nextBits() & 1) != 0; }

int main()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const auto amounts = dimAmounts();

    // ---- 1. pass through -------------------------------------------------
    //
    // Bit exact, not close. A control room with nothing engaged is not a
    // process that happens to be nearly transparent, it is the mix.
    std::printf ("pass through\n");
    {
        const State fresh;
        const auto m = matrixFor (fresh);

        checkExact (m.leftFromLeft,   1.0, "a fresh state's leftFromLeft");
        checkExact (m.leftFromRight,  0.0, "a fresh state's leftFromRight");
        checkExact (m.rightFromLeft,  0.0, "a fresh state's rightFromLeft");
        checkExact (m.rightFromRight, 1.0, "a fresh state's rightFromRight");

        check (isNeutral (fresh), "a fresh state should be neutral");

        const auto p = passThrough();
        check (isIdentity (p), "passThrough should be the identity");

        // And through the signal, because the matrix being the identity is
        // only interesting if applying it is also exact.
        for (int i = 0; i < 2000; ++i)
        {
            const double L = uniform (-1.0, 1.0), R = uniform (-1.0, 1.0);
            const auto o = apply (m, L, R);
            checkExact (o.left,  L, "a fresh state should pass the left leg through");
            checkExact (o.right, R, "a fresh state should pass the right leg through");
        }

        // The default dim amount has to be a usable one or the first press of
        // a dim that has never been calibrated is a surprise.
        check (defaultDimDb >= dimFloorDb && defaultDimDb <= dimCeilingDb,
               "the default dim amount should be inside the adjustable range");
        check (dimFloorDb < dimCeilingDb, "the dim range should be the right way up");

        // The published dim amounts the header's comment collects, which is
        // what it says the range covers. Ardour's -12, the C|24's -20,
        // UAD's -17 default and the PreSonus and Grace floors at -30 are all
        // inside it.
        //
        // UAD Console's lowest discrete steps, which that same comment cites
        // as -43, -51 and -60 dB, are deliberately NOT asserted here. They
        // are outside this range on purpose, and the header says so in both
        // places rather than claiming to cover everything published: "the
        // console side of that spread and not all of it" at the top, and "a
        // 60 dB dim is a mute with extra steps" at `dimFloorDb`. So there is
        // nothing to report and nothing to fail. This paragraph used to say
        // the header overstated the range and that the overstatement was
        // being reported; the header had already been corrected and no
        // report was ever printed, so the note was sending the next reader to
        // fix a sentence that was right.
        for (double published : { -12.0, -17.0, -20.0, -30.0, -6.0, -9.0 })
            check (published >= dimFloorDb && published <= dimCeilingDb,
                   "the range should cover the published dim amount "
                       + std::to_string (published));

        // The three dim figures are pinned rather than derived, because
        // nothing derives them: the header says so plainly and names what
        // each one came from. A default is the one number here a person meets
        // before they have calibrated anything, so it is worth a check that
        // fails when somebody moves it without moving the citation with it.
        // -20 dB is the Digidesign C|24's documented reduction and the
        // majority of the console side; -30 dB is the Grace m908's and the
        // PreSonus Monitor Station V2's floor; 0 dB is what makes an engaged
        // dim able to do nothing, which `isNeutral` is written around.
        checkExact (defaultDimDb, -20.0, "the default dim amount");
        checkExact (dimFloorDb,   -30.0, "the dim floor");
        checkExact (dimCeilingDb,   0.0, "the dim ceiling");
    }

    // ---- 2. isNeutral is the matrix being the identity, both ways ---------
    //
    // A one way check lets the indicator either claim the speakers are
    // hearing the mix when they are not, or nag when nothing is happening to
    // the sound, and the second is how an indicator teaches people to ignore
    // it.
    std::printf ("isNeutral against the matrix, over %d x %d states\n",
                 64, (int) amounts.size());
    {
        int neutralCount = 0;

        for (double db : amounts)
            for (int bits = 0; bits < 64; ++bits)
            {
                const State s = stateFromBits (bits, db);
                const bool identity = isIdentity (matrixFor (s));

                if (identity)
                    ++neutralCount;

                check (isNeutral (s) == identity,
                       "isNeutral disagrees with the matrix for " + describeState (s)
                           + ": isNeutral " + (isNeutral (s) ? "true" : "false")
                           + ", identity " + (identity ? "true" : "false"));
            }

        std::printf ("  %d of %d states are neutral\n", neutralCount, 64 * (int) amounts.size());

        // Pinned explicitly because it is the case the header says is
        // deliberate: the dim latched on, calibrated to do nothing.
        State zeroDim;
        zeroDim.dim   = true;
        zeroDim.dimDb = 0.0;
        check (isNeutral (zeroDim), "a dim engaged at exactly 0 dB should be neutral");
        checkExact (dimGain (zeroDim), 1.0, "a dim engaged at exactly 0 dB");

        // And the other side of it: a dim engaged at the default is not
        // neutral, so the indicator does say something when the speakers are
        // quieter than the mix.
        State realDim;
        realDim.dim = true;
        check (! isNeutral (realDim), "a dim engaged at the default should not be neutral");
    }

    // ---- 3. the fold leaves centred content at unity ---------------------
    std::printf ("the fold on centred content\n");
    {
        State mono;
        mono.mono = true;
        const auto m = matrixFor (mono);

        // 0.5 + 0.5 is representable, so there is no excuse for a tolerance.
        checkExact (m.leftFromLeft + m.leftFromRight,   1.0, "the fold's left row should sum to unity");
        checkExact (m.rightFromLeft + m.rightFromRight, 1.0, "the fold's right row should sum to unity");

        for (int i = 0; i < 4000; ++i)
        {
            const double c = uniform (-1.0, 1.0);
            const auto o = apply (m, c, c);
            checkExact (o.left,  c, "centred content through the fold, left");
            checkExact (o.right, c, "centred content through the fold, right");
        }

        // The same with the dim calibrated to nothing, since that is a state
        // somebody can actually be sitting in.
        State monoZeroDim = mono;
        monoZeroDim.dim   = true;
        monoZeroDim.dimDb = 0.0;
        const auto mz = matrixFor (monoZeroDim);
        checkExact (mz.leftFromLeft + mz.leftFromRight, 1.0,
                    "the fold with a 0 dB dim should still sum to unity");
    }

    // ---- 4. the fold writes both legs identically ------------------------
    //
    // Over the enumeration rather than on one case, because the way this goes
    // wrong is one of the two rows being built from the wrong leg, which only
    // shows where the two legs differ.
    std::printf ("the fold writes both legs identically\n");
    {
        for (double db : amounts)
            for (int bits = 0; bits < 64; ++bits)
            {
                const State s = stateFromBits (bits, db);

                if (! s.mono)
                    continue;

                const auto m = matrixFor (s);
                checkExact (m.rightFromLeft,  m.leftFromLeft,
                            "a folded state's two rows differ in the left column for " + describeState (s));
                checkExact (m.rightFromRight, m.leftFromRight,
                            "a folded state's two rows differ in the right column for " + describeState (s));
            }
    }

    // ---- 5. the documented level consequences of the fold ----------------
    std::printf ("the level the fold costs\n");
    {
        State mono;
        mono.mono = true;
        const auto m = matrixFor (mono);

        // A hard panned element. -6.0206 dB is 20 log10 (1/2).
        for (int i = 0; i < 2000; ++i)
        {
            const double s = uniform (0.05, 1.0);
            const auto o = apply (m, s, 0.0);
            checkClose (20.0 * std::log10 (o.left / s), -6.020599913279624, 1.0e-9,
                        "a hard panned element through the fold");
            checkExact (o.right, o.left, "a hard panned element should reach both legs equally");
        }

        // Two uncorrelated legs of equal power, derived: the fold's power
        // gain on independent legs is the sum of the squared coefficients.
        {
            const double power = m.leftFromLeft * m.leftFromLeft
                               + m.leftFromRight * m.leftFromRight;
            checkClose (10.0 * std::log10 (power), -3.010299956639812, 1.0e-12,
                        "two uncorrelated legs through the fold, derived");
        }

        // And measured, over randomised uncorrelated noise, which is the
        // version that would also catch the fold reading one leg twice. The
        // measurement is pointed at a known wrong coefficient first: if it
        // cannot tell 0.5 from 0.7071 it is not evidence about either.
        {
            const int n = 400000;
            double inPower = 0.0, outPower = 0.0, wrongPower = 0.0;
            const Matrix wrong { 0.7071067811865476, 0.7071067811865476,
                                 0.7071067811865476, 0.7071067811865476 };

            for (int i = 0; i < n; ++i)
            {
                // Two independent legs. Uniform rather than Gaussian so a
                // single sample cannot leave full scale, which is what the
                // overflow claim is about.
                const double L = uniform (-1.0, 1.0), R = uniform (-1.0, 1.0);

                inPower  += 0.5 * (L * L + R * R);
                const auto o  = apply (m, L, R);
                const auto ow = apply (wrong, L, R);
                outPower   += o.left * o.left;
                wrongPower += ow.left * ow.left;
            }

            const double measured = 10.0 * std::log10 (outPower / inPower);
            const double known    = 10.0 * std::log10 (wrongPower / inPower);
            std::printf ("  two uncorrelated legs: measured %.4f dB (0.7071 would read %.4f dB)\n",
                         measured, known);

            checkClose (measured, -3.010299956639812, 0.05,
                        "two uncorrelated legs through the fold, measured");

            // The same measurement, able to fail: the number the header says
            // is wrong has to read differently, and it is the +3.01 dB of
            // clipping headroom the comment describes.
            check (std::abs (known - measured) > 2.5,
                   "the uncorrelated measurement cannot tell 0.5 from 0.7071, so it is not evidence");
            checkClose (known, 0.0, 0.05,
                        "a 0.7071 fold on two uncorrelated legs");
        }

        // Anti-correlated content cancels exactly. This is the thing the
        // button is pressed to find, so "nearly nothing" is not the claim.
        for (int i = 0; i < 2000; ++i)
        {
            const double v = uniform (-1.0, 1.0);
            const auto o = apply (m, v, -v);
            checkExact (o.left,  0.0, "anti-correlated content through the fold, left");
            checkExact (o.right, 0.0, "anti-correlated content through the fold, right");
        }
    }

    // ---- 6. mid and side -------------------------------------------------
    //
    // The sum and difference halves convention, which is the header's third
    // argument for the coefficient: with 0.7071 these come out a factor of
    // sqrt(2) away from the textbook pair.
    std::printf ("mid and side\n");
    {
        State diffRight;
        diffRight.mono        = true;
        diffRight.invertRight = true;

        State diffLeft;
        diffLeft.mono       = true;
        diffLeft.invertLeft = true;

        const auto mr = matrixFor (diffRight);
        const auto ml = matrixFor (diffLeft);

        for (int i = 0; i < 4000; ++i)
        {
            const double L = uniform (-1.0, 1.0), R = uniform (-1.0, 1.0);

            // Exact: 0.5 is a power of two, so halving either the difference
            // or each leg rounds identically.
            const double side = 0.5 * (L - R);

            const auto a = apply (mr, L, R);
            checkExact (a.left,  side, "inverting the right leg under the fold should give (L - R) / 2, left");
            checkExact (a.right, side, "inverting the right leg under the fold should give (L - R) / 2, right");

            const auto b = apply (ml, L, R);
            checkExact (b.left,  -side, "inverting the left leg under the fold should give -(L - R) / 2, left");
            checkExact (b.right, -side, "inverting the left leg under the fold should give -(L - R) / 2, right");

            // And the mid, for the other half of the pair.
            State mono;
            mono.mono = true;
            const auto mid = apply (matrixFor (mono), L, R);
            checkExact (mid.left, 0.5 * (L + R), "the fold should give (L + R) / 2");

            // The inverse the header names: L = M + S and R = M - S.
            checkExact (mid.left + a.left, L, "M + S should reconstruct the left leg");
            checkExact (mid.left - a.left, R, "M - S should reconstruct the right leg");
        }

        // The whole point of the difference listen: centred content is gone.
        for (int i = 0; i < 2000; ++i)
        {
            const double c = uniform (-1.0, 1.0);
            const auto o = apply (mr, c, c);
            checkExact (o.left,  0.0, "a centred signal in the difference listen, left");
            checkExact (o.right, 0.0, "a centred signal in the difference listen, right");
        }
    }

    // ---- 7. the per leg stage is before the fold -------------------------
    //
    // Pinned rather than discovered, because it is invisible from the UI: the
    // other order gives `R` on both legs where this one gives `0.5 * R`, and
    // both sound like a mono fold with one leg cut.
    std::printf ("the per leg stage before the fold\n");
    {
        State cutL;
        cutL.mono    = true;
        cutL.cutLeft = true;
        const auto a = matrixFor (cutL);
        checkExact (a.leftFromLeft,   0.0, "cut left plus mono, leftFromLeft");
        checkExact (a.leftFromRight,  0.5, "cut left plus mono, leftFromRight");
        checkExact (a.rightFromLeft,  0.0, "cut left plus mono, rightFromLeft");
        checkExact (a.rightFromRight, 0.5, "cut left plus mono, rightFromRight");

        State cutR;
        cutR.mono     = true;
        cutR.cutRight = true;
        const auto b = matrixFor (cutR);
        checkExact (b.leftFromLeft,   0.5, "cut right plus mono, leftFromLeft");
        checkExact (b.leftFromRight,  0.0, "cut right plus mono, leftFromRight");
        checkExact (b.rightFromLeft,  0.5, "cut right plus mono, rightFromLeft");
        checkExact (b.rightFromRight, 0.0, "cut right plus mono, rightFromRight");

        for (int i = 0; i < 2000; ++i)
        {
            const double L = uniform (-1.0, 1.0), R = uniform (-1.0, 1.0);
            const auto o = apply (a, L, R);
            checkExact (o.left,  0.5 * R, "cut left plus mono should give half the right leg");
            checkExact (o.right, 0.5 * R, "cut left plus mono should give half the right leg on both");
        }

        // A cut with the fold off touches its own row only.
        State onlyCutL;
        onlyCutL.cutLeft = true;
        const auto c = matrixFor (onlyCutL);
        checkExact (c.leftFromLeft,   0.0, "cut left alone, leftFromLeft");
        checkExact (c.leftFromRight,  0.0, "cut left alone, leftFromRight");
        checkExact (c.rightFromLeft,  0.0, "cut left alone should leave the right row alone");
        checkExact (c.rightFromRight, 1.0, "cut left alone should leave the right row alone");

        State onlyCutR;
        onlyCutR.cutRight = true;
        const auto d = matrixFor (onlyCutR);
        checkExact (d.leftFromLeft,   1.0, "cut right alone should leave the left row alone");
        checkExact (d.leftFromRight,  0.0, "cut right alone should leave the left row alone");
        checkExact (d.rightFromLeft,  0.0, "cut right alone, rightFromLeft");
        checkExact (d.rightFromRight, 0.0, "cut right alone, rightFromRight");

        // A cut beats a polarity invert on the same leg, which is the only
        // reading of a cut that means anything.
        State cutAndInvert;
        cutAndInvert.cutLeft    = true;
        cutAndInvert.invertLeft = true;
        const auto e = matrixFor (cutAndInvert);
        checkExact (e.leftFromLeft, 0.0, "a cut leg that is also inverted should still be silent");
    }

    // ---- 7b. the whole section is linear, and in that order --------------
    //
    // The header's claim that it resolves to four coefficients, checked
    // against the section written as a sequence of stages over signals. This
    // is where a fold on the wrong side of the per leg stage, a swapped
    // column or a polarity applied to the wrong leg shows up for every state
    // at once rather than only in the cases pinned above.
    std::printf ("the section as stages against the matrix, over %d x %d states\n",
                 64, (int) amounts.size());
    {
        for (double db : amounts)
            for (int bits = 0; bits < 64; ++bits)
            {
                const State s = stateFromBits (bits, db);
                const auto m = matrixFor (s);

                for (int i = 0; i < 12; ++i)
                {
                    const double L = uniform (-1.0, 1.0), R = uniform (-1.0, 1.0);
                    const auto got = apply (m, L, R);
                    const auto want = reference (s, L, R);

                    checkExact (got.left,  want.left,
                                "the matrix disagrees with the staged section, left leg, " + describeState (s));
                    checkExact (got.right, want.right,
                                "the matrix disagrees with the staged section, right leg, " + describeState (s));
                }
            }
    }

    // ---- 8. the dim is the number of decibels asked for ------------------
    //
    // Read back with a logarithm, which is the opposite direction to the
    // header's exponential, so the exponent and the sign of the decibels are
    // both checked against the definition rather than against themselves.
    std::printf ("the dim in decibels\n");
    {
        for (double db = dimFloorDb; db <= dimCeilingDb + 1.0e-12; db += 0.25)
        {
            State s;
            s.dim   = true;
            s.dimDb = db;

            const double g = dimGain (s);
            checkClose (20.0 * std::log10 (g), db, 1.0e-9,
                        "the dim gain at " + std::to_string (db) + " dB");

            // And it composes multiplicatively with everything else rather
            // than being a case of its own: every undimmed coefficient scaled
            // by the same factor, in every state.
            for (int bits = 0; bits < 64; ++bits)
            {
                State dimmed = stateFromBits (bits, db);
                dimmed.dim = true;
                State plain = dimmed;
                plain.dim = false;

                const auto a = matrixFor (dimmed);
                const auto b = matrixFor (plain);

                const double ea[4] { a.leftFromLeft, a.leftFromRight, a.rightFromLeft, a.rightFromRight };
                const double eb[4] { b.leftFromLeft, b.leftFromRight, b.rightFromLeft, b.rightFromRight };

                for (int k = 0; k < 4; ++k)
                {
                    if (eb[k] == 0.0)
                    {
                        checkExact (ea[k], 0.0, "the dim should leave a silent coefficient silent");
                        continue;
                    }

                    // Relative, because the coefficients span 1.0 down to
                    // 0.0158 and an absolute tolerance would be meaningless
                    // at one end or the other.
                    const double ratio = ea[k] / eb[k];
                    checkClose (20.0 * std::log10 (ratio), db, 1.0e-9,
                                "the dim should scale coefficient " + std::to_string (k)
                                    + " by " + std::to_string (db) + " dB for "
                                    + describeState (dimmed));
                }
            }
        }

        // A dim that is not engaged is exactly one, not nearly one: a latched
        // dim is the only thing that should cost a multiply.
        for (double db : amounts)
        {
            State s;
            s.dim   = false;
            s.dimDb = db;
            checkExact (dimGain (s), 1.0, "a dim that is not engaged");
        }

        // Monotone in the amount, over the range, which is the one thing a
        // person turning the knob can actually hear.
        double previous = -1.0;
        for (double db = dimFloorDb; db <= dimCeilingDb; db += 0.1)
        {
            State s;
            s.dim   = true;
            s.dimDb = db;
            const double g = dimGain (s);
            check (g > previous, "the dim gain should rise with the amount at " + std::to_string (db));
            previous = g;
        }
    }

    // ---- 9. clampDimDb and a dim gain that is always a number ------------
    std::printf ("clampDimDb\n");
    {
        for (double db : { 0.0, -0.5, -1.0, -6.0, -12.0, -17.0, -20.0, -29.5, -30.0 })
            checkExact (clampDimDb (db), db, "a dim amount inside the range should come back unchanged");

        for (double db : { -30.000001, -45.0, -60.0, -1.0e6, -1.0e300 })
            checkExact (clampDimDb (db), dimFloorDb, "a dim amount below the floor should clamp to the floor");

        for (double db : { 0.000001, 3.0, 20.0, 1.0e6, 1.0e300 })
            checkExact (clampDimDb (db), dimCeilingDb, "a dim amount above the ceiling should clamp to the ceiling");

        // The one the header singles out. std::min and std::max propagate a
        // NaN rather than reject it, and a NaN coefficient does not go quiet,
        // it silences the speaker feed and says nothing about where it came
        // from.
        checkExact (clampDimDb (nan),  defaultDimDb, "a NaN dim amount should come back as the default");
        checkExact (clampDimDb (inf),  defaultDimDb, "an infinite dim amount should come back as the default");
        checkExact (clampDimDb (-inf), defaultDimDb, "a negative infinite dim amount should come back as the default");

        // And the consequence, which is the thing that actually reaches the
        // speakers: no stored value whatsoever produces a coefficient that is
        // not a number.
        const double floorGain = std::pow (10.0, dimFloorDb / 20.0);

        for (int i = 0; i < 20000; ++i)
        {
            State s;
            s.dim = true;

            switch (i % 6)
            {
                case 0:  s.dimDb = uniform (-40.0, 10.0);     break;
                case 1:  s.dimDb = uniform (-1.0e300, 1.0e300); break;
                case 2:  s.dimDb = nan;                        break;
                case 3:  s.dimDb = inf;                        break;
                case 4:  s.dimDb = -inf;                       break;
                default: s.dimDb = uniform (dimFloorDb, dimCeilingDb); break;
            }

            const double g = dimGain (s);

            check (std::isfinite (g), "a dim gain that is not a number, from " + describeState (s));
            check (g > 0.0 && g <= 1.0,
                   "a dim gain outside (0, 1] from " + describeState (s)
                       + ": " + std::to_string (g));
            check (g >= floorGain * (1.0 - 1.0e-12),
                   "a dim quieter than the floor from " + describeState (s));

            // The matrix built from it has to be finite too, since that is
            // what gets multiplied into the buffer.
            const auto m = matrixFor (s);
            check (std::isfinite (m.leftFromLeft) && std::isfinite (m.leftFromRight)
                    && std::isfinite (m.rightFromLeft) && std::isfinite (m.rightFromRight),
                   "a matrix coefficient that is not a number, from " + describeState (s));
        }
    }

    // ---- 10. the overflow bound at rest ----------------------------------
    std::printf ("the overflow bound at rest\n");
    {
        double worst = 0.0;

        for (double db : amounts)
            for (int bits = 0; bits < 64; ++bits)
            {
                const State s = stateFromBits (bits, db);
                const auto m = matrixFor (s);

                worst = std::max (worst, std::max (leftBound (m), rightBound (m)));

                check (leftBound (m) <= 1.0, "the left bound exceeds unity for " + describeState (s)
                                                 + ": " + allDigits (leftBound (m)));
                check (rightBound (m) <= 1.0, "the right bound exceeds unity for " + describeState (s)
                                                  + ": " + allDigits (rightBound (m)));
            }

        std::printf ("  worst bound at rest: %.17g\n", worst);

        // What the bound means, rather than only how large it is: it is the
        // largest the leg can get for input inside full scale, so it has to
        // be both respected and attained over the corners of the unit square.
        // Checked for its own row, which is the half a bound that reads the
        // other row would pass without: both rows are equal under the fold
        // and differ everywhere else.
        for (double db : amounts)
            for (int bits = 0; bits < 64; ++bits)
            {
                const State s = stateFromBits (bits, db);
                const auto m = matrixFor (s);

                double peakLeft = 0.0, peakRight = 0.0;

                for (double L : { -1.0, 1.0 })
                    for (double R : { -1.0, 1.0 })
                    {
                        const auto o = apply (m, L, R);
                        peakLeft  = std::max (peakLeft,  std::abs (o.left));
                        peakRight = std::max (peakRight, std::abs (o.right));
                    }

                // Exact: the corner whose signs match the coefficients sums
                // two numbers of the same sign, which rounds the same way as
                // adding their magnitudes.
                checkExact (peakLeft,  leftBound (m),
                            "the left bound is not what the left leg reaches for " + describeState (s));
                checkExact (peakRight, rightBound (m),
                            "the right bound is not what the right leg reaches for " + describeState (s));
            }

        // The bound measurement pointed at the number the header exists to
        // rule out. If it does not reject a 0.7071 fold then nothing it says
        // about 0.5 is evidence.
        const Matrix wrong { 0.7071067811865476, 0.7071067811865476,
                             0.7071067811865476, 0.7071067811865476 };
        check (leftBound (wrong) > 1.0,
               "the overflow bound does not reject a 0.7071 fold, so it is not evidence");

        // It is the sum of absolute values and not the sum, which is the
        // distinction that matters the moment a polarity is inverted: a
        // difference listen sums to zero and bounds at one.
        State diff;
        diff.mono        = true;
        diff.invertRight = true;
        const auto d = matrixFor (diff);
        checkExact (leftBound (d), 1.0, "the difference listen's left bound");
        checkExact (d.leftFromLeft + d.leftFromRight, 0.0,
                    "the difference listen's coefficients should sum to nothing");
    }

    // ---- 11. the overflow bound during a gesture -------------------------
    //
    // The strongest check here, and the reason it is randomised: the engine
    // slews the four coefficients independently, and because each ramp is
    // clamped against its own target the one with less travel arrives first.
    // So the path between two matrices is not the straight line between them,
    // and the bound holding at both ends says nothing about the corner. This
    // walks every intermediate sample.
    std::printf ("the overflow bound during a slew\n");
    {
        const std::vector<double> steps
        {
            // A step larger than the whole travel, which is what a tiny
            // buffer at a low rate gives, and what `stepPerSample` returns
            // before a device has opened at all.
            GainRamp::stepPerSample (0.0, GainRamp::levelSeconds),
            2.0,
            0.5,
            0.1,
            GainRamp::stepPerSample (44100.0, GainRamp::panSeconds),
            GainRamp::stepPerSample (44100.0, GainRamp::levelSeconds),
            GainRamp::stepPerSample (48000.0, GainRamp::levelSeconds),
            1.0e-3
        };

        double worst = 0.0;
        std::string worstWhere;
        long long samples = 0;

        auto randomState = [&]()
        {
            State s;
            s.mono        = coin();
            s.dim         = coin();
            s.cutLeft     = coin();
            s.cutRight    = coin();
            s.invertLeft  = coin();
            s.invertRight = coin();

            // Garbage amounts included, because a stored dim that has not
            // been read yet is exactly the case the clamp is there for and
            // the bound has to hold through it too.
            switch ((int) (nextBits() % 8))
            {
                case 0:  s.dimDb = 0.0;              break;
                case 1:  s.dimDb = dimFloorDb;       break;
                case 2:  s.dimDb = uniform (-60.0, 10.0); break;
                case 3:  s.dimDb = nan;              break;
                case 4:  s.dimDb = inf;              break;
                default: s.dimDb = uniform (dimFloorDb, dimCeilingDb); break;
            }

            return s;
        };

        for (int pair = 0; pair < 3000; ++pair)
        {
            const State from = randomState(), to = randomState();
            const auto a = matrixFor (from), b = matrixFor (to);

            const double targets[4] { b.leftFromLeft, b.leftFromRight, b.rightFromLeft, b.rightFromRight };
            const double starts[4]  { a.leftFromLeft, a.leftFromRight, a.rightFromLeft, a.rightFromRight };

            for (double step : steps)
            {
                GainRamp::Ramp r[4];
                for (int k = 0; k < 4; ++k)
                    r[k].latch (starts[k]);

                double previous[4] { starts[0], starts[1], starts[2], starts[3] };

                // Long enough for the slowest step to cross the widest
                // travel, which is 2.0, and then some, so arrival is reached
                // rather than assumed.
                const int limit = (int) std::min (6000.0, 4.0 + 2.5 / step);

                for (int n = 0; n < limit; ++n)
                {
                    Matrix m;
                    m.leftFromLeft   = r[0].next (targets[0], step);
                    m.leftFromRight  = r[1].next (targets[1], step);
                    m.rightFromLeft  = r[2].next (targets[2], step);
                    m.rightFromRight = r[3].next (targets[3], step);
                    ++samples;

                    const double now[4] { m.leftFromLeft, m.leftFromRight, m.rightFromLeft, m.rightFromRight };

                    // Raw conditionals rather than the check helpers in here,
                    // because this loop runs millions of times and building
                    // the message for every passing sample costs more than
                    // the whole rest of the file.
                    for (int k = 0; k < 4; ++k)
                    {
                        // Never reverses: always at least as close to the
                        // target as it was.
                        if (! (std::abs (now[k] - targets[k]) <= std::abs (previous[k] - targets[k])))
                            fail ("a ramp moved away from its target for "
                                      + describeState (from) + " -> " + describeState (to));

                        // Never overshoots. A polarity flip travels through
                        // zero and an overshoot there is a momentarily
                        // inverted gain, which is the one artifact worse than
                        // the click it is replacing.
                        if (! ((now[k] - targets[k]) * (starts[k] - targets[k]) >= 0.0))
                            fail ("a ramp overshot its target for "
                                      + describeState (from) + " -> " + describeState (to));

                        previous[k] = now[k];
                    }

                    const double bound = std::max (leftBound (m), rightBound (m));

                    if (bound > worst)
                    {
                        worst = bound;
                        worstWhere = describeState (from) + " -> " + describeState (to)
                                   + " step " + std::to_string (step);
                    }

                    // The slack is not a tolerance on the claim, it is the
                    // accumulation of the additions that got here. The bound
                    // is not merely respected along this path, it is reached:
                    // a slew from the mix into a fold holds |a| + |b| at
                    // exactly one for the whole of the move, so every sample
                    // of it is a sum of two numbers that have each been
                    // stepped a few hundred times and round in opposite
                    // directions. Measured over 13.2 million slewed samples
                    // the worst excess is 4.7e-15, which is 285 dB below the
                    // thing it would have to clip, and the worst case is
                    // printed below so a structural excess cannot hide in
                    // here: anything that is not rounding is orders of
                    // magnitude larger than this.
                    if (! (bound <= 1.0 + 1.0e-12))
                        fail ("the bound exceeds unity mid slew at " + allDigits (bound)
                                  + " for " + describeState (from) + " -> " + describeState (to));
                }

                // Arrival is exact, which is what lets a settled block be one
                // constant multiply.
                for (int k = 0; k < 4; ++k)
                {
                    checkExact (r[k].value, targets[k], "a ramp did not arrive exactly");
                    check (r[k].settled (targets[k]), "a ramp did not report itself settled");
                }
            }
        }

        // A separate pass with a step small enough that nothing arrives,
        // because the interesting corner is early in the move and a step this
        // size is what a high sample rate gives. No arrival assertion here,
        // deliberately: the point is the first part of the path.
        for (int pair = 0; pair < 40; ++pair)
        {
            const State from = randomState(), to = randomState();
            const auto a = matrixFor (from), b = matrixFor (to);

            const double targets[4] { b.leftFromLeft, b.leftFromRight, b.rightFromLeft, b.rightFromRight };
            GainRamp::Ramp r[4];
            r[0].latch (a.leftFromLeft);
            r[1].latch (a.leftFromRight);
            r[2].latch (a.rightFromLeft);
            r[3].latch (a.rightFromRight);

            for (int n = 0; n < 40000; ++n)
            {
                Matrix m;
                m.leftFromLeft   = r[0].next (targets[0], 1.0e-6);
                m.leftFromRight  = r[1].next (targets[1], 1.0e-6);
                m.rightFromLeft  = r[2].next (targets[2], 1.0e-6);
                m.rightFromRight = r[3].next (targets[3], 1.0e-6);
                ++samples;

                const double bound = std::max (leftBound (m), rightBound (m));

                if (bound > worst)
                {
                    worst = bound;
                    worstWhere = describeState (from) + " -> " + describeState (to) + " step 1e-6";
                }

                if (! (bound <= 1.0 + 1.0e-12))
                    fail ("the bound exceeds unity mid slew at " + allDigits (bound));
            }
        }

        std::printf ("  %lld slewed samples, worst bound %.17g\n", samples, worst);

        std::printf ("  worst case was %s\n", worstWhere.c_str());

        // The bound is reached, not merely respected, which is worth knowing:
        // a slew from the mix straight into a fold sits at exactly unity for
        // the whole of the move, so there is no headroom to spend here and a
        // fold coefficient any larger than 0.5 would overflow during the
        // gesture even if it did not at rest.
        check (worst >= 1.0 - 1.0e-12, "the bound should be reached during a slew, not just respected");
    }

    // ---- 12. describe ----------------------------------------------------
    //
    // The words are built next to the matrix so they cannot describe a state
    // the matrix does not have. The two claims checked here are the two that
    // would be read as statements of fact about the signal.
    //
    // The case that made this section worth writing: with the fold on, one
    // leg inverted and the other leg cut, the first version of `describe`
    // said "difference" while the matrix was half of one leg with a polarity
    // flip, which cancels nothing centred at all. It is pinned below rather
    // than excluded, and reinstating the old branch order fails it.
    std::printf ("describe against the matrix, over %d x %d states\n",
                 64, (int) amounts.size());
    {
        const std::string mixWords    = "hearing the mix";
        const std::string silentWords = "silent";

        for (double db : amounts)
            for (int bits = 0; bits < 64; ++bits)
            {
                const State s = stateFromBits (bits, db);
                const auto m = matrixFor (s);
                const char* raw = describe (s);

                check (raw != nullptr, "describe returned nothing for " + describeState (s));

                if (raw == nullptr)
                    continue;

                const std::string text (raw);

                check (! text.empty(), "describe returned an empty string for " + describeState (s));
                check (text.rfind ("control room: ", 0) == 0,
                       "describe should name the section: " + text);

                const bool saysMix = text.find (mixWords) != std::string::npos;
                check (saysMix == isNeutral (s),
                       "describe says \"" + text + "\" where isNeutral is "
                           + (isNeutral (s) ? "true" : "false") + " for " + describeState (s));
                check (saysMix == isIdentity (m),
                       "describe says \"" + text + "\" where the matrix is "
                           + (isIdentity (m) ? "" : "not ") + "the identity for " + describeState (s));

                const bool saysSilent = text.find (silentWords) != std::string::npos;
                check (saysSilent == isSilent (m),
                       "describe says \"" + text + "\" where the matrix is "
                           + (isSilent (m) ? "" : "not ") + "all zeroes for " + describeState (s));

                // And a state that is neither has to say something else,
                // rather than falling through to one of the two readings that
                // would be a claim about the signal.
                if (! isIdentity (m) && ! isSilent (m))
                    check (! saysMix && ! saysSilent,
                           "describe makes a claim about the signal it does not have: " + text);

                // The fold is named whenever the fold is on, and never when
                // it is off. Every reading of a folded state mentions it,
                // including the difference one, which calls it a mono fold.
                // Excluding the silent state, which describes the cuts
                // instead and is right to: what the fold does with silence is
                // not what somebody needs to be told.
                if (! isSilent (m))
                    check ((text.find ("mono") != std::string::npos) == s.mono,
                           "describe says \"" + text + "\" where mono is "
                               + (s.mono ? "on" : "off") + " for " + describeState (s));

                // The difference reading is a statement that centred content
                // is gone, so it has to actually be gone. Asserted over every
                // state rather than over the ones with both legs present,
                // which is the strong form and the form that found the fault:
                // the first version of `describe` claimed a difference listen
                // whenever the fold was on and the polarities differed, and
                // with one leg cut and the other inverted that is half of the
                // surviving leg rather than a cancellation. A centred signal
                // came out at half level while the strip said it was gone.
                // Reachable in four button presses, invisible to every other
                // check in this file, and a sentence rather than a signal,
                // which is why it is pinned here and not left to an eye.
                const auto centred = apply (m, 0.37, 0.37);
                const bool cancels = centred.left == 0.0 && centred.right == 0.0
                                  && ! isSilent (m);

                check ((text.find ("difference") != std::string::npos) == cancels,
                       "describe says \"" + text + "\" where centred content "
                           + (cancels ? "cancels" : "survives") + " for " + describeState (s));

                // ---- which leg, and which way up ------------------------
                //
                // Everything above this point reads the words as four coarse
                // categories: mix, silent, mono, difference. All four are
                // insensitive to left against right and to how many legs are
                // involved, so every leg name in `describe` could be
                // transposed and this section passed. That is the one thing
                // the function exists to get right, and the one fault it has
                // already shipped was of exactly that shape.
                //
                // So the leg the words name is checked against the leg the
                // matrix actually carries, derived from the coefficients
                // rather than from the buttons: a leg is heard when it
                // reaches either speaker at all, and it is upside down when
                // it reaches one with a negative coefficient.
                const bool leftHeard  = m.leftFromLeft  != 0.0 || m.rightFromLeft  != 0.0;
                const bool rightHeard = m.leftFromRight != 0.0 || m.rightFromRight != 0.0;

                const bool leftFlipped  = m.leftFromLeft  < 0.0 || m.rightFromLeft  < 0.0;
                const bool rightFlipped = m.leftFromRight < 0.0 || m.rightFromRight < 0.0;

                // A leg reaches the two speakers with one coefficient or with
                // two equal ones, so it cannot arrive upright at one and
                // upside down at the other. Asserted because the check below
                // reads a single polarity per leg and would be meaningless if
                // that were not true.
                check (! (m.leftFromLeft > 0.0 && m.rightFromLeft < 0.0)
                    && ! (m.leftFromLeft < 0.0 && m.rightFromLeft > 0.0),
                       "the left leg arrives at the two speakers with opposite signs for "
                           + describeState (s));
                check (! (m.leftFromRight > 0.0 && m.rightFromRight < 0.0)
                    && ! (m.leftFromRight < 0.0 && m.rightFromRight > 0.0),
                       "the right leg arrives at the two speakers with opposite signs for "
                           + describeState (s));

                const bool saysLeftOnly  = text.find ("left leg only")  != std::string::npos;
                const bool saysRightOnly = text.find ("right leg only") != std::string::npos;

                if (saysLeftOnly)
                    check (leftHeard && ! rightHeard,
                           "describe says \"" + text + "\" where the left leg is "
                               + (leftHeard ? "heard" : "not heard") + " and the right leg is "
                               + (rightHeard ? "heard" : "not heard") + " for " + describeState (s));

                if (saysRightOnly)
                    check (rightHeard && ! leftHeard,
                           "describe says \"" + text + "\" where the right leg is "
                               + (rightHeard ? "heard" : "not heard") + " and the left leg is "
                               + (leftHeard ? "heard" : "not heard") + " for " + describeState (s));

                // A named cut is the mirror of a named survivor: the leg the
                // words cut must be the one that is gone, and the other must
                // still be there, or the reading is of a different state.
                if (text.find ("left leg cut") != std::string::npos)
                    check (! leftHeard && rightHeard,
                           "describe says \"" + text + "\" where the left leg is "
                               + (leftHeard ? "heard" : "not heard") + " and the right leg is "
                               + (rightHeard ? "heard" : "not heard") + " for " + describeState (s));

                if (text.find ("right leg cut") != std::string::npos)
                    check (! rightHeard && leftHeard,
                           "describe says \"" + text + "\" where the right leg is "
                               + (rightHeard ? "heard" : "not heard") + " and the left leg is "
                               + (leftHeard ? "heard" : "not heard") + " for " + describeState (s));

                // ---- a polarity invert is never dropped -----------------
                //
                // This is the one property in this file that is about the
                // words rather than about the sound, and it is here because
                // of what the sound does not do. A cut goes silent, a dim
                // goes quiet and a fold collapses the image, so all three
                // announce themselves the moment they are pressed. A
                // polarity invert announces nothing: both legs flipped is
                // inaudible on the speaker pair, and one leg flipped is
                // inaudible on anything centred until something else joins
                // it. `isNeutral` argues that case at length and reports it.
                //
                // So for that one control these words are the only report
                // there is, and two states that differ only by a polarity
                // reaching a speaker must not read the same. Counted off the
                // matrix, so the words have to follow the signal rather than
                // the buttons: a flipped leg that is cut is not flipped at
                // the speaker and must not be mentioned.
                const int flipped = (leftHeard  && leftFlipped  ? 1 : 0)
                                  + (rightHeard && rightFlipped ? 1 : 0);

                // The difference reading counts as naming it: a difference
                // listen is a statement that one leg is upside down, which is
                // the whole of what it means.
                const bool saysFlipped = text.find ("invert") != std::string::npos
                                      || text.find ("difference") != std::string::npos;

                check (saysFlipped == (flipped > 0),
                       "describe says \"" + text + "\" where " + std::to_string (flipped)
                           + " of the legs reaching a speaker are upside down, for "
                           + describeState (s));

                // And where the words count the flipped legs, the count has
                // to be the right one. Without this, relaxing the two legged
                // reading to anything that fires on one leg would go
                // unnoticed, because the check above only asks whether a
                // flip is mentioned at all.
                if (text.find ("both legs inverted") != std::string::npos)
                    check (flipped == 2,
                           "describe says \"" + text + "\" where " + std::to_string (flipped)
                               + " leg(s) are upside down, for " + describeState (s));

                if (text.find ("one leg inverted") != std::string::npos)
                    check (flipped == 1,
                           "describe says \"" + text + "\" where " + std::to_string (flipped)
                               + " leg(s) are upside down, for " + describeState (s));

                // A flip pinned to a named leg has to be pinned to the leg
                // that is really upside down, which is the leg name check
                // again in the one reading that carries both a cut and a
                // polarity.
                if (text.find ("left leg inverted") != std::string::npos)
                    check (leftHeard && leftFlipped,
                           "describe says \"" + text + "\" where the left leg is "
                               + (leftHeard ? "heard" : "not heard") + " and "
                               + (leftFlipped ? "flipped" : "upright") + " for " + describeState (s));

                if (text.find ("right leg inverted") != std::string::npos)
                    check (rightHeard && rightFlipped,
                           "describe says \"" + text + "\" where the right leg is "
                               + (rightHeard ? "heard" : "not heard") + " and "
                               + (rightFlipped ? "flipped" : "upright") + " for " + describeState (s));
            }

        std::printf ("  neutral:   %s\n", describe (State {}));

        State silent;
        silent.cutLeft = silent.cutRight = true;
        std::printf ("  both cut:  %s\n", describe (silent));

        State diff;
        diff.mono = true;
        diff.invertRight = true;
        std::printf ("  difference: %s\n", describe (diff));
    }

    // -----------------------------------------------------------------------
    // The one output channel case.
    //
    // A device with a single output has one leg, and the engine has a separate
    // figure for it rather than the matrix with a missing column, because
    // every way of filling that column in is wrong in a way that would not
    // announce itself: the missing leg as silence makes the fold a 6 dB drop
    // for nothing, and as a copy of the leg that is there makes a cut of the
    // absent leg a 6 dB drop instead.
    //
    // So the properties are: the leg's own cut, polarity and dim all apply,
    // and nothing about the other leg or about the fold reaches it. Asserted
    // over the whole enumeration rather than on a handful of cases, because
    // the fault would be the fold or the other leg leaking in on some
    // combination rather than on all of them.
    std::printf ("the one output channel case, over %d x %d states\n",
                 64, (int) amounts.size());
    {
        for (double db : amounts)
        {
            for (int bits = 0; bits < 64; ++bits)
            {
                const State s = stateFromBits (bits, db);
                const double got = oneChannelGain (s);

                // Its own leg, and only its own leg.
                const double want = s.cutLeft ? 0.0
                                              : (s.invertLeft ? -dimGain (s) : dimGain (s));

                checkExact (got, want,
                            "the one channel gain is the left leg's own for " + describeState (s));

                // The fold has nothing to fold, so engaging it must not move
                // this at all. That is the whole claim and it is the one a
                // matrix with a missing column would break.
                State folded = s;
                folded.mono = ! folded.mono;
                checkExact (oneChannelGain (folded), got,
                            "the fold does not move the one channel gain for " + describeState (s));

                // Nor must anything about the leg that is not there.
                State other = s;
                other.cutRight    = ! other.cutRight;
                other.invertRight = ! other.invertRight;
                checkExact (oneChannelGain (other), got,
                            "the absent leg does not move the one channel gain for "
                                + describeState (s));

                // It cannot overflow either, which is the same property the
                // matrix has and is worth asserting separately because this
                // path does not go through the matrix at all.
                check (std::abs (got) <= 1.0,
                       "the one channel gain is inside unity for " + describeState (s));
                check (std::isfinite (got),
                       "the one channel gain is finite for " + describeState (s));
            }
        }
    }

    // -----------------------------------------------------------------------
    // The word the latches travel to the audio thread as.
    //
    // Six booleans packed into six bits by hand and unpacked by hand is twelve
    // chances to transpose a pair, and a pair transposed in the unpack alone
    // means a studio that works with the speakers swapped under one button.
    // That is why the pack and the unpack are in the header and not in the
    // engine, where nothing could reach them.
    //
    // The round trip is the check that catches it. Note what it cannot catch
    // and does not need to: a transposition made in both directions composes
    // to the identity, so it is not a fault at all, since nothing but these
    // two functions ever sees the bits.
    std::printf ("the packed word, over %d x %d states\n", 64, (int) amounts.size());
    {
        std::vector<unsigned> words;

        for (int bits = 0; bits < 64; ++bits)
        {
            const State s = stateFromBits (bits, defaultDimDb);
            const State back = unpack (pack (s), s.dimDb);

            check (back.mono        == s.mono
                && back.dim         == s.dim
                && back.cutLeft     == s.cutLeft
                && back.cutRight    == s.cutRight
                && back.invertLeft  == s.invertLeft
                && back.invertRight == s.invertRight,
                   "the packed word round trips for " + describeState (s));

            checkExact (back.dimDb, s.dimDb,
                        "the amount travels beside the word for " + describeState (s));

            // And the matrix is the same either side of the trip, which is the
            // thing that actually has to survive it.
            const auto a = matrixFor (s);
            const auto b = matrixFor (back);
            check (a.leftFromLeft == b.leftFromLeft && a.leftFromRight == b.leftFromRight
                && a.rightFromLeft == b.rightFromLeft && a.rightFromRight == b.rightFromRight,
                   "the matrix survives the packed word for " + describeState (s));

            words.push_back (pack (s));
        }

        // Every state has to get its own word. Two latches sharing a bit would
        // round trip one of them and lose the other, and the round trip above
        // would pass for whichever one won.
        std::sort (words.begin(), words.end());
        check (std::adjacent_find (words.begin(), words.end()) == words.end(),
               "the 64 states pack to 64 distinct words");

        for (double db : amounts)
        {
            // The amount travels beside the word and is not touched by it, so
            // whatever came in comes out, a stored NaN included. Compared this
            // way rather than exactly because a NaN is not equal to itself,
            // and the claim here is that nothing happened to it: bringing it
            // inside the range is clampDimDb's job and is checked there.
            const State s = stateFromBits (0, db);
            const double back = unpack (pack (s), s.dimDb).dimDb;

            check ((back == db) || (std::isnan (back) && std::isnan (db)),
                   "the word carries no part of the amount for " + allDigits (db));
        }
    }

    // -----------------------------------------------------------------------
    // The state as one speaker leg can express it.
    //
    // A device with one output channel has one leg, so the fold has nothing to
    // fold and the second leg's cut and polarity have nothing to act on. The
    // reduction is a function rather than a branch in the engine because the
    // arithmetic, the words on the strip and the indicator all have to be
    // asking about the same state: an audit found the version where they were
    // not, with mono lighting the "this is not the mix" indicator and changing
    // nothing, and "mono, right leg only" printed over a silent speaker.
    std::printf ("the state as one leg can express it, over %d x %d states\n",
                 64, (int) amounts.size());
    {
        for (double db : amounts)
        {
            for (int bits = 0; bits < 64; ++bits)
            {
                const State s = stateFromBits (bits, db);

                // Two legs or more, and nothing is reduced at all. Zero legs
                // is the stereo reading too, deliberately: there is no feed to
                // describe, so describing the buttons is the least surprising
                // thing to do with them.
                for (int legs : { 0, 2, 3, 6 })
                {
                    const State full = asHeard (s, legs);
                    check (full.mono == s.mono && full.dim == s.dim
                        && full.cutLeft == s.cutLeft && full.cutRight == s.cutRight
                        && full.invertLeft == s.invertLeft
                        && full.invertRight == s.invertRight
                        && ((full.dimDb == s.dimDb) || (std::isnan (full.dimDb) && std::isnan (s.dimDb))),
                           "a feed of " + std::to_string (legs) + " legs reduces nothing for "
                               + describeState (s));
                }

                const State one = asHeard (s, 1);

                check (! one.mono && ! one.cutRight && ! one.invertRight,
                       "one leg drops the fold and the absent leg for " + describeState (s));
                check (one.cutLeft == s.cutLeft && one.invertLeft == s.invertLeft
                    && one.dim == s.dim,
                       "one leg keeps its own cut, polarity and dim for " + describeState (s));

                // Reducing twice is reducing once, which is what makes it safe
                // to apply wherever the question is asked.
                const State twice = asHeard (one, 1);
                check (pack (twice) == pack (one),
                       "the reduction is idempotent for " + describeState (s));

                // The one leg really is one coefficient: nothing can put
                // anything on the off diagonal of a reduced state, so the
                // engine applying leftFromLeft alone is applying the whole of
                // it.
                const auto m = matrixFor (one);
                checkExact (m.leftFromRight, 0.0,
                            "a reduced state has no crossfeed for " + describeState (s));
                checkExact (oneChannelGain (s), m.leftFromLeft,
                            "the one channel gain is the reduced matrix for "
                                + describeState (s));

                // And the words and the indicator, asked about the reduced
                // state, agree with it. This is the fault the audit found,
                // stated as the property that forbids it: on one leg, the
                // indicator lights exactly when that leg is not carrying the
                // mix.
                const bool neutralOne = isNeutral (one);
                checkExact (m.leftFromLeft, neutralOne ? 1.0 : m.leftFromLeft,
                            "a neutral one leg state passes its leg through for "
                                + describeState (s));
                check (neutralOne == (m.leftFromLeft == 1.0),
                       "the indicator on one leg follows the leg for " + describeState (s));

                const std::string words = describe (one);
                check ((words.find ("mono") != std::string::npos) == false,
                       "nothing on one leg is described as a fold, said \"" + words
                           + "\" for " + describeState (s));
                check ((words.find ("difference") != std::string::npos) == false,
                       "nothing on one leg is described as a difference, said \"" + words
                           + "\" for " + describeState (s));

                // Nor is the leg that is not there ever named. The fault the
                // reduction exists to prevent was a reading about the right
                // leg on a device that has no right leg, so the absence of
                // that word is the property, asserted directly rather than
                // left to follow from the three latches being cleared.
                check (words.find ("right") == std::string::npos,
                       "a one leg reading names the leg that is not there, said \"" + words
                           + "\" for " + describeState (s));

                // And the polarity rule holds here too, against the one
                // coefficient that is actually applied rather than against a
                // matrix with a column nothing reads. A leg arriving upside
                // down is as inaudible on one speaker as it is on two, and on
                // one speaker this line is even more alone: the fold, the cut
                // of the absent leg and its polarity button are all greyed
                // out, so there is less on the panel to read it off.
                const bool flippedOne = m.leftFromLeft < 0.0;

                check ((words.find ("invert") != std::string::npos) == flippedOne,
                       "a one leg reading says \"" + words + "\" where the leg arrives "
                           + (flippedOne ? "upside down" : "upright") + " for "
                           + describeState (s));
            }
        }
    }

    if (failures == 0)
        std::printf ("ControlRoomTest: all checks passed\n");
    else
        std::printf ("ControlRoomTest: %d check(s) failed\n", failures);

    return failures == 0 ? 0 : 1;
}
