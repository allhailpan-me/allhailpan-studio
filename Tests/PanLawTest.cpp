// ---------------------------------------------------------------------------
// Checks what the pan control does to the level of what it is panning.
//
// This is arithmetic that cannot announce that it is wrong. A pan law that is
// off does not error, does not crackle and does not look wrong on a meter: it
// just means a mix balanced in this studio is not balanced anywhere else, and
// that a mono guitar loses level every time it is moved off centre. So the
// properties below are the published definitions of the laws, asserted
// directly rather than through the formulas that implement them.
//
// Three of them matter more than the rest:
//
//   * A constant power law keeps total power flat across the whole sweep.
//     That is the whole reason the mono panner exists, so it is checked as a
//     power sum over thousands of positions rather than at the centre only.
//   * The centre gain of each law is the number the law is named by. Those
//     numbers are computed here from the published formulas evaluated at
//     centre, not read back out of the header, so the test disagrees with the
//     header if either is wrong.
//   * Balance mode is still bit for bit the old `min (1, 1 +/- p)`. That is
//     what guarantees no project already mixed in this studio moves, and it
//     is checked through a float round trip because that is how the engine
//     applies it.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include "PanLaw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <vector>

static int failures = 0;

// Capped, because a property checked over twenty thousand positions can fail
// twenty thousand times and the first few lines are the only ones anybody
// reads. The count is still complete and is what the exit code comes from.
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

static const char* lawName (PanLaw::Law law) { return PanLaw::name (law); }

// The published centre gains, worked out here from the formulas rather than
// taken from the header. Oland and Dannenberg give the linear law as a
// straight line, so half gain at centre; the constant power law as a sine, so
// sin(pi/4); and the -4.5 dB law as the square root of the product of the two.
// Switched on the law itself rather than on the curve the header says the law
// uses, which matters: a law wired to the wrong curve would otherwise take the
// test's expectation along with it and every check below would still pass.
static double publishedCentreGain (PanLaw::Law law)
{
    const double linear        = 0.5;
    const double constantPower = std::sqrt (0.5);

    switch (law)
    {
        case PanLaw::Law::zero:                     return 1.0;
        case PanLaw::Law::minus3:
        case PanLaw::Law::minus3Compensated:        return constantPower;
        case PanLaw::Law::minus4point5:
        case PanLaw::Law::minus4point5Compensated:  return std::sqrt (linear * constantPower);
        case PanLaw::Law::minus6:
        case PanLaw::Law::minus6Compensated:        return linear;
    }

    return 1.0;
}


// The published curve itself, written the way the sources write it rather than
// the way the header computes it: `cos (theta)` for the side the source is
// moving away from, where the header reads one function at the complementary
// position. That difference is the point. Without this, every assertion in
// here is a property with many solutions, and a constant power law really can
// be replaced by a square root crossfade without anything noticing: it has the
// same centre gain, the same extremes, the same flat power and the same
// monotonicity, and it is still 2.89 dB out at half pan. A number that is
// present, plausible and wrong is the failure this whole file is against.
static PanLaw::Gains publishedGains (PanLaw::Law law, double position)
{
    const double t     = (position + 1.0) * 0.5;
    const double theta = t * 3.14159265358979323846 / 2.0;

    PanLaw::Gains g;

    switch (law)
    {
        case PanLaw::Law::zero:
            // Logic's 0 dB law, which is the formula the studio already used.
            g = { std::min (1.0, 1.0 - position), std::min (1.0, 1.0 + position) };
            break;

        case PanLaw::Law::minus3:
        case PanLaw::Law::minus3Compensated:
            // Oland and Dannenberg: L = cos (theta), R = sin (theta).
            g = { std::cos (theta), std::sin (theta) };
            break;

        case PanLaw::Law::minus4point5:
        case PanLaw::Law::minus4point5Compensated:
            // "the square root of the product of the linear and constant
            // power laws", with the linear law being the straight line below.
            g = { std::sqrt ((1.0 - t) * std::cos (theta)),
                  std::sqrt (t * std::sin (theta)) };
            break;

        case PanLaw::Law::minus6:
        case PanLaw::Law::minus6Compensated:
            // Oland and Dannenberg's linear law, in t rather than theta.
            g = { 1.0 - t, t };
            break;
    }

    if (PanLaw::isCompensated (law))
    {
        const double centre = publishedCentreGain (law);
        g.left  /= centre;
        g.right /= centre;
    }

    return g;
}

int main()
{
    using namespace PanLaw;

    // Written out rather than collected from `lawAt`, so that a law dropped
    // from the list the menu and the project file are built on is a failure
    // here instead of a law nobody can choose and a saved project that comes
    // back on a different one.
    const std::vector<Law> laws { Law::zero,
                                  Law::minus3, Law::minus3Compensated,
                                  Law::minus4point5, Law::minus4point5Compensated,
                                  Law::minus6, Law::minus6Compensated };

    std::mt19937 rng (20261013);
    std::uniform_real_distribution<double> anywhere (-1.0, 1.0);

    // ---- the stored round trip -------------------------------------------
    std::printf ("what goes in the project file\n");

    check (numLaws == (int) laws.size(),
           "numLaws says " + std::to_string (numLaws) + " but there are "
               + std::to_string (laws.size()) + " laws");

    for (int i = 0; i < numLaws; ++i)
    {
        check (lawToStored (lawAt (i)) == i,
               "law index " + std::to_string (i) + " does not survive a round trip");
        check (lawFromStored (lawToStored (lawAt (i))) == lawAt (i),
               std::string ("law ") + lawName (lawAt (i)) + " does not survive a round trip");
    }

    // Every law has to be reachable from the index the menu and the file use,
    // and reachable exactly once.
    for (auto law : laws)
    {
        int found = 0;
        for (int i = 0; i < numLaws; ++i)
            if (lawAt (i) == law)
                ++found;

        check (found == 1, std::string ("law ") + lawName (law) + " is reachable "
                               + std::to_string (found) + " times by index, not once");

        check (lawFromStored (lawToStored (law)) == law,
               std::string ("law ") + lawName (law) + " does not survive a round trip");
    }

    for (int stored : { -1000, -1, numLaws, numLaws + 1, 9999 })
        check (lawFromStored (stored) == defaultLaw,
               "a stored law of " + std::to_string (stored) + " should fall back to the default");

    check (modeFromStored (modeToStored (Mode::balance)) == Mode::balance, "balance mode round trip");
    check (modeFromStored (modeToStored (Mode::pan)) == Mode::pan, "pan mode round trip");

    for (int stored : { -7, 2, 99 })
        check (modeFromStored (stored) == Mode::balance,
               "an unknown stored mode should read as balance, not pan");

    check (defaultMode == Mode::balance, "the default mode must stay balance or saved mixes move");

    // What makes compensation harmless in balance mode: the flat curve is
    // already unity at centre, so dividing by its centre gain changes nothing.
    // Asserted so that the redundant looking mode test in `gains` can be read
    // as redundant on purpose rather than removed as dead.
    checkExact (centreGain (Curve::flat), 1.0, "the flat curve should be unity at centre");

    // ---- the number in each law's name -----------------------------------
    std::printf ("each law's centre gain is the figure it is named by\n");

    for (auto law : laws)
    {
        const double expected = publishedCentreGain (law);
        const auto   g        = gains (Mode::pan, law, 0.0);

        if (isCompensated (law))
        {
            // Compensated means unity at centre, and exactly unity: the curve
            // is divided by its own centre value, so this is one value over
            // itself and must land on 1.0 with nothing left over. An insert
            // sitting centred has to be able to change mode in silence.
            checkExact (g.left,  1.0, std::string ("centre left gain under ") + lawName (law));
            checkExact (g.right, 1.0, std::string ("centre right gain under ") + lawName (law));
            checkExact (centreGainDb (Mode::pan, law), 0.0,
                        std::string ("centre dB under ") + lawName (law));
        }
        else
        {
            checkClose (g.left,  expected, 1.0e-12, std::string ("centre left gain under ") + lawName (law));
            checkClose (g.right, expected, 1.0e-12, std::string ("centre right gain under ") + lawName (law));
        }
    }

    // The names, against the decibels the published formulas give.
    checkClose (centreGainDb (Mode::pan, Law::zero),        0.0,     1.0e-9,  "the 0 dB law at centre");
    checkClose (centreGainDb (Mode::pan, Law::minus3),      -3.0103, 1.0e-4,  "the -3 dB law at centre");
    checkClose (centreGainDb (Mode::pan, Law::minus4point5), -4.5154, 1.0e-4, "the -4.5 dB law at centre");
    checkClose (centreGainDb (Mode::pan, Law::minus6),      -6.0206, 1.0e-4,  "the -6 dB law at centre");

    // ---- the extremes ----------------------------------------------------
    std::printf ("the extremes\n");

    for (auto law : laws)
    {
        const double endpoint = isCompensated (law) ? 1.0 / publishedCentreGain (law) : 1.0;

        const auto hardLeft  = gains (Mode::pan, law, -1.0);
        const auto hardRight = gains (Mode::pan, law,  1.0);

        checkExact (hardLeft.right, 0.0, std::string ("hard left should leave nothing on the right under ") + lawName (law));
        checkExact (hardRight.left, 0.0, std::string ("hard right should leave nothing on the left under ") + lawName (law));

        checkClose (hardLeft.left,   endpoint, 1.0e-12, std::string ("hard left gain under ") + lawName (law));
        checkClose (hardRight.right, endpoint, 1.0e-12, std::string ("hard right gain under ") + lawName (law));
    }

    // Logic's own figures for a compensated law: the ends come up by the
    // amount the law is named for.
    checkClose (20.0 * std::log10 (gains (Mode::pan, Law::minus3Compensated, 1.0).right),
                3.0103, 1.0e-4, "a -3 dB compensated law should raise the extreme by 3 dB");
    checkClose (20.0 * std::log10 (gains (Mode::pan, Law::minus6Compensated, 1.0).right),
                6.0206, 1.0e-4, "a -6 dB compensated law should raise the extreme by 6 dB");

    // ---- the curve itself, not just its properties -----------------------
    std::printf ("every law matches its published curve across the sweep\n");

    for (auto law : laws)
    {
        double worst = 0.0;
        double worstAt = 0.0;

        for (int step = 0; step <= 4000; ++step)
        {
            const double p = -1.0 + 2.0 * (double) step / 4000.0;
            const auto   g = gains (Mode::pan, law, p);
            const auto   want = publishedGains (law, p);

            const double off = std::max (std::abs (g.left - want.left),
                                         std::abs (g.right - want.right));

            if (off > worst)
            {
                worst = off;
                worstAt = p;
            }
        }

        checkClose (worst, 0.0, 1.0e-12,
                    std::string ("the curve departs from the published one under ")
                        + lawName (law) + ", worst at p = " + std::to_string (worstAt));
    }

    // ---- symmetry, monotonicity and range over a random sweep ------------
    std::printf ("symmetry, monotonicity and range over 20000 random positions\n");

    for (auto mode : { Mode::balance, Mode::pan })
    {
        for (auto law : laws)
        {
            const double endpoint = mode == Mode::pan && isCompensated (law)
                                        ? 1.0 / publishedCentreGain (law) : 1.0;

            for (int n = 0; n < 20000; ++n)
            {
                const double p = anywhere (rng);
                const auto   g = gains (mode, law, p);
                const auto   m = gains (mode, law, -p);

                // Exact, not approximate: the two sides are one function read
                // at t and at 1 - t, so a mirrored position has to give the
                // mirrored pair down to the last bit. Anything less means the
                // two sides were written out separately, which is how a
                // panner ends up slightly off centre at centre.
                checkExact (g.left,  m.right, std::string ("mirror symmetry under ") + lawName (law));
                checkExact (g.right, m.left,  std::string ("mirror symmetry under ") + lawName (law));

                check (std::isfinite (g.left) && std::isfinite (g.right),
                       std::string ("a gain that is not finite under ") + lawName (law));
                check (g.left >= 0.0 && g.right >= 0.0,
                       std::string ("a negative gain under ") + lawName (law));
                check (g.left <= endpoint + 1.0e-12 && g.right <= endpoint + 1.0e-12,
                       std::string ("a gain above the law's own extreme under ") + lawName (law));
            }

            // Monotonic across the sweep. A law that doubles back makes the
            // knob move the sound the wrong way somewhere in its travel.
            double previousLeft = gains (mode, law, -1.0).left;
            double previousRight = gains (mode, law, -1.0).right;

            for (int step = 1; step <= 4000; ++step)
            {
                const double p = -1.0 + 2.0 * (double) step / 4000.0;
                const auto   g = gains (mode, law, p);

                check (g.left  <= previousLeft  + 1.0e-15,
                       std::string ("the left gain rose while panning right under ") + lawName (law));
                check (g.right >= previousRight - 1.0e-15,
                       std::string ("the right gain fell while panning right under ") + lawName (law));

                previousLeft  = g.left;
                previousRight = g.right;
            }
        }
    }

    // ---- the property the mono panner exists to produce -------------------
    std::printf ("a mono source keeps its level across the sweep\n");

    // A mono source is centred, so both halves of the insert carry the same
    // signal and what reaches the room is the power in the two halves added
    // together. Panning it must not change that. This is the complaint the
    // whole change is for, so it is asserted over the sweep rather than at
    // three points.
    for (auto law : { Law::minus3, Law::minus3Compensated })
    {
        const double atCentre = [law]
        {
            const auto g = gains (Mode::pan, law, 0.0);
            return g.left * g.left + g.right * g.right;
        }();

        double worst = 0.0;

        for (int step = 0; step <= 20000; ++step)
        {
            const double p = -1.0 + 2.0 * (double) step / 20000.0;
            const auto   g = gains (Mode::pan, law, p);
            const double power = g.left * g.left + g.right * g.right;

            worst = std::max (worst, std::abs (10.0 * std::log10 (power / atCentre)));
        }

        checkClose (worst, 0.0, 1.0e-9,
                    std::string ("power should not move at all across the sweep under ") + lawName (law));
    }

    // And the other half of the same idea: a linear law holds total amplitude
    // rather than total power, which is what keeps a mix the same level after
    // it is folded to mono. Both are real laws for real reasons, so both are
    // checked for the thing they claim.
    for (auto law : { Law::minus6, Law::minus6Compensated })
    {
        const auto centre = gains (Mode::pan, law, 0.0);
        const double atCentre = centre.left + centre.right;

        for (int step = 0; step <= 20000; ++step)
        {
            const double p = -1.0 + 2.0 * (double) step / 20000.0;
            const auto   g = gains (Mode::pan, law, p);

            checkClose (g.left + g.right, atCentre, 1.0e-12,
                        std::string ("amplitude should not move across the sweep under ") + lawName (law));
        }
    }

    // The -4.5 dB law sits between the two, which is the only thing it claims
    // to do: it cannot be flat in either, and it must be closer to flat in
    // both than the law it is a compromise with.
    {
        const auto measure = [] (Law law, bool power)
        {
            const auto centre = gains (Mode::pan, law, 0.0);
            const double at = power ? centre.left * centre.left + centre.right * centre.right
                                    : centre.left + centre.right;
            double worst = 0.0;

            for (int step = 0; step <= 4000; ++step)
            {
                const double p = -1.0 + 2.0 * (double) step / 4000.0;
                const auto   g = gains (Mode::pan, law, p);
                const double v = power ? g.left * g.left + g.right * g.right
                                       : g.left + g.right;
                worst = std::max (worst, std::abs (v / at - 1.0));
            }

            return worst;
        };

        check (measure (Law::minus4point5, true) < measure (Law::minus6, true),
               "the -4.5 dB law should hold power better than the linear law");
        check (measure (Law::minus4point5, false) < measure (Law::minus3, false),
               "the -4.5 dB law should hold amplitude better than the constant power law");
    }

    // ---- what the studio did before, and still does by default -----------
    std::printf ("balance mode is still bit for bit what the studio always did\n");

    std::uniform_real_distribution<float> anyFloat (-1.0f, 1.0f);

    for (int n = 0; n < 50000; ++n)
    {
        const float p = n < 3 ? (n == 0 ? -1.0f : (n == 1 ? 0.0f : 1.0f)) : anyFloat (rng);

        // Exactly how the engine used to work it out, floats and all.
        const float wasLeft  = std::min (1.0f, 1.0f - p);
        const float wasRight = std::min (1.0f, 1.0f + p);

        const auto g = gains (Mode::balance, defaultLaw, (double) p);

        checkExact ((double) (float) g.left,  (double) wasLeft,
                    "balance mode changed the left gain at p = " + std::to_string (p));
        checkExact ((double) (float) g.right, (double) wasRight,
                    "balance mode changed the right gain at p = " + std::to_string (p));
    }

    // A balance control takes no law, so every law has to give the same pair.
    for (int n = 0; n < 5000; ++n)
    {
        const double p = anywhere (rng);
        const auto reference = gains (Mode::balance, Law::zero, p);

        for (auto law : laws)
        {
            const auto g = gains (Mode::balance, law, p);
            checkExact (g.left,  reference.left,  std::string ("the law reached balance mode: ") + lawName (law));
            checkExact (g.right, reference.right, std::string ("the law reached balance mode: ") + lawName (law));
        }

        // And the 0 dB law is the balance curve, which is why keeping balance
        // as the default is the same as keeping Logic's 0 dB pan law.
        const auto asPan = gains (Mode::pan, Law::zero, p);
        checkExact (asPan.left,  reference.left,  "the 0 dB pan law should be the balance curve");
        checkExact (asPan.right, reference.right, "the 0 dB pan law should be the balance curve");
    }

    // The complaint, stated as a number. Under the old behaviour a mono source
    // panned hard over keeps one half of its power, which is 3.01 dB down, and
    // that is what the fader was being ridden to put back.
    {
        const auto centre = gains (Mode::balance, defaultLaw, 0.0);
        const auto hard   = gains (Mode::balance, defaultLaw, -1.0);

        const double centrePower = centre.left * centre.left + centre.right * centre.right;
        const double hardPower   = hard.left * hard.left + hard.right * hard.right;

        checkClose (10.0 * std::log10 (hardPower / centrePower), -3.0103, 1.0e-4,
                    "a balanced mono source should lose exactly 3 dB at the extreme");
    }

    // ---- changing mode on a centred insert must be silent ----------------
    std::printf ("switching mode at centre\n");

    for (auto law : laws)
    {
        const auto asBalance = gains (Mode::balance, law, 0.0);
        const auto asPan     = gains (Mode::pan, law, 0.0);

        if (isCompensated (law) || law == Law::zero)
        {
            checkExact (asPan.left,  asBalance.left,  std::string ("switching mode at centre should be silent under ") + lawName (law));
            checkExact (asPan.right, asBalance.right, std::string ("switching mode at centre should be silent under ") + lawName (law));
        }
        else
        {
            // Documented, not an oversight: an uncompensated law puts unity at
            // the extremes, so it has to be below unity at centre. Anybody
            // choosing one is choosing that.
            check (asPan.left < asBalance.left,
                   std::string ("an uncompensated law should be quieter at centre: ") + lawName (law));
        }
    }

    // ---- a position that cannot be trusted -------------------------------
    std::printf ("positions out of range\n");

    for (auto law : laws)
    {
        for (double p : { 1.5, 2.0, 1.0e9, std::numeric_limits<double>::infinity() })
        {
            const auto g = gains (Mode::pan, law, p);
            const auto hard = gains (Mode::pan, law, 1.0);
            checkExact (g.left,  hard.left,  std::string ("a position past hard right should clamp under ") + lawName (law));
            checkExact (g.right, hard.right, std::string ("a position past hard right should clamp under ") + lawName (law));
        }

        for (double p : { -1.5, -2.0, -1.0e9, -std::numeric_limits<double>::infinity() })
        {
            const auto g = gains (Mode::pan, law, p);
            const auto hard = gains (Mode::pan, law, -1.0);
            checkExact (g.left,  hard.left,  std::string ("a position past hard left should clamp under ") + lawName (law));
            checkExact (g.right, hard.right, std::string ("a position past hard left should clamp under ") + lawName (law));
        }

        // A gain of NaN does not go quiet, it silences everything downstream
        // and says nothing about where it came from, so a position that is not
        // a number is read as centre.
        const auto nan = gains (Mode::pan, law, std::numeric_limits<double>::quiet_NaN());
        const auto mid = gains (Mode::pan, law, 0.0);
        checkExact (nan.left,  mid.left,  std::string ("a NaN position should read as centre under ") + lawName (law));
        checkExact (nan.right, mid.right, std::string ("a NaN position should read as centre under ") + lawName (law));
    }

    // ---- the names ---------------------------------------------------------
    for (auto law : laws)
        check (name (law) != nullptr && name (law)[0] != '\0', "a law with no name");

    if (failures == 0)
        std::printf ("PanLawTest: all checks passed\n");
    else
        std::printf ("PanLawTest: %d check(s) failed\n", failures);

    return failures == 0 ? 0 : 1;
}
