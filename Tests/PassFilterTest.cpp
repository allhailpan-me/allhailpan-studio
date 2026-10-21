// Checks Fx/PassFilter.h: the steep high pass and low pass an equaliser needs.
//
// The one pole section the odd orders are built on has its own file now,
// OnePoleTest.cpp, which measures all five of its responses out of the
// running section rather than off its two analytic curves. What is left here
// about the section is kept because the cascade leans on it: the two passes
// against their closed forms, the allpass, and the shelves.
//
// There is one claim and it is a closed form: a Butterworth filter of order N
// has magnitude 1/sqrt(1 + r^2N) and is 3.0103 decibels down at its cutoff
// whatever N is. So every check here compares against that, evaluated on the
// prewarped axis, rather than against the Q table the filter was written from.
//
// **Do not be tempted to test this by measuring decibels per octave.** The
// bilinear frequency axis makes that reading wrong in both directions
// depending on where it is taken: a six decibel per octave low pass at 1 kHz
// and 48 kHz measures 5.46 dB between 2 and 4 kHz, because the asymptote is
// not reached yet, and 9.49 dB between 8 and 16 kHz, because of the warping
// near Nyquist. Both are the same correct filter, and a test built on that
// reading would be loose enough to say nothing. Those two numbers are asserted
// below, so that the trap is documented rather than rediscovered.
//
// The check that earns its place most is the last one: it pins the signature
// of the wrong construction. Cascading sections that are all at Q = 0.7071 is
// the obvious implementation, it is a real alignment, and it is not this one:
// it sits 6, 9 and 12 decibels down at the cutoff for orders 4, 6 and 8 where
// Butterworth is always 3.0103 down. Writing a constant into the Q table is
// the likeliest future breakage here and nothing else in the suite would see
// it.

#include "Fx/OnePole.h"
#include "Fx/PassFilter.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

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

double db (double x) { return 20.0 * std::log10 (x); }

/** The Butterworth magnitude, on the prewarped axis.

    `r` is the ratio of the prewarped frequency to the prewarped cutoff, which
    is the axis a bilinear designed filter really lives on. Comparing on the
    plain frequency axis instead is what makes a correct filter look wrong
    near Nyquist.
*/
double butterworth (double at, double fc, double rate, int order, bool highpass)
{
    const double r = std::tan (pi * at / rate) / std::tan (pi * fc / rate);
    const double shaped = std::pow (highpass ? 1.0 / r : r, 2.0 * (double) order);

    return 1.0 / std::sqrt (1.0 + shaped);
}

//==============================================================================
/** The one pole section against its own two closed forms, and the statement
    that the pair is complementary. */
void theOnePoleIsExact()
{
    std::printf ("the one pole section\n");

    const double rate = 48000.0;
    const double fc = 1000.0;

    Fx::OnePole low, high;

    low.setSampleRate (rate);
    high.setSampleRate (rate);

    low.set (Fx::OnePole::Type::lowpass, fc);
    high.set (Fx::OnePole::Type::highpass, fc);

    double worstLow = 0.0, worstHigh = 0.0;

    const double g = std::tan (pi * fc / rate);

    for (double at = 1.0; at < rate * 0.49; at *= 1.02)
    {
        const double w = std::tan (pi * at / rate) / g;

        const std::complex<double> s (0.0, w);

        worstLow = std::max (worstLow,
                             std::fabs (db (low.magnitudeAt (at))
                                          - db (std::abs (1.0 / (1.0 + s)))));
        worstHigh = std::max (worstHigh,
                              std::fabs (db (high.magnitudeAt (at))
                                           - db (std::abs (s / (1.0 + s)))));

        // Whether the pair adds back up to the input is asked of the two
        // running sections in OnePoleTest. It cannot be asked here: the two
        // closed forms are 1/(1+s) and s/(1+s), and that they add to one is
        // an identity in s which is true however either section behaves.
    }

    std::printf ("    low pass off by %s dB, high pass off by %s dB\n",
                 show (worstLow).c_str(), show (worstHigh).c_str());

    if (worstLow > 1.0e-9 || worstHigh > 1.0e-9)
        fail ("the one pole is not its closed form");

    expectNear (db (low.magnitudeAt (fc)), -3.0103, 0.002,
                "the one pole low pass is not three decibels down at its cutoff");
    expectNear (db (high.magnitudeAt (fc)), -3.0103, 0.002,
                "the one pole high pass is not three decibels down at its cutoff");
}

/** The first order allpass is flat and turns the phase by half a cycle across
    the band, which is the property a phaser is built from. */
void theAllpassIsFlatAndTurnsHalfACycle()
{
    std::printf ("the first order allpass\n");

    const double rate = 48000.0;
    const double fc = 1000.0;

    Fx::OnePole all;
    all.setSampleRate (rate);
    all.set (Fx::OnePole::Type::allpass, fc);

    double worst = 0.0;

    for (double at = 1.0; at < rate * 0.4999; at *= 1.01)
        worst = std::max (worst, std::fabs (all.magnitudeAt (at) - 1.0));

    std::printf ("    flat to %s, phase %.3f pi at DC, %.3f pi at the corner,"
                 " %.3f pi near Nyquist\n", show (worst).c_str(),
                 all.phaseAt (0.0) / pi, all.phaseAt (fc) / pi,
                 all.phaseAt (rate * 0.4999) / pi);

    if (worst > 1.0e-12)
        fail ("the allpass is not flat: off by " + show (worst));

    // Smith's AP1 is pi at DC, pi/2 at the corner and nothing at Nyquist. The
    // other sign convention is also an allpass and puts a phaser's notches
    // somewhere else, so these are signed. They used to take the modulus,
    // which is exactly the direction being thrown away: negating the whole of
    // phaseAt passed. OnePoleTest measures the same thing off the output.
    expectNear (all.phaseAt (0.0), pi, 1.0e-9,
                "the allpass does not invert at DC");
    expectNear (all.phaseAt (fc), pi * 0.5, 1.0e-6,
                "the allpass is not a positive quarter cycle at its corner");
    expectNear (all.phaseAt (rate * 0.4999), 0.0, 1.0e-3,
                "the allpass does not come back to nothing at Nyquist");
}

/** A one pole shelf's corner lands where it was put, whatever its gain, and
    its two ends are its two gains. The prewarping for this is the square root
    of the full gain rather than the fourth root a two pole shelf uses, and
    getting it wrong slides the corner with the amount of boost. */
void theOnePoleShelvesSitWhereTheyWerePut()
{
    std::printf ("the one pole shelves\n");

    const double rate = 48000.0;
    const double fc = 500.0;

    for (double gainDb : { -18.0, -6.0, 6.0, 18.0 })
    {
        for (auto type : { Fx::OnePole::Type::lowShelf, Fx::OnePole::Type::highShelf })
        {
            Fx::OnePole shelf;
            shelf.setSampleRate (rate);
            shelf.set (type, fc, gainDb);

            const bool isLow = type == Fx::OnePole::Type::lowShelf;

            // The two ends are the two gains.
            expectNear (db (shelf.magnitudeAt (0.5)), isLow ? gainDb : 0.0, 0.02,
                        "a one pole shelf's bottom end is wrong at "
                          + show (gainDb) + " dB");
            expectNear (db (shelf.magnitudeAt (rate * 0.4999)),
                        isLow ? 0.0 : gainDb, 0.02,
                        "a one pole shelf's top end is wrong at "
                          + show (gainDb) + " dB");

            // And the half gain point is at the corner, which is what the
            // frequency on the control is supposed to mean.
            expectNear (db (shelf.magnitudeAt (fc)), gainDb * 0.5, 0.01,
                        std::string (isLow ? "a low" : "a high")
                          + " shelf's corner is not at half its gain at "
                          + show (gainDb) + " dB");
        }
    }

    std::printf ("    both ends are the two gains and the corner is at half of"
                 " it, from -18 to +18 dB\n");
}

//==============================================================================
/** Every slope against the closed form, and three decibels down at the
    cutoff at every order. */
void everySlopeIsButterworth()
{
    std::printf ("the cascade against the Butterworth magnitude\n");

    const double rate = 48000.0;
    const double fc = 1000.0;

    for (int order = 1; order <= Fx::PassFilter::maxOrder; ++order)
    {
        for (auto type : { Fx::PassFilter::Type::lowpass,
                           Fx::PassFilter::Type::highpass })
        {
            Fx::PassFilter f;
            f.setSampleRate (rate);
            f.set (type, fc, order);

            const bool highpass = type == Fx::PassFilter::Type::highpass;

            double worst = 0.0;

            for (double at = 1.0; at < rate * 0.4; at *= 1.01)
                worst = std::max (worst,
                                  std::fabs (db (f.magnitudeAt (at))
                                               - db (butterworth (at, fc, rate,
                                                                  order, highpass))));

            if (! highpass)
                std::printf ("    %3.0f dB/oct: low pass off by %-12s",
                             f.slopeDbPerOctave(), show (worst).c_str());
            else
                std::printf ("high pass off by %-12s at the cutoff %+.4f dB\n",
                             show (worst).c_str(), db (f.magnitudeAt (fc)));

            if (worst > 1.0e-8)
                fail (std::string (highpass ? "the high pass" : "the low pass")
                        + " of order " + show (order)
                        + " is not Butterworth: off by " + show (worst) + " dB");

            expectNear (db (f.magnitudeAt (fc)), -3.0103, 0.001,
                        std::string (highpass ? "the high pass" : "the low pass")
                          + " of order " + show (order)
                          + " is not three decibels down at its cutoff");
        }
    }
}

/** The section damping table in the header, against the pole angle formula,
    so that the table cannot drift from the thing it was computed from. */
void theSectionTableIsThePoleAngles()
{
    std::printf ("the section damping values\n");

    struct Expected { int order; std::vector<double> q; };

    const Expected table[]
    {
        { 2,  { 0.707107 } },
        { 3,  { 1.000000 } },
        { 4,  { 1.306563, 0.541196 } },
        { 6,  { 1.931852, 0.707107, 0.517638 } },
        { 8,  { 2.562915, 0.899976, 0.601345, 0.509796 } },
        { 12, { 3.830649, 1.306563, 0.821340, 0.630236, 0.541196, 0.504314 } },
        { 16, { 5.101149, 1.722447, 1.060678, 0.788155, 0.646822, 0.566944,
                0.522499, 0.502419 } }
    };

    for (const auto& e : table)
        for (std::size_t i = 0; i < e.q.size(); ++i)
            expectNear (Fx::PassFilter::sectionQ (e.order, (int) i), e.q[i], 1.0e-6,
                        "section " + show ((double) i) + " of order "
                          + show ((double) e.order) + " has the wrong damping");

    std::printf ("    order 16's highest section is at Q %.6f, which really does"
                 " ring\n", Fx::PassFilter::sectionQ (16, 0));

    if (! (Fx::PassFilter::sectionQ (16, 0) > 5.0))
        fail ("the steepest section is no longer resonant, so this is not"
              " Butterworth any more");
}

/** The wrong construction's signature, so it cannot quietly replace the right
    one.

    Cascading identical sections at Q = 0.7071 is a real filter, it is what a
    Linkwitz-Riley crossover is, and it droops at the cutoff by three decibels
    per section rather than sitting at 3.0103 whatever the order. Asserted as
    exact numbers rather than as an inequality: the point is that these are
    what a constant in the damping table would produce.
*/
void theWrongConstructionDroops()
{
    std::printf ("what a constant damping table would do instead\n");

    const double rate = 48000.0;
    const double fc = 1000.0;

    struct Case { int sections; double atCutoff; };

    const Case cases[] { { 2, -6.021 }, { 3, -9.031 }, { 4, -12.042 } };

    for (const auto& c : cases)
    {
        double product = 1.0;

        for (int i = 0; i < c.sections; ++i)
        {
            Fx::Svf one;
            one.setSampleRate (rate);
            one.set (Fx::Svf::Type::lowpass, fc, 0.70710678118654752);

            product *= one.magnitudeAt (fc);
        }

        Fx::PassFilter right;
        right.setSampleRate (rate);
        right.set (Fx::PassFilter::Type::lowpass, fc, c.sections * 2);

        std::printf ("    %d sections all at Q 0.7071: %+.3f dB at the cutoff,"
                     " against %+.4f for Butterworth\n",
                     c.sections, db (product), db (right.magnitudeAt (fc)));

        expectNear (db (product), c.atCutoff, 0.002,
                    "the drooping construction's signature has moved");

        if (std::fabs (db (right.magnitudeAt (fc)) - db (product)) < 1.0)
            fail ("the cascade has become the drooping construction at order "
                  + show ((double) (c.sections * 2)));
    }
}

/** The trap this file's header is about, asserted so that nobody reinvents
    the test that would have been useless. */
void decibelsPerOctaveIsNotTheMeasurement()
{
    std::printf ("why the slope is not measured in decibels per octave\n");

    const double rate = 48000.0;
    const double fc = 1000.0;

    Fx::PassFilter f;
    f.setSampleRate (rate);
    f.set (Fx::PassFilter::Type::lowpass, fc, 1);

    const double low = db (f.magnitudeAt (2000.0)) - db (f.magnitudeAt (4000.0));
    const double high = db (f.magnitudeAt (8000.0)) - db (f.magnitudeAt (16000.0));

    std::printf ("    a six decibel per octave filter measures %.2f dB between"
                 " 2 and 4 kHz and %.2f between 8 and 16\n", low, high);

    expectNear (low, 5.46, 0.02, "the low reading of the slope has moved");
    expectNear (high, 9.49, 0.02, "the high reading of the slope has moved");
}

/** The running filter is the curve it draws, which everything above relies
    on: `magnitudeAt` is analytic and `process` is a recursion, and a test
    that only ever asked the first one would not notice the second being
    broken. */
void runningItMatchesTheCurve()
{
    std::printf ("the running filter against its own curve\n");

    const double rate = 48000.0;
    double worst = 0.0;

    for (int order : { 1, 2, 3, 8, 16 })
        for (auto type : { Fx::PassFilter::Type::lowpass,
                           Fx::PassFilter::Type::highpass })
            for (double at : { 200.0, 1000.0, 5000.0 })
            {
                Fx::PassFilter f;
                f.setSampleRate (rate);
                f.set (type, 1000.0, order);

                // A mean square over whole cycles rather than a peak: these
                // filters delay, and at the top of the band a fraction of a
                // sample of delay moves every sampled point off the top of
                // the wave. The wavefolder's probe read 0.69 dB low that way.
                const int settle = (int) (rate * 0.4);
                const int measure = (int) (rate * 0.4);

                double sum = 0.0;

                for (int i = 0; i < settle + measure; ++i)
                {
                    const double in = std::sin (2.0 * pi * at * (double) i / rate);
                    const double out = f.process (in);

                    if (i >= settle)
                        sum += out * out;
                }

                const double measured = std::sqrt (sum / (double) measure / 0.5);

                if (measured > 1.0e-7)
                    worst = std::max (worst,
                                      std::fabs (db (measured)
                                                   - db (f.magnitudeAt (at))));
            }

    std::printf ("    worst disagreement %s dB\n", show (worst).c_str());

    if (worst > 0.01)
        fail ("the running filter is not the curve it draws: off by "
              + show (worst) + " dB");
}

void nothingUnreasonableBreaksIt()
{
    std::printf ("out of range use\n");

    for (double rate : { 0.0, -1.0, 8000.0, 192000.0 })
    {
        Fx::PassFilter f;
        f.setSampleRate (rate);

        for (int order : { -4, 0, 1, 16, 99 })
            for (double hz : { -100.0, 0.0, 1.0e-9, 1.0e9 })
            {
                f.set (Fx::PassFilter::Type::highpass, hz, order);
                f.reset();

                for (int i = 0; i < 200; ++i)
                    if (! std::isfinite (f.process ((i % 7) * 0.3 - 1.0)))
                    {
                        fail ("the output stopped being finite");

                        return;
                    }

                if (! std::isfinite (f.magnitudeAt (1000.0)))
                {
                    fail ("the curve stopped being finite");

                    return;
                }
            }
    }

    std::printf ("    survives nonsense at every sample rate\n");
}

} // namespace

int main()
{
    std::printf ("Fx::PassFilter and Fx::OnePole\n\n");

    theOnePoleIsExact();
    theAllpassIsFlatAndTurnsHalfACycle();
    theOnePoleShelvesSitWhereTheyWerePut();
    everySlopeIsButterworth();
    theSectionTableIsThePoleAngles();
    theWrongConstructionDroops();
    decibelsPerOctaveIsNotTheMeasurement();
    runningItMatchesTheCurve();
    nothingUnreasonableBreaksIt();

    std::printf ("\n");

    if (failures > 0)
    {
        std::printf ("%d check(s) failed.\n", failures);
        return 1;
    }

    std::printf ("All checks passed.\n");
    return 0;
}
