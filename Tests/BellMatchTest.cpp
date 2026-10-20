// Checks Fx/BellMatch.h: the bell that keeps its skirt at the top of the
// band.
//
// The first check here is the one that matters most, and it is not a check on
// the code at all. These equations were transcribed from a third party scan of
// a paywalled paper by a tool that admitted it had to restore dropped square
// root signs, so the first question is not "is the implementation right" but
// "are these the paper's equations". The paper publishes three numbers for a
// worked example in its section 6, and they are reproduced here to six
// decimal places. Nothing else in this file would be worth anything without
// that: a faithful implementation of a mistranscribed equation is a filter
// that is confidently wrong, which is the exact failure mode CLAUDE.md's
// research section is about.
//
// After that, four properties, each of which fails differently:
//
//  - the design conditions on the realised filter, which say it is a bell of
//    the right height, flat at DC, and has the prescribed gain at Nyquist
//  - that it is closer to the analogue prototype than the conventional design
//    is, by the amount claimed, because being different is not the same as
//    being better
//  - that the feasibility boundary is the closed form and does not move with
//    gain, which is what stops somebody simplifying the predicate into one
//    that works on boosts and produces NaN on cuts
//  - that a cut is the exact mirror of a boost, which is the property the
//    reciprocal construction exists for and the one a sign error breaks

#include "Fx/BellMatch.h"
#include "Fx/Svf.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>

namespace
{

int failures = 0;

void fail (const std::string& what)
{
    std::printf ("  FAIL  %s\n", what.c_str());
    ++failures;
}

std::string show (double v)
{
    char buffer[40];
    std::snprintf (buffer, sizeof (buffer), "%.6g", v);
    return buffer;
}

void expectNear (double actual, double expected, double tolerance,
                 const std::string& what)
{
    if (! (std::fabs (actual - expected) <= tolerance))
        fail (what + ": expected " + show (expected) + ", got " + show (actual)
                   + ", off by " + show (std::fabs (actual - expected)));
}

constexpr double pi = 3.14159265358979323846;

/** The magnitude of a design at a digital frequency in radians per sample,
    straight from the five numbers, which is what the state variable filter
    will run. */
double magnitude (const Fx::BellDesign& d, double w)
{
    const std::complex<double> s (0.0, std::tan (w * 0.5) / d.g);
    const std::complex<double> den = s * s + d.k * s + 1.0;
    const std::complex<double> num = d.m0 * den + d.m1 * s + d.m2;

    return std::abs (num / den);
}

double db (double x) { return 20.0 * std::log10 (x); }

//==============================================================================
/** Section 6 of the paper, which is the check on the transcription.

    A twelve decibel boost with a bandwidth of 0.2 pi, measured at nine
    decibels rather than at the half gain point, has a Nyquist gain of 2.053
    decibels at a centre of 0.3 pi, 2.725 at 0.5 pi and 4.420 at 0.7 pi. Those
    three numbers are printed in the paper, so reproducing them says the
    equations here are the equations there.

    Note the nine decibel bandedge: neither of the conventions anybody ships.
    That is why `matchedBellBoost` takes the bandedge gain rather than
    assuming it, and this is the reason.
*/
void thePaperSOwnNumbers()
{
    std::printf ("the paper's worked example\n");

    const double g = std::pow (10.0, 12.0 / 20.0);
    const double gb = std::pow (10.0, 9.0 / 20.0);

    struct Case { double centreFraction, published; };

    const Case cases[] { { 0.3, 2.053 }, { 0.5, 2.725 }, { 0.7, 4.420 } };

    for (const auto& c : cases)
    {
        const auto d = Fx::matchedBellBoost (c.centreFraction * pi, 0.2 * pi,
                                             g, gb, 1.0);

        if (! d.matched)
        {
            fail ("the paper's own example was reported as infeasible at "
                  + show (c.centreFraction) + " pi");
            continue;
        }

        // m0 is the prescribed Nyquist gain, G1.
        std::printf ("    centre %.1f pi: Nyquist gain %+.6f dB, paper says %+.3f\n",
                     c.centreFraction, db (d.m0), c.published);

        expectNear (db (d.m0), c.published, 0.001,
                    "the prescribed Nyquist gain disagrees with the paper at "
                      + show (c.centreFraction) + " pi");
    }
}

/** The five conditions the realised filter has to satisfy at 0.5 pi, and the
    regression vector for that case.

    The vector is hard coded on purpose. It is the one place in this file
    where a number is asserted rather than a property, and it is worth it:
    these five numbers are what reach the filter, and a change to any equation
    above that still happens to satisfy the properties would move them.
*/
void theDesignConditionsHold()
{
    std::printf ("the design conditions at a centre of 0.5 pi\n");

    const double g = std::pow (10.0, 12.0 / 20.0);
    const double gb = std::pow (10.0, 9.0 / 20.0);

    const auto d = Fx::matchedBellBoost (0.5 * pi, 0.2 * pi, g, gb, 1.0);

    if (! d.matched)
    {
        fail ("the design was reported as infeasible");
        return;
    }

    expectNear (db (magnitude (d, 0.5 * pi)), 12.0, 1.0e-9,
                "the bell does not reach its gain at its centre");
    expectNear (db (magnitude (d, 1.0e-9)), 0.0, 1.0e-9,
                "the bell is not flat at DC");
    expectNear (db (magnitude (d, pi * 0.9999999)), 2.725076, 1.0e-5,
                "the bell does not have the prescribed gain at Nyquist");

    std::printf ("    at the centre %+.6f dB, at DC %+.6f dB, at Nyquist %+.6f dB\n",
                 db (magnitude (d, 0.5 * pi)), db (magnitude (d, 1.0e-9)),
                 db (magnitude (d, pi * 0.9999999)));

    expectNear (d.g,   0.984968633, 1.0e-9, "g has moved");
    expectNear (d.k,   0.587258577, 1.0e-9, "k has moved");
    expectNear (d.m0,  1.368528393, 1.0e-9, "m0 has moved");
    expectNear (d.m1,  1.502145353, 1.0e-9, "m1 has moved");
    expectNear (d.m2, -0.368528393, 1.0e-9, "m2 has moved");

    std::printf ("    g %.9f k %.9f m0 %.9f m1 %.9f m2 %.9f\n",
                 d.g, d.k, d.m0, d.m1, d.m2);
}

//==============================================================================
/** The analogue bell this is all trying to be. */
double prototype (double f, double f0, double q, double gainDb)
{
    const double A = std::pow (10.0, gainDb / 40.0);
    const double k = 1.0 / (q * A);
    const std::complex<double> s (0.0, f / f0);

    return std::abs ((s * s + k * A * A * s + 1.0) / (s * s + k * s + 1.0));
}

/** Being different from the conventional design is not the same as being
    better than it, so the comparison is against the prototype both of them
    are approximations of, and the improvement is asserted as a quantity. */
void itIsCloserToTheProtoype()
{
    std::printf ("against the analogue prototype, at 44.1 kHz\n");

    const double rate = 44100.0;

    struct Case { double hz, q, gainDb, matched, conventional; };

    // Every figure in here is measured and then written down, not expected
    // and then measured. Two of them were guessed at first and both were
    // wrong, which is why the note is here.
    const Case cases[]
    {
        { 12000.0, 1.0,  12.0, 1.140, 5.233 },
        { 12000.0, 1.0, -12.0, 1.140, 5.233 },
        {  8000.0, 2.0,  -9.0, 0.185, 1.247 },
        {  8000.0, 1.0,   6.0, 0.244, 1.299 },
        {  1000.0, 1.0,  12.0, 0.019, 0.052 }
    };

    for (const auto& c : cases)
    {
        Fx::Svf matched, conventional;

        matched.setSampleRate (rate);
        matched.set (Fx::Svf::Type::bellMatched, c.hz, c.q, c.gainDb);

        conventional.setSampleRate (rate);
        conventional.set (Fx::Svf::Type::bell, c.hz, c.q, c.gainDb);

        if (! matched.isMatched())
        {
            fail ("the correction was unavailable at " + show (c.hz) + " Hz, Q "
                  + show (c.q) + ", where it should reach");
            continue;
        }

        double worstMatched = 0.0, worstConventional = 0.0;

        for (double at = 20.0; at <= 20000.0; at *= 1.002)
        {
            const double want = db (prototype (at, c.hz, c.q, c.gainDb));

            worstMatched = std::max (worstMatched,
                                     std::fabs (db (matched.magnitudeAt (at)) - want));
            worstConventional = std::max (worstConventional,
                                          std::fabs (db (conventional.magnitudeAt (at))
                                                       - want));
        }

        std::printf ("    %5.0f Hz Q %.1f %+5.1f dB: matched off by %.3f dB,"
                     " conventional off by %.3f dB\n",
                     c.hz, c.q, c.gainDb, worstMatched, worstConventional);

        expectNear (worstMatched, c.matched, 0.01,
                    "the matched design's error at " + show (c.hz) + " Hz has moved");
        expectNear (worstConventional, c.conventional, 0.01,
                    "the conventional design's error at " + show (c.hz)
                      + " Hz has moved");

        if (! (worstMatched < worstConventional))
            fail ("the correction made the response worse at " + show (c.hz) + " Hz");
    }
}

/** The feasibility boundary is a closed form in the centre frequency and Q,
    and gain does not appear in it.

    Both halves are asserted. The first is that the predicate agrees with the
    boundary found by searching upward until the design stops working, which
    is what says the predicate describes the algebra rather than a guess at
    it. The second is that the boundary is the same at every gain, which is
    the surprising half and the one a future simplification would break.
*/
void theBoundaryIsTheClosedForm()
{
    std::printf ("the feasibility boundary\n");

    const double rate = 44100.0;

    for (double q : { 0.5, 0.70710678118654752, 1.0, 2.0, 8.0 })
    {
        const double closed = 0.25 * rate * (std::sqrt (1.0 / (q * q) + 4.0) - 1.0 / q);

        double firstSearched = -1.0;

        for (double gainDb : { 1.0, 6.0, 12.0, 24.0, -6.0, -18.0 })
        {
            // Upward until the design gives up, which is the boundary as the
            // arithmetic sees it rather than as the predicate claims it.
            double highest = 0.0;

            for (double hz = 100.0; hz < rate * 0.49; hz += 5.0)
            {
                const auto d = Fx::matchedBell (hz, q, gainDb, rate);

                if (! d.matched)
                    break;

                highest = hz;
            }

            if (firstSearched < 0.0)
            {
                firstSearched = highest;

                std::printf ("    Q %5.3f: highest feasible %.0f Hz, closed form"
                             " %.0f Hz\n", q, highest, closed);

                expectNear (highest, closed, 6.0,
                            "the searched boundary disagrees with the closed form at Q "
                              + show (q));
            }
            else if (std::fabs (highest - firstSearched) > 1.0e-9)
            {
                fail ("the boundary moved with gain at Q " + show (q) + ": "
                      + show (firstSearched) + " Hz against " + show (highest)
                      + " Hz at " + show (gainDb) + " dB");
            }

            // And the predicate itself, either side of where it says the edge
            // is, which is the part the filter actually consults.
            if (Fx::bellIsMatchable (closed * 1.01, q, rate))
                fail ("the predicate says a band above the boundary is matchable at Q "
                      + show (q));

            if (! Fx::bellIsMatchable (closed * 0.99, q, rate))
                fail ("the predicate says a band below the boundary is not matchable at Q "
                      + show (q));
        }
    }
}

/** A cut is the exact mirror of a boost, in decibels, at every frequency.

    This is what the reciprocal construction is for, and a sign error in it
    produces a cut that is a plausible shape and the wrong one, which nobody
    would hear until they tried to undo a boost.
*/
void aCutMirrorsItsBoost()
{
    std::printf ("a cut against its boost\n");

    const double rate = 48000.0;
    double worst = 0.0;

    for (double hz : { 60.0, 500.0, 3000.0, 9000.0 })
        for (double q : { 0.7, 1.5, 6.0 })
            for (double gainDb : { 1.0, 6.0, 18.0 })
            {
                const auto up = Fx::matchedBell (hz, q, gainDb, rate);
                const auto down = Fx::matchedBell (hz, q, -gainDb, rate);

                if (! up.matched || ! down.matched)
                {
                    fail ("one direction was feasible and the other was not at "
                          + show (hz) + " Hz, Q " + show (q));
                    continue;
                }

                for (double w = 0.0001; w < pi; w *= 1.05)
                    worst = std::max (worst,
                                      std::fabs (db (magnitude (up, w))
                                                   + db (magnitude (down, w))));
            }

    std::printf ("    mirrors to %s dB\n", show (worst).c_str());

    if (worst > 1.0e-9)
        fail ("a matched cut is not the mirror of a matched boost: off by "
              + show (worst) + " dB");
}

/** Nothing anybody can ask for produces a number that is not a number.

    The NaN this is about is a real one: taking the absolute values in
    equation 17 and then designing a cut directly puts a negative quantity
    under a square root, and the result reaches the user as an equaliser that
    sometimes kills the audio. So this sweeps the whole clamped parameter
    space, in both directions of gain, and asserts that every design either
    reports that it could not match or hands back five finite numbers.
*/
void nothingProducesANotANumber()
{
    std::printf ("the whole parameter space\n");

    int matched = 0, declined = 0;

    for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (double hz = 10.0; hz < rate * 0.5; hz *= 1.07)
            for (double q : { 0.025, 0.1, 0.5, 1.0, 4.0, 20.0, 100.0 })
                for (double gainDb : { -60.0, -24.0, -6.0, -0.5, 0.0, 0.5,
                                       6.0, 24.0, 60.0 })
                {
                    const auto d = Fx::matchedBell (hz, q, gainDb, rate);

                    if (! d.matched)
                    {
                        ++declined;

                        continue;
                    }

                    ++matched;

                    const bool finite = std::isfinite (d.g) && std::isfinite (d.k)
                                          && std::isfinite (d.m0) && std::isfinite (d.m1)
                                          && std::isfinite (d.m2);

                    if (! finite || ! (d.g > 0.0))
                    {
                        fail ("a design that reported success is not usable at "
                              + show (hz) + " Hz, Q " + show (q) + ", "
                              + show (gainDb) + " dB");

                        return;
                    }

                    // And the response itself, which is what a NaN coefficient
                    // would show up in even if the five numbers looked fine.
                    for (double w = 0.001; w < pi; w *= 1.2)
                        if (! std::isfinite (magnitude (d, w)))
                        {
                            fail ("the response is not finite at " + show (hz)
                                  + " Hz, Q " + show (q));

                            return;
                        }
                }

    std::printf ("    %d designs matched, %d declined, none unusable\n",
                 matched, declined);

    if (matched < 1000 || declined < 100)
        fail ("the sweep did not exercise both branches: " + show (matched)
              + " matched and " + show (declined) + " declined");
}

/** A band outside the boundary keeps the conventional design rather than
    anything approximate, and says which one it is running. */
void outOfReachFallsBackAndSaysSo()
{
    std::printf ("above the boundary\n");

    const double rate = 44100.0;

    Fx::Svf matched, conventional;

    matched.setSampleRate (rate);
    conventional.setSampleRate (rate);

    // Q 0.7 at 44.1 kHz runs out at 11.4 kHz, so 16 kHz is well past it and
    // is exactly the wide air boost somebody would reach for.
    matched.set (Fx::Svf::Type::bellMatched, 16000.0, 0.7, 6.0);
    conventional.set (Fx::Svf::Type::bell, 16000.0, 0.7, 6.0);

    if (matched.isMatched())
    {
        fail ("a 16 kHz band at Q 0.7 claims to be matched at 44.1 kHz");
        return;
    }

    double worst = 0.0;

    for (double at = 20.0; at < rate * 0.49; at *= 1.02)
        worst = std::max (worst, std::fabs (db (matched.magnitudeAt (at))
                                              - db (conventional.magnitudeAt (at))));

    std::printf ("    falls back exactly: differs from the conventional bell by"
                 " %s dB\n", show (worst).c_str());

    if (worst > 1.0e-12)
        fail ("the fallback is not the conventional bell: off by " + show (worst)
              + " dB");

    // And a flat band, which has nothing to match and must not pretend to.
    matched.set (Fx::Svf::Type::bellMatched, 1000.0, 1.0, 0.0);

    if (matched.isMatched())
        fail ("a flat band claims to be matched");

    for (double at = 20.0; at < rate * 0.49; at *= 1.1)
        expectNear (matched.magnitudeAt (at), 1.0, 1.0e-12,
                    "a flat matched bell is not flat");
}

} // namespace

int main()
{
    std::printf ("Fx::BellMatch\n\n");

    thePaperSOwnNumbers();
    theDesignConditionsHold();
    itIsCloserToTheProtoype();
    theBoundaryIsTheClosedForm();
    aCutMirrorsItsBoost();
    outOfReachFallsBackAndSaysSo();
    nothingProducesANotANumber();

    std::printf ("\n");

    if (failures > 0)
    {
        std::printf ("%d check(s) failed.\n", failures);
        return 1;
    }

    std::printf ("All checks passed.\n");
    return 0;
}
