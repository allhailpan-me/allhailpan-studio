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
// There is a stronger statement available than the paper's three numbers,
// and it needs no access to the paper at all. **Five conditions determine a
// digital biquad**, and this design claims five things: the gain at DC, the
// gain at Nyquist, the gain at the centre, the gain at both bandedges, and
// the distance between those bandedges. A realisation that satisfies all five
// is the right filter whatever happened to the square roots in the scan. The
// prescribed Nyquist gain is checked against the analogue prototype's gain at
// half the sample rate, derived in this file rather than read out of the
// header, so equation (21) is verified and not merely restated.
//
// After that, the properties, each of which fails differently:
//
//  - the five design conditions on the realised filter, at one setting with a
//    regression vector, and then over the whole space a band can be set to
//  - that the centre is the most the band does anywhere, which the five
//    conditions do not imply and a lifted upper skirt could break
//  - that it is closer to the analogue prototype than the conventional design
//    is, by the amount claimed, because being different is not the same as
//    being better, together with the one corner where it is not and the size
//    of that exception
//  - that the correction is applied wherever the predicate says it will be
//    and nowhere else, which is the check this file was missing: the
//    correction could be switched off above Q 10, below Q 0.5, or at every
//    rate above 48 kHz, and nothing here noticed
//  - that the feasibility boundary is the closed form at every rate a device
//    can open at and does not move with gain, which is what stops somebody
//    simplifying the predicate into one that works on boosts and produces NaN
//    on cuts, or measuring it against a fixed 44.1 kHz
//  - that a cut is the exact mirror of a boost, which is the property the
//    reciprocal construction exists for and the one a sign error breaks
//  - that a bandedge gain the caller chose, which is what the paper's own
//    worked example uses, is either designed correctly or declined: the only
//    way to reach four of the guards in the header
//  - that arguments which are not settings are refused rather than designed

#include "Fx/BellMatch.h"
#include "Fx/Svf.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
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

    // At every rate a device can open at, not at one. The boundary is a
    // quarter of the sample rate times a function of Q alone, so a version of
    // it that read a fixed 44.1 kHz would be right here and wrong everywhere
    // else: at 96 kHz it would put the edge at 13.6 kHz where it belongs at
    // 29.7, and every band between the two would drop silently to the
    // conventional design. That breakage was tried and it survived this check
    // while it ran at one rate. It is the fault `Fx/Tilt.h`'s entry in
    // CLAUDE.md is about, in a file that takes the rate as an argument and
    // looks immune to it.
    for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    for (double q : { 0.5, 0.70710678118654752, 1.0, 2.0, 8.0 })
    {
        const double closed = 0.25 * rate * (std::sqrt (1.0 / (q * q) + 4.0) - 1.0 / q);

        // Five hertz at 44.1 kHz, and the same fraction of the band at every
        // other rate, so the search resolves the boundary equally well at all
        // of them rather than finely at one.
        const double step = rate / 8820.0;

        double firstSearched = -1.0;

        for (double gainDb : { 1.0, 6.0, 12.0, 24.0, -6.0, -18.0 })
        {
            // Upward until the design gives up, which is the boundary as the
            // arithmetic sees it rather than as the predicate claims it.
            double highest = 0.0;

            for (double hz = 100.0; hz < rate * 0.49; hz += step)
            {
                const auto d = Fx::matchedBell (hz, q, gainDb, rate);

                if (! d.matched)
                    break;

                highest = hz;
            }

            if (firstSearched < 0.0)
            {
                firstSearched = highest;

                std::printf ("    %6.0f Hz, Q %5.3f: highest feasible %.0f Hz,"
                             " closed form %.0f Hz\n", rate, q, highest, closed);

                expectNear (highest, closed, 1.2 * step,
                            "the searched boundary disagrees with the closed form at Q "
                              + show (q) + " at " + show (rate) + " Hz");
            }
            else if (std::fabs (highest - firstSearched) > 1.0e-9)
            {
                fail ("the boundary moved with gain at Q " + show (q) + ", "
                      + show (rate) + " Hz: " + show (firstSearched)
                      + " Hz against " + show (highest) + " Hz at "
                      + show (gainDb) + " dB");
            }

            // And the predicate itself, either side of where it says the edge
            // is, which is the part the filter actually consults.
            if (Fx::bellIsMatchable (closed * 1.01, q, rate))
                fail ("the predicate says a band above the boundary is matchable at Q "
                      + show (q) + " at " + show (rate) + " Hz");

            if (! Fx::bellIsMatchable (closed * 0.99, q, rate))
                fail ("the predicate says a band below the boundary is not matchable at Q "
                      + show (q) + " at " + show (rate) + " Hz");
        }
    }
}

/** The conventional bell as `Svf` builds it, as five numbers rather than as a
    filter.

    Here rather than through `Svf` for the comparison below, because `Svf`
    clamps a centre frequency to 0.49 of the sample rate and the feasibility
    boundary runs above that at a Q over about ninety. Comparing a design
    against a prototype at the frequency that was asked for, when the filter
    quietly used a different one, would read as an error in the design. The
    five hand picked cases above do go through the real filter, which is where
    that path belongs.
*/
Fx::BellDesign conventionalBell (double hz, double q, double gainDb, double rate)
{
    const double A = std::pow (10.0, gainDb / 40.0);

    Fx::BellDesign d;
    d.g = std::tan (pi * hz / rate);
    d.k = 1.0 / (q * A);
    d.m0 = 1.0;
    d.m1 = d.k * (A * A - 1.0);
    d.m2 = 0.0;
    d.matched = true;

    return d;
}

/** The correction is an improvement everywhere a band can sit, and the one
    corner where it is not, pinned so it cannot grow.

    The five cases above assert the improvement at five settings at 44.1 kHz.
    This asserts it as a rule, over the whole clamped space at every rate, and
    it needs one honest exception.

    **Say which band the comparison is over.** The first version of this
    measured over 20 Hz to 20 kHz, the way the five cases above do, and
    reported the matched design as up to 21 dB **worse** than the conventional
    one. That figure is real and it is not about the filter: at 192 kHz a band
    can be centred at 54 kHz, and over 20 Hz to 20 kHz the comparison is then
    looking at nothing but the bottom of the skirt, where the two designs
    differ most and where neither is doing the job the band was set to do.
    Over the whole band to Nyquist the same sweep says the matched design is
    closer at every Q down to 0.15. This is the trap `Fx/Saturator.h`'s entry
    in CLAUDE.md is about, met again in a different file.

    The exception is a genuine one. Below a Q of about 0.15, which is a
    bandwidth seven times the centre frequency and wider, the matched design
    can sit further from the analogue prototype than the conventional one, by
    a measured worst of 0.58 dB at the 0.025 the mixer clamps Q to, which a
    finer sweep than this one found and which this grid reads as 0.17. The
    mechanism is the bilinear transform rather than anything in the paper: the
    analogue prototype is geometrically symmetric about its centre in
    frequency, the digital filter is geometrically symmetric in `tan(w/2)`,
    and the tangent stretches the top of the band. So a band whose upper
    bandedge is near Nyquist has to put its lower bandedge further down than
    the prototype does, and for a very wide band that pushes the lower skirt
    below where it belongs. The correction trades an upper skirt crushed to
    nothing for a lower skirt that is too low, and for the widest bands that
    is the worse trade.
*/
void theCorrectionIsAnImprovementAndWhereItIsNot()
{
    std::printf ("the correction against the conventional bell, whole band\n");

    double worstWide = -1000.0, worstNarrow = -1000.0;
    std::string whereWide, whereNarrow;

    for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (double q : { 0.025, 0.05, 0.1, 0.15, 0.25, 0.5, 0.70710678118654752,
                          1.0, 2.0, 8.0, 30.0, 100.0 })
        {
            const double boundary = 0.25 * rate
                                      * (std::sqrt (1.0 / (q * q) + 4.0) - 1.0 / q);

            for (double frac : { 0.05, 0.2, 0.4, 0.6, 0.8, 0.9, 0.97 })
                for (double gainDb : { 1.0, 6.0, 18.0, 48.0, 120.0,
                                       -1.0, -6.0, -18.0, -48.0, -120.0 })
                {
                    const double hz = boundary * frac;

                    if (hz < 10.0 || hz > rate * 0.49)
                        continue;

                    const auto m = Fx::matchedBell (hz, q, gainDb, rate);

                    if (! m.matched)
                        continue;

                    const auto c = conventionalBell (hz, q, gainDb, rate);

                    double errorMatched = 0.0, errorConventional = 0.0;

                    for (double at = 20.0; at <= rate * 0.49; at *= 1.02)
                    {
                        const double want = db (prototype (at, hz, q, gainDb));
                        const double w = 2.0 * pi * at / rate;

                        errorMatched = std::max (errorMatched,
                                                 std::fabs (db (magnitude (m, w)) - want));
                        errorConventional = std::max (errorConventional,
                                                      std::fabs (db (magnitude (c, w)) - want));
                    }

                    const double difference = errorMatched - errorConventional;
                    const std::string where = show (hz) + " Hz, Q " + show (q) + ", "
                                                + show (gainDb) + " dB at " + show (rate) + " Hz";

                    if (q < 0.15)
                    {
                        if (difference > worstWide) { worstWide = difference; whereWide = where; }
                    }
                    else if (difference > worstNarrow)
                    {
                        worstNarrow = difference;
                        whereNarrow = where;
                    }
                }
        }

    std::printf ("    at Q 0.15 and above, worst %+.4f dB against the conventional bell, at %s\n",
                 worstNarrow, whereNarrow.c_str());
    std::printf ("    below Q 0.15,        worst %+.4f dB, at %s\n",
                 worstWide, whereWide.c_str());

    // Above Q 0.15 there is no slack to give: the correction has to be at
    // least as close as the design it replaces, everywhere.
    if (worstNarrow > 1.0e-6)
        fail ("the correction is worse than the conventional bell at a usable Q, by "
                + show (worstNarrow) + " dB, at " + whereNarrow);

    // Below it, the exception is allowed and bounded. The worst this grid
    // finds is 0.17 dB and the worst a sweep fine enough to hunt for it finds
    // is 0.58, so the ceiling is 1 dB: loose enough not to fail on the grid
    // moving, tight enough to fail if the mechanism above ever gets worse
    // without somebody deciding that it should.
    if (worstWide > 1.0)
        fail ("the correction is worse than the conventional bell on a very wide band by "
                + show (worstWide) + " dB, which is past the 0.58 dB this is known to cost, at "
                + whereWide);
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

    std::printf ("    mirrors to %s dB over the fixed cases\n", show (worst).c_str());

    if (worst > 1.0e-9)
        fail ("a matched cut is not the mirror of a matched boost: off by "
              + show (worst) + " dB");

    // And over everything a band can be set to, which the fixed cases above
    // do not reach: three frequencies and three gains at one rate leave the
    // top of the gain clamp and the ends of the Q range untried, and the
    // reciprocal construction is at its worst where the arithmetic is.
    std::mt19937_64 rng (4242);
    std::uniform_real_distribution<double> unit (0.0, 1.0);

    double worstSwept = 0.0;
    std::string where;
    int pairs = 0;

    for (int trial = 0; trial < 2000; ++trial)
    {
        const double rate = (trial % 4 == 0 ? 44100.0 : trial % 4 == 1 ? 48000.0
                                            : trial % 4 == 2 ? 96000.0 : 192000.0);
        const double hz = std::pow (10.0, 1.0 + unit (rng) * (std::log10 (rate * 0.49) - 1.0));
        const double q = std::pow (10.0, std::log10 (0.025)
                                           + unit (rng) * (std::log10 (100.0) - std::log10 (0.025)));
        const double gainDb = 0.5 + 119.5 * unit (rng);

        const auto up = Fx::matchedBell (hz, q, gainDb, rate);
        const auto down = Fx::matchedBell (hz, q, -gainDb, rate);

        // A boost and its cut are the same design problem, so one of them
        // being out of reach while the other is not would mean the boundary
        // had picked up a dependence on the sign of the gain.
        if (up.matched != down.matched)
        {
            fail ("one direction was feasible and the other was not at " + show (hz)
                    + " Hz, Q " + show (q) + ", " + show (gainDb) + " dB at "
                    + show (rate) + " Hz");
            return;
        }

        if (! up.matched)
            continue;

        ++pairs;

        for (double w = 1.0e-5; w < pi; w *= 1.02)
        {
            const double off = std::fabs (db (magnitude (up, w)) + db (magnitude (down, w)));

            if (off > worstSwept)
            {
                worstSwept = off;
                where = show (hz) + " Hz, Q " + show (q) + ", " + show (gainDb)
                          + " dB at " + show (rate) + " Hz";
            }
        }
    }

    std::printf ("    %d pairs over the reachable space mirror to %s dB, worst at %s\n",
                 pairs, show (worstSwept).c_str(), where.c_str());

    // Measured 2.1e-09 dB, at the top of the gain clamp where the design
    // loses most of its significant figures. Anything above a decade of that
    // is a sign error rather than conditioning.
    if (worstSwept > 1.0e-7)
        fail ("a matched cut is not the mirror of its boost over the swept space: off by "
                + show (worstSwept) + " dB, at " + where);

    if (pairs < 500)
        fail ("the sweep hardly found a feasible pair: " + show (pairs));
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

//==============================================================================
/** The analogue prototype's gain at the Nyquist frequency, derived here
    rather than read out of the header, which is what makes this a check on
    the transcription rather than a restatement of it.

    A prototype with peak `g` at `w0`, reference `g0` and bandwidth `dw`
    measured where the gain is `gb` has squared magnitude

        (g0^2 (w0^2-w^2)^2 + g^2 dw^2 r w^2) / ((w0^2-w^2)^2 + dw^2 r w^2)

    with `r = (gb^2-g0^2)/(g^2-gb^2)`, which follows from asking where that
    expression equals `gb^2`: the two roots come out a bandwidth apart. The
    whole idea of the correction is to prescribe that number at Nyquist, so a
    mistranscribed equation (21) disagrees with this.
*/
double analogueAtNyquist (double w0, double dw, double g, double gb, double g0)
{
    const double r = (gb * gb - g0 * g0) / (g * g - gb * gb);
    const double off = (w0 * w0 - pi * pi) * (w0 * w0 - pi * pi);
    const double spread = pi * pi * dw * dw * r;

    return std::sqrt ((g0 * g0 * off + g * g * spread) / (off + spread));
}

/** Where a design's magnitude crosses a gain, bisected between two
    frequencies, or -1 if it does not cross between them.

    Found from the realised filter rather than by rearranging the design
    equations, because a bandedge computed out of the same algebra that placed
    it would agree with itself however wrong both were. */
double crossing (const Fx::BellDesign& d, double lo, double hi, double target)
{
    const bool loAbove = magnitude (d, lo) > target;

    if (loAbove == (magnitude (d, hi) > target))
        return -1.0;

    for (int i = 0; i < 80; ++i)
    {
        const double mid = 0.5 * (lo + hi);

        if ((magnitude (d, mid) > target) == loAbove)
            lo = mid;
        else
            hi = mid;
    }

    return 0.5 * (lo + hi);
}

/** All five conditions that determine the filter, everywhere a band can be
    set, rather than three of them at one setting.

    A digital biquad has five degrees of freedom and this design claims five
    things: the gain at DC, the gain at Nyquist, the gain at the centre, the
    gain at both bandedges, and the distance between those bandedges. Satisfy
    all five and it is the right filter whatever happened to the square roots
    on the way out of the scan the equations were transcribed from. Three of
    them at one setting, which is what this file checked before, leaves the
    two that say what Q means unasserted: `dw-times-q` and `bandedge-three-db`
    were caught only by the five hand picked cases below, so a slip that
    happened to miss those five settings would have gone through.

    The bandedges are also the two conditions that cannot be satisfied by
    accident. The gain at DC is one line of the design and the gain at
    Nyquist is `m0` by the structure of the filter, but the bandedge gain and
    the bandwidth come out of equations (16), (17) and (19) together.

    Randomised over the clamped space `Svf::set` allows, at every rate a
    device can open at, in both directions of gain.
*/
void allFiveDesignConditionsHold()
{
    std::printf ("the five design conditions, over the reachable space\n");

    const double rates[] { 44100.0, 48000.0, 96000.0, 192000.0 };

    std::mt19937_64 rng (20261104);
    std::uniform_real_distribution<double> unit (0.0, 1.0);

    double worstDc = 0.0, worstNyquist = 0.0, worstCentre = 0.0;
    double worstEdge = 0.0, worstWidth = 0.0;
    int designs = 0, declined = 0;

    for (int trial = 0; trial < 3000; ++trial)
    {
        const double rate = rates[trial % 4];

        // Log uniform in both, because the awkward settings are at the ends
        // and a linear draw almost never visits them.
        const double hz = std::pow (10.0, 1.0 + unit (rng)
                                              * (std::log10 (rate * 0.49) - 1.0));
        const double q = std::pow (10.0, std::log10 (0.025)
                                           + unit (rng) * (std::log10 (100.0)
                                                             - std::log10 (0.025)));
        const double gainDb = (unit (rng) < 0.5 ? 1.0 : -1.0) * (0.5 + 119.5 * unit (rng));

        const auto d = Fx::matchedBell (hz, q, gainDb, rate);

        if (! d.matched)
        {
            ++declined;
            continue;
        }

        ++designs;

        const double w0 = 2.0 * pi * hz / rate;
        const double dw = w0 / q;

        // A cut is the reciprocal of its boost at every frequency, so every
        // target below is the boost's target with the sign of the gain on it.
        const double edgeDb = 0.5 * gainDb;
        const double edge = std::pow (10.0, edgeDb / 20.0);

        worstDc = std::max (worstDc, std::fabs (db (magnitude (d, 1.0e-13))));
        worstCentre = std::max (worstCentre, std::fabs (db (magnitude (d, w0)) - gainDb));

        // Equation (21) against the analogue gain derived above, which is the
        // one assertion here that does not need the paper.
        const double boostG = std::pow (10.0, std::fabs (gainDb) / 20.0);
        const double boostGb = std::sqrt (boostG);
        const double wantNyquist = (gainDb > 0.0 ? 1.0 : -1.0)
                                     * db (analogueAtNyquist (w0, dw, boostG, boostGb, 1.0));

        worstNyquist = std::max (worstNyquist, std::fabs (db (d.m0) - wantNyquist));

        // The bandedges. Below the centre for a boost the magnitude rises
        // through the edge gain exactly once, and above it falls through it
        // exactly once, because feasibility is the condition that puts the
        // upper edge inside the band.
        const double lo = crossing (d, 1.0e-16, w0, edge);
        const double hi = crossing (d, w0, pi * (1.0 - 1.0e-15), edge);

        if (lo < 0.0 || hi < 0.0)
        {
            fail ("a bandedge is missing at " + show (hz) + " Hz, Q " + show (q)
                    + ", " + show (gainDb) + " dB, " + show (rate) + " Hz");
            return;
        }

        worstEdge = std::max (worstEdge,
                              std::max (std::fabs (db (magnitude (d, lo)) - edgeDb),
                                        std::fabs (db (magnitude (d, hi)) - edgeDb)));
        worstWidth = std::max (worstWidth, std::fabs ((hi - lo) - dw) / dw);
    }

    std::printf ("    %d designs, %d declined\n", designs, declined);
    std::printf ("    at DC %.2e dB, at Nyquist %.2e dB, at the centre %.2e dB,\n"
                 "    at the bandedges %.2e dB, bandwidth %.2e relative\n",
                 worstDc, worstNyquist, worstCentre, worstEdge, worstWidth);

    // Tolerances are the measured figures with a decade of room, not round
    // numbers: every one of these is an identity, so anything much above the
    // arithmetic's own noise is a fault rather than an approximation.
    //
    // The centre is the one that is not at 1e-11 with the rest, and it is
    // worth saying why rather than loosening it quietly. Equation (17) takes
    // the difference of two quantities that both approach `G^2`, so at the
    // top of the gain clamp it is subtracting numbers near 1e12 to get
    // something near 1e6 and loses ten of its sixteen digits. Measured at
    // 48 kHz against the gain asked for, worst over every centre and Q: 9e-12
    // dB at 1 dB of gain, 4e-11 at 12, 2.6e-09 at 48, 7e-07 at 96 and 1.1e-05
    // at 120, every one of them at a Q near the hundred the mixer clamps to.
    // A hundredth of a millionth of a decibel at a gain nothing musical uses
    // is not worth a reformulation, but a figure that grows with the gain is
    // worth having written down so the next person does not read it as an
    // error that could be anywhere.
    if (worstDc > 1.0e-9)       fail ("the gain at DC is not unity: off by " + show (worstDc) + " dB");
    if (worstNyquist > 1.0e-9)  fail ("the prescribed Nyquist gain is not the analogue prototype's: off by " + show (worstNyquist) + " dB");
    if (worstCentre > 1.0e-4)   fail ("the bell does not reach its gain at its centre: off by " + show (worstCentre) + " dB");
    if (worstEdge > 1.0e-7)     fail ("the bandedge gain is not the half gain point: off by " + show (worstEdge) + " dB");
    if (worstWidth > 1.0e-7)    fail ("the bandwidth between the bandedges is not w0/Q: off by " + show (worstWidth));

    if (designs < 1000)
        fail ("the sweep hardly designed anything: " + show (designs));
}

/** The centre is the most the band does anywhere.

    Not implied by the five conditions: a filter can pass through `G` at its
    centre and reach further somewhere else, and with a prescribed gain at
    Nyquist lifting the upper skirt that is exactly where it would happen. An
    equaliser whose +6 dB band boosts 7 dB somewhere is a gain staging fault
    that no amount of looking at the number would find.

    Measured against the response at the centre rather than against the gain
    asked for, which are the same number to within the arithmetic noise the
    check above measures. Against the asked for gain this reads 1.2e-05 dB of
    overshoot at a Q of 100 and 120 dB, which is that noise and not the
    response going anywhere: the claim worth making is that nothing beats the
    centre, and it holds at the clamp as well as in the middle.
*/
void theCentreIsTheHighestPoint()
{
    std::printf ("the centre is the extreme of the response\n");

    const double rates[] { 44100.0, 48000.0, 96000.0, 192000.0 };
    double worst = -1000.0;
    std::string where;

    for (double rate : rates)
        for (double q : { 0.025, 0.1, 0.5, 0.70710678118654752, 1.0, 4.0, 20.0, 100.0 })
            for (double gainDb : { 0.5, 3.0, 12.0, 48.0, 120.0, -0.5, -3.0, -12.0, -48.0, -120.0 })
                for (double hz = 10.0; hz < rate * 0.49; hz *= 1.3)
                {
                    const auto d = Fx::matchedBell (hz, q, gainDb, rate);

                    if (! d.matched)
                        continue;

                    // Coarsely, then refined around wherever the coarse scan
                    // peaked. A single scan fine enough to sit on top of a Q
                    // of 100 at 120 dB is fifteen thousand steps per design,
                    // and the refinement gets closer than that scan can in
                    // sixty more evaluations.
                    double extreme = 0.0, bestW = 1.0e-6, previous = 1.0e-6;

                    for (double w = 1.0e-6; w < pi; w *= 1.01)
                    {
                        const double here = std::fabs (db (magnitude (d, w)));

                        if (here > extreme)
                        {
                            extreme = here;
                            bestW = w;
                        }

                        previous = w;
                    }

                    double lo = std::max (1.0e-9, bestW / 1.02);
                    double hi = std::min (pi * (1.0 - 1.0e-12), bestW * 1.02);

                    for (int i = 0; i < 60 && previous > 0.0; ++i)
                    {
                        const double a = lo + (hi - lo) / 3.0;
                        const double b = hi - (hi - lo) / 3.0;
                        const double fa = std::fabs (db (magnitude (d, a)));
                        const double fb = std::fabs (db (magnitude (d, b)));

                        extreme = std::max (extreme, std::max (fa, fb));

                        if (fa > fb)
                            hi = b;
                        else
                            lo = a;
                    }

                    const double over = extreme - std::fabs (db (magnitude (d, 2.0 * pi * hz / rate)));

                    if (over > worst)
                    {
                        worst = over;
                        where = show (hz) + " Hz, Q " + show (q) + ", " + show (gainDb)
                                  + " dB at " + show (rate) + " Hz";
                    }
                }

    std::printf ("    worst overshoot of the gain at the centre %+.3e dB, at %s\n",
                 worst, where.c_str());

    // The scan approaches a peak from below rather than over it, so anything
    // much above the arithmetic's own noise is the response genuinely going
    // past its own centre. Measured worst 3.2e-09 dB, at a Q of 100 and
    // 120 dB of cut, which is the same conditioning the check above measures.
    if (worst > 1.0e-7)
        fail ("the response goes past its gain at the centre by " + show (worst)
                + " dB, at " + where);
}

/** The correction is applied wherever the predicate says it will be, and
    nowhere else.

    This is the check the file was missing entirely, and the reason is worth
    recording. `isMatched()` was read twice in 493 lines: once to require it
    true at five hand picked settings, and once to require it false above the
    boundary. So the correction could be declined for every band with Q above
    10, or below 0.5, or at every sample rate above 48 kHz, and nothing here
    would have failed. All three were tried as deliberate breakages and all
    three survived. A band quietly running the conventional bell is still a
    bell, five decibels from what its own number says, and `isMatched()`
    reports the fallback honestly, so there is nothing to see from outside
    except the response.

    Both directions, because a predicate that says yes and a design that
    declines is the fallback firing where the equaliser's curve says it will
    not, and the other way round is a design running past its own feasibility
    condition.
*/
void theCorrectionIsAppliedWhereItShouldBe()
{
    std::printf ("the correction is applied where the predicate says\n");

    const double rates[] { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };

    int agreed = 0, matched = 0, declined = 0, disagreed = 0;

    for (double rate : rates)
        for (double q = 0.025; q <= 100.0; q *= 1.1)
            for (double hz = 10.0; hz < rate * 0.49; hz *= 1.1)
                for (double gainDb : { 0.5, 6.0, 24.0, -0.5, -6.0, -24.0 })
                {
                    const bool predicted = Fx::bellIsMatchable (hz, q, rate);
                    const auto d = Fx::matchedBell (hz, q, gainDb, rate);

                    if (d.matched == predicted)
                    {
                        ++agreed;
                        d.matched ? ++matched : ++declined;
                        continue;
                    }

                    // The two disagree only where the closed form and the
                    // feasibility test inside the design round differently,
                    // which is a band or so wide at the boundary itself.
                    const double boundary = 0.25 * rate
                                              * (std::sqrt (1.0 / (q * q) + 4.0) - 1.0 / q);

                    if (std::fabs (hz / boundary - 1.0) > 1.0e-9 && ++disagreed < 6)
                        fail ("the predicate said " + std::string (predicted ? "yes" : "no")
                                + " and the design said the other, at " + show (hz)
                                + " Hz, Q " + show (q) + ", " + show (gainDb) + " dB, "
                                + show (rate) + " Hz, boundary " + show (boundary));
                }

    std::printf ("    %d settings, %d matched, %d declined, %d disagreements\n",
                 agreed + disagreed, matched, declined, disagreed);

    // Both branches have to be well populated or the agreement is cheap.
    if (matched < 5000 || declined < 500)
        fail ("the sweep did not exercise both branches: " + show (matched)
                + " matched and " + show (declined) + " declined");
}

/** A bandedge gain other than the half gain point, which is the only way to
    reach four of the guards in the header.

    `matchedBellBoost` takes the peak, bandedge and reference gains
    separately, and the header says why: the paper's own worked example uses a
    bandedge nine decibels up on a twelve decibel boost, which is neither
    convention anybody ships. Nothing in this file called it with anything but
    the paper's one example and the half gain point, so the design's own
    feasibility test, its `aSquared` and `wSquared` guards and its closing
    `isfinite` sweep were all unreachable: removing any of the four changed
    nothing. They are each other's backstop, which is a good arrangement and
    not one to find out about by accident.

    `bellIsMatchable`'s closed form does not apply here. It is derived with
    the bandedge at the half gain point, where the ratio `r` is exactly `1/G`
    and the gain cancels, so with a free bandedge the only feasibility test is
    the one inside the design. Hence the shape of the assertion: whatever it
    decides, a design it reports as matched has to satisfy the five
    conditions, and a triple it cannot do has to be declined rather than
    approximated.
*/
void aFreeBandedgeGainIsStillRightOrDeclined()
{
    std::printf ("a bandedge gain the caller chose\n");

    std::mt19937_64 rng (765);
    std::uniform_real_distribution<double> unit (0.0, 1.0);

    int ok = 0, declined = 0;
    double worst = 0.0;

    for (int trial = 0; trial < 4000; ++trial)
    {
        const double w0 = (0.001 + 0.95 * unit (rng)) * pi;
        const double dw = (0.0005 + 1.2 * unit (rng)) * pi;

        // Gains in any order, including the orders the design has to refuse:
        // a bandedge above the peak, a reference above the bandedge, and the
        // degenerate cases where two of them are equal.
        const double g0Db = -24.0 + 48.0 * unit (rng);
        const double gDb = g0Db + (-6.0 + 42.0 * unit (rng));
        const double gbDb = g0Db + (gDb - g0Db) * (-0.2 + 1.4 * unit (rng));

        const double g0 = std::pow (10.0, g0Db / 20.0);
        const double g = std::pow (10.0, gDb / 20.0);
        const double gb = std::pow (10.0, gbDb / 20.0);

        const auto d = Fx::matchedBellBoost (w0, dw, g, gb, g0);

        if (! d.matched)
        {
            ++declined;
            continue;
        }

        ++ok;

        const double lo = crossing (d, 1.0e-16, w0, gb);
        const double hi = crossing (d, w0, pi * (1.0 - 1.0e-15), gb);

        if (lo < 0.0 || hi < 0.0)
        {
            fail ("a design with a free bandedge has no bandedge at w0 "
                    + show (w0 / pi) + " pi, dw " + show (dw / pi) + " pi");
            return;
        }

        const double errors[]
        {
            std::fabs (db (magnitude (d, 1.0e-13)) - db (g0)),
            std::fabs (db (d.m0) - db (analogueAtNyquist (w0, dw, g, gb, g0))),
            std::fabs (db (magnitude (d, w0)) - db (g)),
            std::fabs (db (magnitude (d, lo)) - db (gb)),
            std::fabs (db (magnitude (d, hi)) - db (gb)),
            std::fabs ((hi - lo) - dw) / dw * 20.0
        };

        for (double e : errors)
            worst = std::max (worst, e);
    }

    std::printf ("    %d designed, %d declined, worst design condition %.2e\n",
                 ok, declined, worst);

    if (worst > 1.0e-6)
        fail ("a design with a chosen bandedge gain does not satisfy its own"
              " conditions: off by " + show (worst));

    if (ok < 500 || declined < 500)
        fail ("the sweep did not exercise both branches: " + show (ok)
                + " designed and " + show (declined) + " declined");
}

/** Nothing anybody can pass in produces a design rather than a refusal.

    The guards exist because a caller is a knob and a knob can be at either
    end of itself. `Svf::set` clamps, so these are not reachable through it
    today, which is the reason to pin them now: the next caller may not.
*/
void nonsenseIsRefused()
{
    std::printf ("arguments that are not settings\n");

    struct Case { double hz, q, gainDb, rate; const char* what; };

    const Case cases[]
    {
        { 1000.0,  0.0, 6.0, 48000.0, "a Q of zero" },
        { 1000.0, -1.0, 6.0, 48000.0, "a negative Q" },
        {    0.0,  1.0, 6.0, 48000.0, "a centre at DC" },
        { -100.0,  1.0, 6.0, 48000.0, "a negative centre" },
        { 1000.0,  1.0, 6.0,     0.0, "no sample rate" },
        { 1000.0,  1.0, 6.0, -48000.0, "a negative sample rate" },
        { 24000.0, 1.0, 6.0, 48000.0, "a centre at Nyquist" },
        { 30000.0, 1.0, 6.0, 48000.0, "a centre above Nyquist" },
        { 1000.0,  1.0, 0.0, 48000.0, "a flat band" }
    };

    for (const auto& c : cases)
    {
        const auto d = Fx::matchedBell (c.hz, c.q, c.gainDb, c.rate);

        if (d.matched)
            fail (std::string (c.what) + " produced a design");

        if (Fx::bellIsMatchable (c.hz, c.q, c.rate) && c.gainDb != 0.0 && c.hz < c.rate * 0.5)
            fail (std::string (c.what) + " is reported as matchable");
    }

    // And the one the predicate has to let through: a flat band is matchable
    // as a frequency and a Q, and has nothing to match as a gain.
    if (! Fx::bellIsMatchable (1000.0, 1.0, 48000.0))
        fail ("an ordinary band is reported as not matchable");

    std::printf ("    %d refused\n", (int) (sizeof (cases) / sizeof (cases[0])));
}

} // namespace

int main()
{
    std::printf ("Fx::BellMatch\n\n");

    thePaperSOwnNumbers();
    theDesignConditionsHold();
    allFiveDesignConditionsHold();
    aFreeBandedgeGainIsStillRightOrDeclined();
    theCentreIsTheHighestPoint();
    itIsCloserToTheProtoype();
    theCorrectionIsAnImprovementAndWhereItIsNot();
    theBoundaryIsTheClosedForm();
    theCorrectionIsAppliedWhereItShouldBe();
    aCutMirrorsItsBoost();
    outOfReachFallsBackAndSaysSo();
    nonsenseIsRefused();
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
