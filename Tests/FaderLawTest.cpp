// ---------------------------------------------------------------------------
// Checks what a mixer fader's travel means, and what happens to the travel
// saved by every project written before it had a law.
//
// This is the shape of arithmetic this project keeps getting caught by: a
// fader law a decibel out does not error, does not crackle and looks entirely
// normal on a meter, and a migration that is wrong moves every fader in every
// project anybody has ever saved. Nothing downstream can announce either.
//
// So the properties below are the law's specification asserted directly,
// rather than the header's formulas typed out a second time. Recomputing
// `p * 40 - 30` here and comparing it against `positionToDb` would pass
// whatever the eight segment constants were, which is the one thing a test of
// this cannot afford.
//
// Four checks carry most of the weight:
//
//   * The eleven ticks printed on the X32's channel strip. That is the whole
//     published specification, every position on it is an exact dyadic
//     fraction so there is no tolerance question on the input side, and it is
//     the check that would have caught the law this replaces, whose unity sat
//     at 0.640 of the travel. It is asserted in all three directions: the dB
//     a position means, the gain it gives, and the position a gain comes back
//     at.
//   * The migration preserves gain, not travel fraction. Going through the
//     fraction of the travel looks right and is a silent disaster, because
//     old unity is 0.640 of the travel and new unity is 0.75, so every fader
//     in every existing project would come back 4 dB down. Asserted as an
//     exact equality at the two points where the two routes disagree, and as
//     a decibel comparison against the old formula over half a million
//     randomised positions.
//   * Both round trips, randomised. The forward direction branches on
//     position and the inverse branches on decibels, so a test that only
//     checks one direction cannot see a wrong breakpoint in the other.
//   * The three breakpoints, approached from both sides one double at a time.
//     A single wrong segment constant puts a jump of tens of decibels at one
//     position and leaves everything else correct, so a sweep on any grid
//     coarser than the floating point one steps straight past it.
//
// One thing about generating the random decibel figures is worth reading
// before trusting any number in here, and it is noted again at the generator
// itself. Drawing them with a uniform distribution over the whole -90 .. +10
// range makes the decibel round trip look bit exact, and it is not: the
// affine map inside the distribution subtracts two numbers near 90 to reach a
// figure near zero, so every value it can produce near zero is a multiple of
// a coarse step, and on those the law's arithmetic happens to be exact for a
// reason that has nothing to do with the law. Drawn per segment instead, the
// round trip measures 4.44e-15 and two thirds of the cases are inexact. A
// measurement with its own floor, exactly as this project keeps finding.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include "FaderLaw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <vector>

static int failures = 0;

// Capped, because a property checked over half a million positions can fail
// half a million times and the first few lines are the only ones anybody
// reads. The count stays complete and is what the exit code comes from.
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

// Relative, for gains. A gain spans five decades across the travel, so one
// absolute tolerance is either meaningless at the top or unmeetable at the
// bottom.
static void checkCloseRelative (double actual, double expected, double tolerance, const std::string& what)
{
    const double scale = std::abs (expected) > 0.0 ? std::abs (expected) : 1.0;

    if (! (std::abs (actual - expected) / scale <= tolerance))
        fail (what + ": got " + std::to_string (actual)
                   + ", expected " + std::to_string (expected)
                   + " within a relative " + std::to_string (tolerance));
}

//==============================================================================
// The published specification: the eleven figures printed down the channel
// strip of the X32 operation manual, with the position each sits at and the
// gain it means. The gains are written out as literals rather than computed
// with `pow`, so that a change to the law has to disagree with a number
// somebody can read off a photograph of a real console.
struct Tick
{
    double db;
    double position;
    double gain;
};

static const Tick ticks[] =
{
    {  10.0, 1.0,    3.16227766016837933 },
    {   5.0, 0.875,  1.77827941003892280 },
    {   0.0, 0.75,   1.0                 },
    {  -5.0, 0.625,  0.56234132519034908 },
    { -10.0, 0.5,    0.31622776601683794 },
    { -20.0, 0.375,  0.1                 },
    { -30.0, 0.25,   0.03162277660168379 },
    { -40.0, 0.1875, 0.01                },
    { -50.0, 0.125,  0.00316227766016838 },
    { -60.0, 0.0625, 0.001               }
    // The eleventh row, -inf at position 0, is checked on its own below: its
    // gain is exactly zero and its decibel reading is not a number that can
    // be compared to anything.
};

static const int numTicks = (int) (sizeof (ticks) / sizeof (ticks[0]));

// The three positions where the slope changes, which are the only places an
// inclusive-on-one-side boundary can show.
static const double breakpoints[] = { 0.0625, 0.25, 0.5 };

// The same three in decibels, which is what the inverse branches on.
static const double dbBreakpoints[] = { -60.0, -30.0, -10.0 };

//==============================================================================
// Generating a position or a decibel figure.
//
// Both are drawn within a randomly chosen segment as well as across the whole
// travel, and the reason is the one in the header comment above: a uniform
// draw over the full -90 .. +10 range cannot produce a full precision double
// near zero, because the distribution reaches one by subtracting two numbers
// near ninety. Every value it gives near zero is a multiple of a step about
// five hundred times coarser than the spacing of doubles there, and the law's
// arithmetic is exact on all of them. Drawing inside one segment at a time
// removes the cancellation and the inexact cases appear.
static const double positionEdges[] = { 0.0, 0.0625, 0.25, 0.5, 1.0 };
static const double dbEdges[]       = { -90.0, -60.0, -30.0, -10.0, 10.0 };

static double randomPosition (std::mt19937_64& rng)
{
    std::uniform_int_distribution<int> which (0, 4);
    const int segment = which (rng);

    if (segment == 4)
        return std::uniform_real_distribution<double> (0.0, 1.0) (rng);

    return std::uniform_real_distribution<double> (positionEdges[segment],
                                                   positionEdges[segment + 1]) (rng);
}

static double randomDb (std::mt19937_64& rng)
{
    std::uniform_int_distribution<int> which (0, 4);
    const int segment = which (rng);

    if (segment == 4)
        return std::uniform_real_distribution<double> (-89.99, 10.0) (rng);

    return std::uniform_real_distribution<double> (dbEdges[segment],
                                                   dbEdges[segment + 1]) (rng);
}

//==============================================================================
int main()
{
    using namespace FaderLaw;

    std::mt19937_64 rng (20261027);

    const double ceilingGain = std::pow (10.0, ceilingDb / 20.0);
    const double floorGain   = std::pow (10.0, floorDb / 20.0);

    // ---- the printed scale -------------------------------------------------
    std::printf ("the eleven figures printed on the channel strip\n");

    {
        double worstDb       = 0.0;
        double worstGain     = 0.0;
        double worstPosition = 0.0;

        for (int i = 0; i < numTicks; ++i)
        {
            const Tick& t = ticks[i];
            const std::string at = " at the " + std::to_string ((int) t.db) + " dB tick";

            // Exact, and it can be: every tick position is a dyadic fraction
            // and the forward law is one multiply and one subtract, so there
            // is nothing here for a rounding to happen in. A tolerance would
            // be hiding the strongest assertion in the file.
            checkExact (positionToDb (t.position), t.db, "the printed decibel figure" + at);
            worstDb = std::max (worstDb, std::abs (positionToDb (t.position) - t.db));

            checkCloseRelative (positionToGain (t.position), t.gain, 1.0e-15,
                                "the gain" + at);
            worstGain = std::max (worstGain, std::abs (positionToGain (t.position) - t.gain) / t.gain);

            // The inverse from the published gain rather than from the gain
            // the law just produced, so that a law and its inverse cannot be
            // wrong together and still agree with each other.
            checkClose (gainToPosition (t.gain), t.position, 1.0e-14,
                        "the position a published gain comes back at" + at);
            worstPosition = std::max (worstPosition, std::abs (gainToPosition (t.gain) - t.position));

            checkClose (dbToPosition (t.db), t.position, 1.0e-14,
                        "the position a printed decibel figure comes back at" + at);
        }

        std::printf ("  worst over the ten finite ticks: %.3e dB, %.3e relative gain, %.3e position\n",
                     worstDb, worstGain, worstPosition);

        // The eleventh row. The gain is exactly nothing, by its own branch and
        // not as the limit of a formula, which is the whole point of it.
        checkExact (positionToGain (0.0), 0.0, "the bottom of the travel is not exact silence");
        checkExact (gainToPosition (0.0), 0.0, "silence does not come back as the bottom of the travel");
        checkExact (positionToDb (0.0), floorDb,
                    "the bottom of the travel should read as the floor rather than as minus infinity");

        // Unity, which is the one figure this law was chosen for. Exact,
        // because the interface draws a double click return to it and the
        // migration lands every old unity fader on it.
        checkExact (positionToGain (unityPosition), 1.0, "unity is not unity gain");
        checkExact (positionToDb (unityPosition), 0.0, "unity is not 0 dB");
        checkExact (unityPosition, 0.75, "unity has moved off three quarters of the travel");
        checkExact (dbToPosition (0.0), unityPosition, "0 dB is not at the unity position");

        // The ends of the travel against the two named constants, so that the
        // numbers the interface draws the scale from are the numbers the law
        // actually produces.
        checkExact (positionToDb (maxPosition), ceilingDb, "the top of the travel is not the ceiling");
        checkCloseRelative (positionToGain (maxPosition), ceilingGain, 1.0e-15,
                            "the gain at the top of the travel");
        checkExact (dbToPosition (ceilingDb), maxPosition, "the ceiling is not at the top of the travel");
        checkExact (minPosition, 0.0, "the bottom of the travel has moved off zero");
    }

    // ---- the printed ticks are equally spaced within each segment ---------
    std::printf ("the printed ticks are equally spaced in travel within each segment\n");

    // This is the header's entire sourcing argument, asserted rather than
    // trusted. Behringer does not publish the law; what it publishes is the
    // artwork, and the reason the artwork implies a piecewise linear in
    // decibels law is that its ticks are evenly spaced within a segment and
    // change spacing at -10 and -30. A smooth law cannot do that. If this
    // ever stops holding, the claim in the header that the segments are
    // derived from the manufacturer's own scale has stopped being true.
    {
        // Grouped by segment, as indices into the tick table.
        const std::vector<std::vector<int>> groups
        {
            { 0, 1, 2, 3, 4 },      // +10 .. -10, the top segment
            { 4, 5, 6 },            // -10 .. -30
            { 6, 7, 8, 9 }          // -30 .. -60
        };

        for (const auto& group : groups)
        {
            const double spacing = ticks[group[0]].position - ticks[group[1]].position;

            for (std::size_t i = 1; i + 1 < group.size(); ++i)
                checkExact (ticks[group[i]].position - ticks[group[i + 1]].position, spacing,
                            "the printed ticks are not evenly spaced between "
                                + std::to_string ((int) ticks[group[i]].db) + " and "
                                + std::to_string ((int) ticks[group[i + 1]].db) + " dB");

            // And the law has to agree that they are: the spacing above is
            // read off the table, this reads it off the law.
            for (std::size_t i = 0; i + 1 < group.size(); ++i)
                checkExact (dbToPosition (ticks[group[i]].db) - dbToPosition (ticks[group[i + 1]].db),
                            spacing,
                            "the law does not space the printed ticks evenly between "
                                + std::to_string ((int) ticks[group[i]].db) + " and "
                                + std::to_string ((int) ticks[group[i + 1]].db) + " dB");
        }

        // The spacing changes between segments, which is the other half of the
        // claim. A law with one spacing everywhere is a straight line in
        // decibels over the whole travel and is not this one.
        const double topSpacing    = ticks[0].position - ticks[1].position;   // 5 dB of travel
        const double bottomSpacing = ticks[6].position - ticks[7].position;   // 10 dB of travel

        check (bottomSpacing < topSpacing,
               "ten decibels at the bottom of the scale should take less travel than five at the top");
    }

    // ---- piecewise linear in decibels, in four segments -------------------
    std::printf ("the law is affine in decibels inside each segment, and steeper below\n");

    // The defining shape of the law, independent of what its constants are: a
    // position to decibel map that is exactly affine between breakpoints, with
    // four distinct slopes that increase downward. Checked by midpoints rather
    // than by differentiating, because an affine function is the only one whose
    // value at the midpoint of any interval is the mean of its ends.
    {
        std::vector<double> slopes;

        for (int segment = 0; segment < 4; ++segment)
        {
            const double lo = positionEdges[segment];
            const double hi = positionEdges[segment + 1];

            // Kept strictly inside the segment at the bottom end, because the
            // very bottom of the travel is silence by its own branch.
            const double a = lo + (hi - lo) * 0.125;
            const double b = lo + (hi - lo) * 0.875;

            for (int step = 0; step <= 64; ++step)
            {
                const double t  = (double) step / 64.0;
                const double x1 = a + (b - a) * t * 0.5;
                const double x2 = b - (b - a) * t * 0.5;
                const double mid = (x1 + x2) * 0.5;

                checkClose (positionToDb (mid),
                            (positionToDb (x1) + positionToDb (x2)) * 0.5, 1.0e-12,
                            "the law bends inside segment " + std::to_string (segment));
            }

            slopes.push_back ((positionToDb (b) - positionToDb (a)) / (b - a));
        }

        std::printf ("  decibels per full travel, bottom segment up: %.4f %.4f %.4f %.4f\n",
                     slopes[0], slopes[1], slopes[2], slopes[3]);

        for (std::size_t i = 0; i + 1 < slopes.size(); ++i)
            check (slopes[i] > slopes[i + 1] * 1.5,
                   "segment " + std::to_string (i) + " should be markedly steeper than the one above it");
    }

    // ---- the breakpoints ---------------------------------------------------
    std::printf ("the segments meet at the breakpoints, approached one double at a time\n");

    // The asymmetry worth being careful about: the forward direction branches
    // on position and the inverse branches on decibels, so a boundary made
    // inclusive on one side and exclusive on the other is silent everywhere
    // except exactly at the breakpoint. A single wrong constant puts a jump of
    // tens of decibels at one position and nothing wrong anywhere else, which
    // a sweep on any grid a human would write down walks straight past. So the
    // breakpoints are approached by `nextafter`, which is the only grid fine
    // enough to be sure.
    {
        double worst = 0.0;

        for (double bp : breakpoints)
        {
            const double below = std::nextafter (bp, -1.0);
            const double above = std::nextafter (bp,  2.0);

            const double atBp   = positionToDb (bp);
            const double fromLo = positionToDb (below);
            const double fromHi = positionToDb (above);

            checkClose (fromLo, atBp, 1.0e-12,
                        "the segment below the breakpoint at " + std::to_string (bp)
                            + " does not meet it");
            checkClose (fromHi, atBp, 1.0e-12,
                        "the segment above the breakpoint at " + std::to_string (bp)
                            + " does not meet it");

            worst = std::max (worst, std::max (std::abs (fromLo - atBp), std::abs (fromHi - atBp)));

            // The sharpest form of the inclusive-or-exclusive check: whichever
            // branch each function takes at the breakpoint, the two have to
            // compose to the identity there exactly.
            checkExact (dbToPosition (positionToDb (bp)), bp,
                        "the breakpoint at " + std::to_string (bp) + " does not survive a round trip");
        }

        for (double bp : dbBreakpoints)
        {
            const double below = std::nextafter (bp, -1000.0);
            const double above = std::nextafter (bp,  1000.0);

            const double atBp = dbToPosition (bp);

            checkClose (dbToPosition (below), atBp, 1.0e-12,
                        "the inverse's segment below " + std::to_string (bp) + " dB does not meet it");
            checkClose (dbToPosition (above), atBp, 1.0e-12,
                        "the inverse's segment above " + std::to_string (bp) + " dB does not meet it");

            checkExact (positionToDb (dbToPosition (bp)), bp,
                        "the decibel breakpoint at " + std::to_string (bp)
                            + " does not survive a round trip");
        }

        std::printf ("  worst discrepancy across a breakpoint: %.3e dB\n", worst);
    }

    // ---- both round trips --------------------------------------------------
    std::printf ("both round trips over 500000 randomised cases each\n");

    {
        double worstPosition = 0.0;
        double worstPositionAt = 0.0;

        for (int n = 0; n < 500000; ++n)
        {
            const double p = randomPosition (rng);

            if (p <= 0.0)
                continue;

            const double back = gainToPosition (positionToGain (p));
            const double err  = std::abs (back - p);

            if (err > worstPosition)
            {
                worstPosition   = err;
                worstPositionAt = p;
            }
        }

        // Within a segment this law is affine in decibels, so it inverts to a
        // floating point unit by construction. That is the property the header
        // picked it for over Ardour's matched pair of transcendentals, so the
        // tolerance is set where a law that had to go through `pow` both ways
        // would not fit.
        checkClose (worstPosition, 0.0, 1.0e-14,
                    "the position round trip is loose, worst at p = "
                        + std::to_string (worstPositionAt));

        double worstDb   = 0.0;
        double worstDbAt = 0.0;
        long   inexact   = 0;

        for (int n = 0; n < 500000; ++n)
        {
            const double db   = randomDb (rng);
            const double back = positionToDb (dbToPosition (db));
            const double err  = std::abs (back - db);

            if (err != 0.0)
                ++inexact;

            if (err > worstDb)
            {
                worstDb   = err;
                worstDbAt = db;
            }
        }

        checkClose (worstDb, 0.0, 1.0e-13,
                    "the decibel round trip is loose, worst at " + std::to_string (worstDbAt) + " dB");

        // Printed rather than asserted, and printed on purpose. Drawn with one
        // uniform distribution over the whole range this count is zero and the
        // round trip looks bit exact. It is not: the distribution cannot
        // produce a full precision double near zero, because it reaches one by
        // subtracting two numbers near ninety. Anybody tightening the
        // tolerance above to exact equality should see this number first.
        std::printf ("  position round trip %.3e, decibel round trip %.3e (%ld of 500000 inexact)\n",
                     worstPosition, worstDb, inexact);

        // And the same thing starting from a gain, because that is what the
        // migration does and what the engine would do if it ever had to read a
        // fader back out of a gain.
        double worstGain = 0.0;

        for (int n = 0; n < 200000; ++n)
        {
            const double g = positionToGain (randomPosition (rng));

            if (g <= 0.0)
                continue;

            worstGain = std::max (worstGain, std::abs (positionToGain (gainToPosition (g)) - g) / g);
        }

        checkClose (worstGain, 0.0, 1.0e-13, "the gain round trip is loose");
        std::printf ("  gain round trip, relative: %.3e\n", worstGain);
    }

    // ---- exact silence, and only there ------------------------------------
    std::printf ("silence is position zero and nothing else\n");

    {
        checkExact (positionToGain (0.0), 0.0, "position zero is not exact silence");
        checkExact (positionToGain (-0.0), 0.0, "negative zero is not exact silence");

        // Anything above the bottom of the travel has to make a sound. The
        // smallest representable positive double is in here because that is
        // where an arithmetic floor, or a `pow` that underflowed, would show,
        // and the figure it gives has to be the floor of the law rather than
        // nothing or a not-a-number.
        const double tiny[] = { std::numeric_limits<double>::denorm_min(),
                                std::numeric_limits<double>::min(),
                                1.0e-300, 1.0e-20, 1.0e-10, 1.0e-6, 1.0e-3 };

        for (double x : tiny)
        {
            const double g = positionToGain (x);

            check (g > 0.0, "a position above the bottom of the travel gave exact silence");
            check (std::isfinite (g), "a position above the bottom of the travel gave a gain that is not finite");
            check (g >= floorGain * (1.0 - 1.0e-12),
                   "a position just above the bottom of the travel is below the law's own floor");
            check (g <= ceilingGain, "a tiny position gave a gain above the ceiling");
        }

        // At the very bottom the law has to land on its floor rather than
        // somewhere near it, because the floor is the figure the header argues
        // the step down to silence against.
        checkCloseRelative (positionToGain (std::numeric_limits<double>::denorm_min()), floorGain,
                            1.0e-12, "the smallest positive position is not at the law's floor");

        // The step itself, stated as the number the header's argument rests
        // on: the jump from the bottom of the continuous travel down to
        // silence has to be of the order of the least significant bit of a 16
        // bit file, which is what makes it inaudible in anything the studio
        // can deliver.
        //
        // Asserted as "within half a decibel of one LSB" and not as "smaller
        // than one LSB", because smaller is what the header claims and is not
        // true. A 16 bit LSB is 1/32768, which is -90.31 dB, and this law's
        // floor is -90 dB, so the step is 1.036 least significant bits: 0.31
        // dB larger than the bit, not smaller. The argument survives the
        // correction, since a step of one bit at the bottom of a 16 bit file
        // is still below anything a converter resolves, but the sentence in
        // the header does not and is reported rather than accommodated.
        const double sixteenBitLsbDb = 20.0 * std::log10 (1.0 / 32768.0);

        checkClose (floorDb, sixteenBitLsbDb, 0.5,
                    "the step from the bottom of the travel to silence is not of the order of a 16 bit LSB");
        check (floorGain <= 2.0 / 32768.0,
               "the step from the bottom of the travel to silence is more than two 16 bit LSBs");

        std::printf ("  the floor is %.2f dB and a 16 bit LSB is %.2f dB, so the step down to silence is %.3f bits\n",
                     floorDb, sixteenBitLsbDb, floorGain * 32768.0);

        checkExact (gainToPosition (0.0), 0.0, "a gain of zero is not the bottom of the fader");
        checkExact (gainToPosition (-0.0), 0.0, "a gain of negative zero is not the bottom of the fader");

        // A negative gain is nonsense arriving from somewhere, and the one
        // answer it must not give is a position that is not a number: that
        // would survive any later clamp and come back as something plausible.
        for (double g : { -1.0, -1.0e-30, -1.0e30, -0.5 })
        {
            const double p = gainToPosition (g);

            check (! std::isnan (p), "a negative gain gave a position that is not a number");
            checkExact (p, minPosition, "a negative gain is not the bottom of the fader");
        }

        const double nan = std::numeric_limits<double>::quiet_NaN();

        check (! std::isnan (gainToPosition (nan)), "a gain of NaN gave a position of NaN");
        checkExact (gainToPosition (nan), minPosition, "a gain of NaN is not the bottom of the fader");

        // A gain below the floor is inaudible under any law and the fader has
        // nowhere to put it, so it reads as the bottom. Checked either side of
        // the floor so the boundary is pinned rather than implied.
        checkExact (gainToPosition (floorGain * 0.5), minPosition,
                    "a gain below the floor is not the bottom of the fader");
        check (gainToPosition (floorGain * 2.0) > minPosition,
               "a gain above the floor should not read as the bottom of the fader");
    }

    // ---- monotonicity ------------------------------------------------------
    std::printf ("strictly increasing across the whole travel, over 200000 random pairs\n");

    {
        for (int n = 0; n < 200000; ++n)
        {
            double a = randomPosition (rng);
            double b = randomPosition (rng);

            if (a > b)
                std::swap (a, b);

            // Non-decreasing always: a law that doubles back makes the fader
            // move the sound the wrong way somewhere in its travel.
            check (positionToGain (a) <= positionToGain (b),
                   "the gain fell while the fader went up, between " + std::to_string (a)
                       + " and " + std::to_string (b));
            check (positionToDb (a) <= positionToDb (b),
                   "the decibel figure fell while the fader went up");

            // And strictly increasing wherever the two positions are far
            // enough apart for floating point to be able to tell. A thousandth
            // of a decibel is thirty five times the spacing of doubles there,
            // so this is a real claim rather than a tolerance.
            if (b - a > 1.0e-13)
            {
                check (positionToGain (a) < positionToGain (b),
                       "the gain stalled while the fader went up, between " + std::to_string (a)
                           + " and " + std::to_string (b));
                check (positionToDb (a) < positionToDb (b),
                       "the decibel figure stalled while the fader went up");
            }
        }

        // The inverse has to be monotonic too, and in the gain domain, because
        // that is the domain the migration works in.
        for (int n = 0; n < 200000; ++n)
        {
            double a = positionToGain (randomPosition (rng));
            double b = positionToGain (randomPosition (rng));

            if (a > b)
                std::swap (a, b);

            check (gainToPosition (a) <= gainToPosition (b),
                   "the fader went down while the gain went up");

            if (a > floorGain * 2.0 && b > a * (1.0 + 1.0e-9))
                check (gainToPosition (a) < gainToPosition (b),
                       "the fader stalled while the gain went up");
        }

        // Monotonic in decibels as well, which is the branch the inverse
        // actually takes.
        for (int n = 0; n < 200000; ++n)
        {
            double a = randomDb (rng);
            double b = randomDb (rng);

            if (a > b)
                std::swap (a, b);

            check (dbToPosition (a) <= dbToPosition (b),
                   "the fader went down while the decibel figure went up");
        }
    }

    // ---- the taper ---------------------------------------------------------
    std::printf ("the taper, as the statements that make this law recognisable\n");

    // The header rejected "the taper is wrong" as the reason to replace the old
    // law and gave figures for both. These are those figures, asserted, so
    // that a future change to any segment constant has to confront what the
    // law is supposed to feel like rather than only what it measures at the
    // ticks.
    {
        const double lower = dbToPosition (-20.0);
        const double upper = dbToPosition (-6.0);

        // Self validating: the two positions have to be where those decibel
        // figures actually sit before the fraction between them means anything.
        checkExact (positionToDb (lower), -20.0, "the -20 dB point is not at -20 dB");
        checkExact (positionToDb (upper), -6.0,  "the -6 dB point is not at -6 dB");

        checkClose (upper - lower, 0.225, 1.0e-15,
                    "the -20 to -6 dB band does not occupy the share of the travel this law is chosen for");

        std::printf ("  -20 to -6 dB occupies %.6f of the travel\n", upper - lower);

        // The top half of the fader spans exactly twenty decibels. That is one
        // segment, and it is the statement that makes the whole top of this
        // law a single straight line from -10 to the ceiling.
        checkExact (positionToDb (1.0) - positionToDb (0.5), 20.0,
                    "the top half of the travel does not span twenty decibels");

        // And the old law's figure, computed here from its own formula, so the
        // comparison the header makes in prose is a thing the test agrees
        // with. If this stops matching, the header's argument that the taper
        // was never the fault has stopped being checkable.
        const double oldLower = legacyUnityPosition * std::sqrt (std::pow (10.0, -20.0 / 20.0)) / legacyMaxPosition;
        const double oldUpper = legacyUnityPosition * std::sqrt (std::pow (10.0, -6.0 / 20.0)) / legacyMaxPosition;

        checkClose (oldUpper - oldLower, 0.2507, 1.0e-4,
                    "the old law's taper is not the figure the header compares against");

        std::printf ("  the law it replaces occupied %.6f, so the taper was never the fault\n",
                     oldUpper - oldLower);
    }

    // ---- clamping rather than extrapolating -------------------------------
    std::printf ("positions and gains off the ends of the fader\n");

    {
        for (double p : { 1.0 + 1.0e-12, 1.0000001, 1.25, 2.0, 1.0e9, 1.0e300 })
        {
            checkExact (positionToDb (p), ceilingDb, "a position past the top did not saturate in decibels");
            checkExact (positionToGain (p), positionToGain (1.0), "a position past the top did not saturate in gain");
        }

        for (double p : { -1.0e-12, -1.0e-30, -0.5, -1.0, -1.0e9, -1.0e300 })
        {
            checkExact (positionToDb (p), floorDb, "a position below the bottom did not saturate in decibels");
            checkExact (positionToGain (p), 0.0, "a position below the bottom is not silence");
        }

        // A decibel figure or a gain above the ceiling has to come back as the
        // top of the fader. The fader has a top, and a number that cannot be
        // reached on it has to read as the top rather than as something off
        // the end that a later clamp turns into a plausible position.
        for (double db : { 10.0 + 1.0e-12, 10.5, 20.0, 1000.0, 1.0e300 })
            checkExact (dbToPosition (db), maxPosition, "a decibel figure above the ceiling did not saturate");

        for (double g : { ceilingGain, ceilingGain * 1.0000001, 4.0, 100.0, 1.0e30 })
            checkExact (gainToPosition (g), maxPosition, "a gain at or above the ceiling did not saturate");

        for (double db : { -90.0, -90.0000001, -120.0, -1000.0, -1.0e300 })
            checkExact (dbToPosition (db), minPosition, "a decibel figure below the floor did not saturate");

        // Nothing may leave the travel, for any input at all. This is the
        // assertion a missing clamp fails on: without it a decibel figure of
        // +20 reads as position 1.25 and a figure of -120 reads as -0.0625,
        // both of which look like positions and are not.
        for (int n = 0; n < 200000; ++n)
        {
            const double p  = randomPosition (rng) * 4.0 - 2.0;
            const double db = randomDb (rng) * 4.0;
            const double g  = positionToGain (randomPosition (rng)) * 1000.0;

            const double fromDb       = dbToPosition (db);
            const double fromGain     = gainToPosition (g);
            const double dbOfPosition = positionToDb (p);
            const double gainOfP      = positionToGain (p);

            check (fromDb >= minPosition && fromDb <= maxPosition,
                   "a decibel figure of " + std::to_string (db) + " left the travel");
            check (fromGain >= minPosition && fromGain <= maxPosition,
                   "a gain of " + std::to_string (g) + " left the travel");
            check (dbOfPosition >= floorDb && dbOfPosition <= ceilingDb,
                   "a position of " + std::to_string (p) + " read outside the law's own scale");
            check (gainOfP >= 0.0 && gainOfP <= ceilingGain,
                   "a position of " + std::to_string (p) + " gave a gain outside the law's own range");
        }
    }

    // ---- inputs that are not numbers --------------------------------------
    std::printf ("positions and gains that are not numbers\n");

    {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();

        // A position that is not a number comes from a corrupt project file or
        // a control that has gone wrong, and the header's choice is the quiet
        // answer: a fader that has lost its position must not be louder than
        // the one next to it. Asserted as the floor rather than merely as
        // "contained", because a NaN that reaches the audio path silences
        // everything downstream and says nothing about where it came from.
        checkExact (positionToDb (nan), floorDb, "a position of NaN should read as the floor");
        checkExact (positionToGain (nan), 0.0, "a position of NaN should be silent");
        checkExact (positionToDb (-inf), floorDb, "a position of minus infinity should read as the floor");
        checkExact (positionToGain (-inf), 0.0, "a position of minus infinity should be silent");
        checkExact (positionToDb (inf), floorDb,
                    "a position of infinity should read as the floor, per the header's own rule that a "
                    "position that has gone wrong goes quiet rather than loud");
        checkExact (positionToGain (inf), 0.0, "a position of infinity should be silent");

        checkExact (dbToPosition (nan), minPosition, "a decibel figure of NaN should be the bottom of the fader");
        checkExact (dbToPosition (-inf), minPosition, "minus infinity decibels should be the bottom of the fader");
        checkExact (dbToPosition (inf), maxPosition, "plus infinity decibels should be the top of the fader");

        for (double g : { inf, -inf, nan })
        {
            const double p = gainToPosition (g);

            check (! std::isnan (p), "a gain that is not a number gave a position that is not a number");
            check (p >= minPosition && p <= maxPosition, "a gain that is not a number left the travel");
        }

        // The two entry points to the inverse have to agree about an infinite
        // input, which is asserted rather than assumed because they did not.
        // `gainToPosition` guarded on `! std::isfinite` and so lumped an
        // infinity in with a NaN, sending an infinite gain to the bottom while
        // `dbToPosition` sent plus infinity to the top: the same request at
        // opposite ends of the fader. A gain above the ceiling saturates, which
        // is what the header says and what an infinity is a limiting case of,
        // so both now give the top.
        //
        // Asserted against dbToPosition rather than against maxPosition
        // directly, because what is being checked is that the two routes into
        // the inverse answer the same question the same way.
        checkExact (gainToPosition (inf), dbToPosition (inf),
                    "the two entry points to the inverse disagree about an infinite input: an infinite "
                    "gain and an infinite decibel figure are the same request and must land at the same "
                    "end of the fader");
        checkExact (gainToPosition (inf), maxPosition,
                    "an infinite gain is above the ceiling, so it should saturate at the top of the "
                    "fader rather than reading as something off the end of it");
        checkExact (gainToPosition (-inf), minPosition, "a gain of minus infinity is not the bottom of the fader");

        // The migration reads whatever is in the file, so every one of its
        // entry points is checked on every non-finite input rather than on
        // NaN alone. Plus infinity is the one that matters: it is the only
        // value where dropping a non-finite test still leaves a guard that
        // appears to cover the case, because a stray infinity survives
        // `std::min` against the top of the travel and comes out as the top of
        // the fader. A corrupt project file would then open with a track at
        // full, which is the loud failure this header is written to avoid.
        check (! std::isnan (legacyToPosition (nan)), "a legacy position of NaN gave a position of NaN");

        for (double x : { nan, inf, -inf })
        {
            checkExact (legacyToPosition (x), minPosition,
                        "a legacy position that is not a number should be the bottom of the fader");
            checkExact (legacyCurveValueToPosition (x), minPosition,
                        "a legacy curve value that is not a number should be the bottom of the fader");
            checkExact (legacyPositionToGain (x), 0.0,
                        "a legacy position that is not a number should be silent");
        }
    }

    // ---- the migration -----------------------------------------------------
    std::printf ("the migration preserves gain, which is the half that can do real damage\n");

    {
        // The two points where the two possible migrations disagree, and the
        // reason this is the most important assertion in the file after the
        // printed scale. Translating the fraction of the travel looks right:
        // old unity is 0.640 of the old travel and new unity is 0.75, so a
        // fader saved at 0 dB would come back at position 0.640, which is
        // -4 dB. Every track of every existing project would come back
        // quieter by an amount depending on where its fader happened to be,
        // and it would present as "my old mixes all sound wrong" with nothing
        // in the audio path to blame.
        //
        // Exact, not close. A fader resting at unity has to come back resting
        // at unity, because that is the position the interface returns to on a
        // double click and the position every untouched insert is saved at.
        checkExact (legacyToPosition (legacyUnityPosition), unityPosition,
                    "a fader saved at old unity does not come back at new unity");
        checkExact (legacyToPosition (0.8), 0.75,
                    "a fader saved at old unity does not come back at three quarters of the travel");
        checkExact (legacyCurveValueToPosition (0.64), 0.75,
                    "an automation curve drawn at old unity does not come back at new unity");

        // Stated as the wrong answer as well as the right one, so that nobody
        // reading this has to work out what the failure would have been.
        const double throughTravelFraction = legacyUnityPosition / legacyMaxPosition;

        checkClose (throughTravelFraction, 0.64, 1.0e-15,
                    "the old law's unity is not where the header says it was");
        check (legacyToPosition (legacyUnityPosition) != throughTravelFraction,
               "the migration went through the fraction of the travel, so every fader in every "
               "saved project has moved to " + std::to_string (throughTravelFraction)
                   + ", which is " + std::to_string (positionToDb (throughTravelFraction)) + " dB");
        checkClose (positionToDb (throughTravelFraction), -4.4, 0.1,
                    "a travel fraction migration should land near -4 dB");

        // The old law's ceiling, which the header's argument that no existing
        // project loses anything at the top rests on.
        checkCloseRelative (legacyPositionToGain (legacyMaxPosition), 2.44140625, 1.0e-15,
                            "the old law's ceiling is not (1.25 / 0.8) squared");
        checkClose (20.0 * std::log10 (legacyPositionToGain (legacyMaxPosition)), 7.75, 0.005,
                    "the old law's ceiling is not the +7.75 dB the header names");
        check (20.0 * std::log10 (legacyPositionToGain (legacyMaxPosition)) < ceilingDb,
               "an old project at the top of its fader cannot be represented under the new ceiling");
        check (legacyToPosition (legacyMaxPosition) < maxPosition,
               "an old project at the top of its fader lands at the top of the new one, so it was clipped");

        // Where the new law's floor cuts the old travel. Computed from the
        // constants rather than copied from the header's prose, and then
        // compared against that prose, because the figure in a doc comment is
        // the sort of thing that goes stale silently.
        const double floorCrossing = legacyUnityPosition * std::sqrt (floorGain);

        checkClose (floorCrossing, 0.0044987, 1.0e-7,
                    "the floor crossing is not the figure the header documents");
        checkClose (floorCrossing / legacyMaxPosition, 0.0036, 1.0e-4,
                    "the floor crossing is not the bottom 0.36 per cent of the old travel the header documents");

        std::printf ("  the new floor cuts the old travel at %.8f, the bottom %.4f per cent\n",
                     floorCrossing, 100.0 * floorCrossing / legacyMaxPosition);

        // Its own property, so that it cannot be hidden inside the tolerance
        // of the check below. The old law had no floor and this one bottoms at
        // -90 dB, so the bottom third of a per cent of the old travel comes
        // back as exact silence instead of as the inaudible figure it was.
        for (int n = 0; n < 50000; ++n)
        {
            const double v = std::uniform_real_distribution<double> (0.0, floorCrossing) (rng);

            checkExact (legacyToPosition (v), minPosition,
                        "an old position below the new floor should come back as exact silence");
            checkExact (positionToGain (legacyToPosition (v)), 0.0,
                        "an old position below the new floor should come back silent");
        }

        check (legacyToPosition (std::nextafter (floorCrossing, 1.0)) >= minPosition,
               "just above the floor crossing left the travel");
        check (legacyToPosition (floorCrossing * 1.001) > minPosition,
               "an old position above the new floor should not come back as silence");

        // And the whole point: above the floor the translation preserves gain.
        // Compared in decibels against the old formula written out here, not
        // against the header's own `legacyPositionToGain`, so that the formula
        // being preserved is a thing this file asserts rather than a thing it
        // reads back out of the code under test.
        double worst       = 0.0;
        double worstAt     = 0.0;
        long   belowFloor  = 0;

        for (int n = 0; n < 500000; ++n)
        {
            const double v = std::uniform_real_distribution<double> (0.0, legacyMaxPosition) (rng);

            // `(v / 0.8)^2`, exactly as `processInsert` computed it.
            const double was = (v / 0.8) * (v / 0.8);

            checkCloseRelative (legacyPositionToGain (v), was, 1.0e-15,
                                "the old law is not being reproduced at v = " + std::to_string (v));

            if (was <= floorGain)
            {
                ++belowFloor;
                checkExact (positionToGain (legacyToPosition (v)), 0.0,
                            "an old gain below the new floor should migrate to exact silence");
                continue;
            }

            const double now = positionToGain (legacyToPosition (v));

            check (now > 0.0, "an audible old fader migrated to silence at v = " + std::to_string (v));

            const double err = std::abs (20.0 * std::log10 (now) - 20.0 * std::log10 (was));

            if (err > worst)
            {
                worst   = err;
                worstAt = v;
            }
        }

        checkClose (worst, 0.0, 1.0e-12,
                    "the migration does not preserve gain, worst at v = " + std::to_string (worstAt));

        std::printf ("  gain preserved to %.3e dB over 500000 old positions, %ld of them below the new floor\n",
                     worst, belowFloor);

        // A curve value is the same translation with the old travel folded in,
        // so it has to agree with going the long way round. A curve stored
        // under the old law was multiplied by 1.25 to reach a position, and
        // getting that factor wrong would leave every drawn fade pointing
        // somewhere else.
        for (int n = 0; n < 200000; ++n)
        {
            const double c = std::uniform_real_distribution<double> (0.0, 1.0) (rng);

            checkExact (legacyCurveValueToPosition (c), legacyToPosition (c * legacyMaxPosition),
                        "a legacy curve value does not translate the same way as the position it meant");
        }

        // A curve is stored 0 .. 1, so anything above the top of it is the top
        // of it, and the ends have to land on the ends of the new travel.
        checkExact (legacyCurveValueToPosition (0.0), minPosition, "a curve at the bottom is not the bottom");
        checkExact (legacyCurveValueToPosition (1.0), legacyToPosition (legacyMaxPosition),
                    "a curve at the top is not the top of the old travel");

        for (double c : { 1.0 + 1.0e-12, 1.5, 2.0, 1.0e9 })
            checkExact (legacyCurveValueToPosition (c), legacyCurveValueToPosition (1.0),
                        "a curve value above the top of its range did not saturate");

        for (double v : { legacyMaxPosition + 1.0e-12, 1.5, 2.0, 1.0e9 })
            checkExact (legacyToPosition (v), legacyToPosition (legacyMaxPosition),
                        "an old position above the top of the old travel did not saturate");

        for (double v : { 0.0, -0.0, -1.0e-30, -1.0, -1.0e9 })
            checkExact (legacyToPosition (v), minPosition, "an old position at or below zero is not silence");

        // The migration has to be monotonic too, or a project would come back
        // with two faders in the wrong order relative to each other.
        for (int n = 0; n < 200000; ++n)
        {
            double a = std::uniform_real_distribution<double> (0.0, legacyMaxPosition) (rng);
            double b = std::uniform_real_distribution<double> (0.0, legacyMaxPosition) (rng);

            if (a > b)
                std::swap (a, b);

            check (legacyToPosition (a) <= legacyToPosition (b),
                   "the migration reordered two old fader positions");
        }

        // The two named old constants, so that a change to either is a
        // decision. They are the only place those numbers should still appear,
        // and 0.8 over 1.25 being 0.640 is the complaint the whole change is
        // for.
        checkExact (legacyUnityPosition, 0.8, "the old law's unity position has changed");
        checkExact (legacyMaxPosition, 1.25, "the old law's travel has changed");
        checkExact (legacyPositionToGain (legacyUnityPosition), 1.0, "old unity was not unity gain");
    }

    if (failures == 0)
        std::printf ("FaderLawTest: all checks passed\n");
    else
        std::printf ("FaderLawTest: %d check(s) failed\n", failures);

    return failures == 0 ? 0 : 1;
}
