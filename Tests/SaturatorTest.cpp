// Checks Fx/Saturator.h.
//
// Four kinds of claim, because a saturator makes four.
//
// That each curve is the curve it says it is. These are not invented shapes:
// the tangent is a transistor pair's transfer function, the cubic is the soft
// clipper whose slope reaches zero exactly where it meets the limit, and the
// clip is a clip. Each is compared against its own definition written out
// again, which is the only description that cannot drift from the code.
//
// That the antiderivatives are the antiderivatives. The whole antialiasing
// stands on that and on nothing else, and they are checked against the
// derivative computed numerically rather than against a second copy of the
// algebra. This check is in here because its absence cost real time once: the
// wavefolder shipped a second antiderivative wrong by a whole term, it still
// ran, and the only symptom was twenty decibels more aliasing.
//
// That the two controls do what their names say and do not do each other's
// job. Drive must change the timbre and not the level, which is the one thing
// that makes a drive control usable, and bias must change the harmonics and
// not the level either. Both are easy to get subtly wrong and neither failure
// looks like a bug: the knob works, it just also does something else.
//
// And that it does not alias. Measured, per curve, rather than argued from the
// order of the antialiasing, because the order is a claim about the method and
// the measurement is a fact about the output.

#include "Fx/Saturator.h"
#include "RealFFT.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
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
    char buffer[32];
    std::snprintf (buffer, sizeof (buffer), "%.2f", v);
    return buffer;
}

constexpr double pi = 3.14159265358979323846;
constexpr double rate = 48000.0;

using Curve = Fx::Saturator::Curve;

const char* nameOf (Curve c)
{
    switch (c)
    {
        case Curve::soft:  return "soft";
        case Curve::cubic: return "cubic";
        case Curve::hard:  return "hard";
    }

    return "?";
}

const Curve everyCurve[] = { Curve::soft, Curve::cubic, Curve::hard };

//==============================================================================
/** How much of the output is not a harmonic of the note, in decibels.

    The same method the wavefolder's test uses, so the two sets of numbers can
    be read against each other. The note is deliberately off the bin grid: a
    signal that repeats exactly still repeats exactly after its harmonics have
    folded, so on the grid the folded partials would land on harmonics and be
    invisible.

    A four term Blackman Harris window has sidelobes 92 dB down at their peak,
    which would be the floor of this measurement if the bins beside each
    harmonic were read. They are not: everything within eight bins of a
    harmonic is excluded, and the window's leakage has fallen a long way by
    then. `theMeasurementCanSeeDownFarEnough` establishes where the real floor
    is rather than leaving it to be assumed.

    Only what lands below 20 kHz is counted, and that is a deliberate part of
    the claim rather than a convenience. The oversampler's filters are
    specified to stop at the oversampled rate minus 20 kHz rather than at
    Nyquist, so that what folds lands in the top few kilohertz where nothing
    can hear it. Counting that band finds a floor around -50 dB that no amount
    of oversampling moves, which is a true number about a thing that does not
    matter, and it hides the real behaviour underneath it.
*/
double aliasFloorDb (const std::vector<double>& rendered, double frequency,
                     double highestOfInterest = 20000.0)
{
    const int n = (int) rendered.size();

    std::vector<float> windowed ((std::size_t) n);

    const double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;

    for (int i = 0; i < n; ++i)
    {
        const double t = 2.0 * pi * (double) i / (double) (n - 1);

        windowed[(std::size_t) i] = (float) (rendered[(std::size_t) i]
            * (a0 - a1 * std::cos (t) + a2 * std::cos (2.0 * t) - a3 * std::cos (3.0 * t)));
    }

    RealFFT fft (n);
    std::vector<std::complex<double>> bins ((std::size_t) fft.getNumBins());
    fft.forward (windowed.data(), bins.data());

    const double binHz = rate / (double) n;

    double loudest = 0.0, stray = 0.0;
    int strayBins = 0;

    for (int k = 10; k < (int) bins.size(); ++k)
    {
        const double hz = (double) k * binHz;
        const double magnitude = std::abs (bins[(std::size_t) k]);
        const double nearest = std::floor (hz / frequency + 0.5);

        if (nearest >= 1.0 && std::fabs (hz - nearest * frequency) / binHz <= 8.0)
        {
            loudest = std::max (loudest, magnitude);
            continue;
        }

        if (hz > highestOfInterest)
            continue;

        ++strayBins;
        stray = std::max (stray, magnitude);
    }

    if (strayBins < 30 || ! (loudest > 0.0))
        return -1000.0;

    return 20.0 * std::log10 (stray / loudest + 1e-300);
}

/** Drives a sine through a fresh saturator and hands back what came out,
    with the first stretch thrown away so the oversampler's filters are full
    and the measurement is of the steady state rather than of the onset. */
std::vector<double> driveASine (Curve curve, double drive, double hz,
                                int count, double bias = 0.0, double level = 1.0)
{
    Fx::Saturator saturator;
    saturator.prepare (rate, 256);
    saturator.setCurve (curve);
    saturator.setDrive (drive);
    saturator.setBias (bias);

    const int discard = 1024;

    std::vector<double> block ((std::size_t) 256);
    std::vector<double> out;
    out.reserve ((std::size_t) count);

    int produced = 0;

    for (int written = 0; written < discard + count; written += 256)
    {
        for (int i = 0; i < 256; ++i)
        {
            const double t = (double) (written + i) / rate;
            block[(std::size_t) i] = level * std::sin (2.0 * pi * hz * t);
        }

        saturator.process (block.data(), 256);

        for (int i = 0; i < 256 && produced < discard + count; ++i, ++produced)
            if (produced >= discard)
                out.push_back (block[(std::size_t) i]);
    }

    return out;
}

//==============================================================================
/** Each curve is the curve it claims to be. */
void theCurvesAreWhatTheySay()
{
    std::printf ("the curves themselves\n");

    for (double x = -8.0; x <= 8.0; x += 0.0009765625)
    {
        // The tangent, against the library's own.
        if (std::fabs (Fx::Saturator::shape (Curve::soft, x) - std::tanh (x)) > 1e-15)
        {
            fail ("the soft curve is not a hyperbolic tangent at x = " + show (x));
            break;
        }

        // The cubic, written out again rather than referred to.
        const double expectedCubic = std::fabs (x) >= 1.0
                                       ? (x < 0.0 ? -1.0 : 1.0)
                                       : 1.5 * x - 0.5 * x * x * x;

        if (std::fabs (Fx::Saturator::shape (Curve::cubic, x) - expectedCubic) > 1e-15)
        {
            fail ("the cubic curve is wrong at x = " + show (x));
            break;
        }

        const double expectedHard = x < -1.0 ? -1.0 : (x > 1.0 ? 1.0 : x);

        if (std::fabs (Fx::Saturator::shape (Curve::hard, x) - expectedHard) > 1e-15)
        {
            fail ("the hard curve is not a clip at x = " + show (x));
            break;
        }
    }

    for (auto curve : everyCurve)
    {
        // Odd, bounded, and rising everywhere, which is what makes a curve a
        // saturation rather than a folder. A folder turns back; this must not.
        double previous = Fx::Saturator::shape (curve, -40.0);

        for (double x = -40.0; x <= 40.0; x += 0.001)
        {
            const double y = Fx::Saturator::shape (curve, x);
            const double mirrored = Fx::Saturator::shape (curve, -x);

            if (std::fabs (y + mirrored) > 1e-12)
            {
                fail (std::string (nameOf (curve)) + " is not odd at x = " + show (x));
                break;
            }

            if (std::fabs (y) > 1.0 + 1e-12)
            {
                fail (std::string (nameOf (curve)) + " left minus one to one at x = "
                      + show (x));
                break;
            }

            if (y < previous - 1e-12)
            {
                fail (std::string (nameOf (curve)) + " turned back at x = " + show (x)
                      + ", which makes it a folder rather than a saturation");
                break;
            }

            previous = y;
        }

        // And the slope at the origin is the one it reports, which is what the
        // level compensation is built on.
        const double h = 1e-6;
        const double measured = (Fx::Saturator::shape (curve, h)
                                   - Fx::Saturator::shape (curve, -h)) / (2.0 * h);

        std::printf ("    %-6s slope at zero: reports %s, measures %s\n",
                     nameOf (curve),
                     show (Fx::Saturator::slopeAtZero (curve)).c_str(),
                     show (measured).c_str());

        if (std::fabs (measured - Fx::Saturator::slopeAtZero (curve)) > 1e-6)
            fail (std::string (nameOf (curve)) + " reports the wrong slope at zero");
    }

    // The cubic's slope has to reach zero exactly where it meets the limit, or
    // it has a corner there and is no gentler than the clip.
    {
        const double h = 1e-7;
        const double slope = (Fx::Saturator::shape (Curve::cubic, 1.0)
                                - Fx::Saturator::shape (Curve::cubic, 1.0 - h)) / h;

        std::printf ("    cubic slope arriving at the limit: %s\n", show (slope).c_str());

        if (std::fabs (slope) > 1e-5)
            fail ("the cubic meets its limit with a corner, slope " + show (slope));
    }
}

/** The antiderivatives are the antiderivatives, checked by differentiating
    them rather than by writing the algebra out twice. */
void theAntiderivativesAreAntiderivatives()
{
    std::printf ("the antiderivatives\n");

    for (auto curve : everyCurve)
    {
        if (Fx::Saturator::integral (curve, 0.0) != 0.0)
            fail (std::string (nameOf (curve)) + "'s integral is not zero at zero");

        if (Fx::Saturator::secondIntegral (curve, 0.0) != 0.0)
            fail (std::string (nameOf (curve)) + "'s second integral is not zero at zero");

        double worstFirst = 0.0, worstSecond = 0.0;
        double worstFirstAt = 0.0, worstSecondAt = 0.0;

        // Right across the join at one, and far enough out that the branch
        // past the limit is covered on both curves that have one.
        const double step = 1e-6;

        for (double x = -6.0; x <= 6.0; x += 0.00025)
        {
            const double slope = (Fx::Saturator::integral (curve, x + step)
                                    - Fx::Saturator::integral (curve, x - step))
                               / (2.0 * step);

            const double error = std::fabs (slope - Fx::Saturator::shape (curve, x));

            if (error > worstFirst)
            {
                worstFirst = error;
                worstFirstAt = x;
            }

            if (! Fx::Saturator::hasSecondIntegral (curve))
                continue;

            const double secondSlope = (Fx::Saturator::secondIntegral (curve, x + step)
                                          - Fx::Saturator::secondIntegral (curve, x - step))
                                     / (2.0 * step);

            const double secondError = std::fabs (secondSlope
                                                    - x * Fx::Saturator::shape (curve, x));

            if (secondError > worstSecond)
            {
                worstSecond = secondError;
                worstSecondAt = x;
            }
        }

        std::printf ("    %-6s first off by %.2e at x = %s",
                     nameOf (curve), worstFirst, show (worstFirstAt).c_str());

        if (Fx::Saturator::hasSecondIntegral (curve))
            std::printf (", second off by %.2e at x = %s",
                         worstSecond, show (worstSecondAt).c_str());
        else
            std::printf (", second not elementary");

        std::printf ("\n");

        // The tolerance is set by the central difference itself, which at a
        // step of a millionth is accurate to about the square of it on a
        // smooth function and much worse across a corner, which both
        // polynomial curves have in their second derivative at the limit.
        if (worstFirst > 2e-4)
            fail (std::string (nameOf (curve)) + "'s integral is not its integral: off by "
                  + show (worstFirst));

        if (worstSecond > 2e-4)
            fail (std::string (nameOf (curve))
                  + "'s second integral is not the integral of x times it: off by "
                  + show (worstSecond));
    }

    // And the tangent's first antiderivative holds up where the naive form
    // has lost its digits, which is the reason it is not written as log(cosh).
    for (double x : { 0.5, 5.0, 50.0, 400.0, 700.0, 1500.0 })
    {
        const double got = Fx::Saturator::integral (Curve::soft, x);

        // Past about 20 the tangent is one to within a part in 10^17, so the
        // integral is x minus log 2 to the same accuracy.
        const double expected = x > 20.0 ? x - 0.69314718055994530942
                                         : std::log (std::cosh (x));

        if (! std::isfinite (got) || std::fabs (got - expected) > 1e-12 * (1.0 + x))
            fail ("the tangent's integral is " + show (got) + " at x = " + show (x)
                  + " rather than " + show (expected));
    }

    std::printf ("    the tangent's integral is still finite at x = 1500\n");
}

/** The antialiasing must not change the curve.

    Fed a signal that barely moves between samples, the averaged form has to
    agree with the curve itself. Every bit of the aliasing performance comes
    from the averaging, so it would be easy to ship something that aliases
    beautifully and is the wrong shape, and nothing would say so except a
    listener who knew the curve.
*/
void aSlowSignalComesBackAsTheCurve()
{
    std::printf ("a slow signal\n");

    for (auto curve : everyCurve)
        for (double drive : { 0.5, 1.0, 4.0, 20.0 })
        {
            Fx::Saturator saturator;
            saturator.prepareAtRate (rate);
            saturator.setCurve (curve);
            saturator.setDrive (drive);

            const double makeup = 1.0 / std::fabs (Fx::Saturator::shape (curve, drive));

            double worst = 0.0;
            double previousWanted = 0.0;

            // A hundredth of a hertz: at 48 kHz that is five million samples
            // a cycle, so consecutive inputs differ in the tenth decimal
            // place and the averaged form has nothing to average.
            //
            // What is left is the delay, not an error. Averaging across the
            // step puts the answer half a sample behind where the curve is,
            // or a whole one for the second order form, so the output is the
            // curve of an input from a moment ago. The allowance is therefore
            // written in terms of how far the expected output moves in a
            // sample rather than as a fixed number: a fixed number would have
            // to be loose enough for the fastest drive and would then say
            // nothing at all about the slowest.
            for (int i = 0; i < 60000; ++i)
            {
                const double x = std::sin (2.0 * pi * 0.01 * (double) i / rate);
                const double got = saturator.processOneAtCurrentRate (x);
                const double want = Fx::Saturator::shape (curve, x * drive) * makeup;

                if (i > 10)
                {
                    const double movesBy = std::fabs (want - previousWanted);

                    worst = std::max (worst,
                                      std::fabs (got - want) - 1.2 * movesBy);
                }

                previousWanted = want;
            }

            std::printf ("    %-6s drive %-5s off the curve by %.2e more than its delay\n",
                         nameOf (curve), show (drive).c_str(), worst);

            // What is allowed on top of the delay is the kink. Both
            // polynomial curves meet their limit in a corner, and across a
            // corner the average of the curve over a window two samples wide
            // is genuinely not the curve at the middle of that window, by an
            // amount that does not shrink with the step. At a drive of twenty
            // that reaches two parts in a million, which is 114 dB down, and
            // the tangent, which has no corner anywhere, measures exactly
            // zero.
            if (worst > 1e-5)
                fail (std::string (nameOf (curve)) + " at drive " + show (drive)
                      + " is not its own curve on a slow signal, beyond the delay"
                        " the averaging costs and the corner in the curve");
        }
}

/** The antialiasing computes a particular average, and this is that average,
    worked out by numerical integration instead.

    This is the strongest check here, because it is the definition rather than
    a consequence of it. The first order form is the shaper averaged evenly
    across the step from the last input to this one; the second order form is
    the shaper averaged under a triangle two samples wide. With the input
    linear between samples, both are ordinary integrals that Simpson's rule
    can do to more decimal places than the closed forms need:

        first:   integral over t in [0,1] of g(x1 + t (x - x1))
        second:  the same weighted by (1 - t), plus the matching term going
                 back from x1 to x2

    Substituting u for the input turns the second of those into exactly the
    paper's quotient, which is the algebra the implementation uses. So the two
    should agree everywhere, including across the threshold where the
    implementation stops dividing and uses its fallbacks, and that crossing is
    the part worth checking: a fallback that is subtly wrong fires only on
    nearly steady signals, where almost nothing else is looking.
*/
void itComputesTheAverageItClaimsTo()
{
    std::printf ("against the average, integrated numerically\n");

    for (auto curve : everyCurve)
        for (double drive : { 0.4, 1.0, 6.0, 25.0 })
            for (double bias : { 0.0, 0.35 })
            {
                Fx::Saturator saturator;
                saturator.prepareAtRate (rate);
                saturator.setCurve (curve);
                saturator.setDrive (drive);
                saturator.setBias (bias);

                const double makeup = 1.0 / std::fabs (Fx::Saturator::shape (curve, drive));
                const bool second = Fx::Saturator::hasSecondIntegral (curve);

                // The shaper, which is the curve offset by the bias and
                // brought back through the origin.
                const auto g = [&] (double u)
                {
                    return Fx::Saturator::shape (curve, u + bias)
                             - Fx::Saturator::shape (curve, bias);
                };

                // Simpson over the segment from b to a, weighted or not.
                const auto average = [&] (double a, double b, bool triangular)
                {
                    const int steps = 2000;          // even, as Simpson needs
                    double sum = 0.0;

                    for (int i = 0; i <= steps; ++i)
                    {
                        const double t = (double) i / (double) steps;
                        const double weight = triangular ? (1.0 - t) : 1.0;
                        const int coefficient = (i == 0 || i == steps) ? 1
                                                                       : (i % 2 ? 4 : 2);

                        sum += (double) coefficient * weight * g (b + t * (a - b));
                    }

                    return sum / (3.0 * (double) steps);
                };

                double u1 = 0.0, u2 = 0.0;
                double worst = 0.0, worstStep = 0.0;

                // Steps spanning the threshold the implementation switches
                // on, which is a hundred thousandth, by a long way either
                // side of it.
                for (double step : { 1e-9, 1e-7, 1e-6, 5e-6, 9.9e-6, 1.01e-5,
                                     1e-4, 1e-3, 0.01, 0.1, 0.4 })
                    for (int i = 0; i < 6; ++i)
                    {
                        const double u = u1 + step * drive * (i % 2 ? -1.0 : 1.0);

                        const double got = saturator.processOneAtCurrentRate (u / drive);

                        const double want = second
                                              ? (average (u, u1, true)
                                                   + average (u2, u1, true)) * makeup
                                              : average (u, u1, false) * makeup;

                        if (std::fabs (got - want) > worst)
                        {
                            worst = std::fabs (got - want);
                            worstStep = step;
                        }

                        u2 = u1;
                        u1 = u;
                    }

                if (worst > 2e-6)
                    fail (std::string (nameOf (curve)) + " at drive " + show (drive)
                          + " bias " + show (bias) + " is not the average it claims:"
                            " off by " + show (worst) + " at a step of "
                          + show (worstStep));
            }

    std::printf ("    every curve, drive and bias matches the integral both sides"
                 " of the fallback\n");
}

/** Drive changes the timbre and not the level. */
void driveDoesNotChangeTheLevel()
{
    std::printf ("drive against level\n");

    for (auto curve : everyCurve)
    {
        std::printf ("    %-6s ", nameOf (curve));

        for (double drive : { 0.25, 1.0, 4.0, 16.0, 64.0 })
        {
            const auto rendered = driveASine (curve, drive, 220.0, 8192);

            double peak = 0.0;

            for (double v : rendered)
                peak = std::max (peak, std::fabs (v));

            std::printf ("%s:%s ", show (drive).c_str(), show (peak).c_str());

            // A full scale sine comes back at full scale, give or take the
            // overshoot of a band limited reconstruction. At the highest
            // drives the output is nearly square, and a square band limited
            // to 20 kHz rings about nine percent past its own flat top, which
            // is Gibbs and not a level error.
            if (! (peak > 0.90 && peak < 1.20))
                fail (std::string (nameOf (curve)) + " at drive " + show (drive)
                      + " peaks at " + show (peak) + " rather than near full scale");
        }

        std::printf ("\n");
    }

    // And a quiet signal at a low drive passes through untouched, which is the
    // other half of the same claim: a saturator has to be transparent when it
    // is not being asked to do anything.
    for (auto curve : everyCurve)
    {
        const double level = 0.001;
        const auto rendered = driveASine (curve, 0.02, 220.0, 4096, 0.0, level);

        double peak = 0.0;

        for (double v : rendered)
            peak = std::max (peak, std::fabs (v));

        const double gain = peak / level;

        std::printf ("    %-6s a quiet signal at the lowest drive: gain %s\n",
                     nameOf (curve), show (gain).c_str());

        if (! (gain > 0.97 && gain < 1.03))
            fail (std::string (nameOf (curve)) + " is not transparent when idle: gain "
                  + show (gain));
    }

    // And the makeup is one over the curve at the drive, with the bias having
    // no part in it. That is the whole of the claim that the two controls do
    // not interact, and it is worth pinning exactly rather than by eye: taken
    // at the drive plus the bias instead, which is the obvious alternative,
    // it is nearly invisible at a high drive and four decibels out at a low
    // one, so a listener would meet it as a bias knob that is also a volume
    // knob, but only sometimes.
    for (auto curve : everyCurve)
        for (double drive : { 0.3, 1.0, 4.0 })
            for (double bias : { -0.5, 0.0, 0.35 })
            {
                Fx::Saturator saturator;
                saturator.prepareAtRate (rate);
                saturator.setCurve (curve);
                saturator.setDrive (drive);
                saturator.setBias (bias);

                const double amplitude = 0.05;

                double highest = -10.0, lowest = 10.0;

                // Exactly one cycle, and slow enough that the averaging has
                // nothing to average, so what is measured is the curve and
                // not the delay. One cycle matters: an earlier version of
                // this used a frequency so low that the sine covered under
                // two percent of a turn in the samples it was given, and
                // every reading was of a signal that had never reached its
                // own peak.
                const int samples = 200000;

                for (int i = 0; i < samples; ++i)
                {
                    const double x = amplitude
                                       * std::sin (2.0 * pi * (double) i / (double) samples);
                    const double y = saturator.processOneAtCurrentRate (x);

                    if (i > 100)
                    {
                        highest = std::max (highest, y);
                        lowest = std::min (lowest, y);
                    }
                }

                const auto at = [&] (double u)
                {
                    return Fx::Saturator::shape (curve, u + bias)
                             - Fx::Saturator::shape (curve, bias);
                };

                const double makeup = 1.0 / std::fabs (Fx::Saturator::shape (curve, drive));

                const double wantHigh = at (amplitude * drive) * makeup;
                const double wantLow  = at (-amplitude * drive) * makeup;

                if (std::fabs (highest - wantHigh) > 1e-5
                      || std::fabs (lowest - wantLow) > 1e-5)
                    fail (std::string (nameOf (curve)) + " at drive " + show (drive)
                          + " bias " + show (bias) + " swings "
                          + show (lowest) + " to " + show (highest)
                          + " where one over the curve at the drive says "
                          + show (wantLow) + " to " + show (wantHigh));
            }

    std::printf ("    the makeup is one over the curve at the drive, at every bias\n");
}

/** Sweeping the bias while something is running lands on the new shaper at
    every step of the way.

    This is the reason the bias belongs to the curve rather than to the input,
    and the check has to be put carefully, because the output does legitimately
    move when the bias does: the shaper changed, so the answer changed. What
    must not happen is a sample that is on neither the old shaper nor the new
    one, which is what a stale history produces. The difference quotient at
    that sample subtracts an integral of the old shaper from an integral of
    the new one, and the size of the resulting nonsense is that mismatch
    divided by however far the signal happened to move, so it is worst when
    the signal is moving slowly and can be enormous.
*/
void sweepingTheBiasStaysOnTheCurve()
{
    std::printf ("sweeping the bias while it runs\n");

    for (auto curve : everyCurve)
    {
        Fx::Saturator saturator;
        saturator.prepareAtRate (rate);
        saturator.setCurve (curve);

        const double drive = 3.0;
        saturator.setDrive (drive);

        const double makeup = 1.0 / std::fabs (Fx::Saturator::shape (curve, drive));

        double bias = -0.9;
        double worst = 0.0;

        for (int i = 0; i < 40000; ++i)
        {
            // A knob being turned: a small step every few milliseconds,
            // rather than a jump from one end to the other.
            if (i > 200 && i % 64 == 0 && bias < 0.9)
            {
                bias = std::min (0.9, bias + 0.005);
                saturator.setBias (bias);
            }

            // Slow, because that is the hard case: a stale history divides
            // its error by how far the signal moved.
            const double x = 0.7 * std::sin (2.0 * pi * 3.0 * (double) i / rate);
            const double y = saturator.processOneAtCurrentRate (x);

            const double onTheShaper = (Fx::Saturator::shape (curve, x * drive + bias)
                                          - Fx::Saturator::shape (curve, bias)) * makeup;

            if (i > 300)
                worst = std::max (worst, std::fabs (y - onTheShaper));
        }

        std::printf ("    %-6s furthest from the shaper during the sweep: %s\n",
                     nameOf (curve), show (worst).c_str());

        if (worst > 0.01)
            fail (std::string (nameOf (curve)) + " left its own shaper by " + show (worst)
                  + " while the bias was being swept");
    }
}

/** Bias brings the even harmonics in, and takes nothing with it. */
void biasBringsInTheEvenHarmonics()
{
    std::printf ("bias\n");

    // A whole number of cycles in the window, exactly: 500 Hz at 48 kHz is 96
    // samples a cycle and 16320 samples is 170 of them. Without that, the
    // unwindowed transform below leaks the first and third harmonics into the
    // second, which reads as half a percent of even harmonic content on a
    // curve that is perfectly odd and has none.
    const double hz = 500.0;
    const int points = 16320;

    const auto harmonicOf = [&] (const std::vector<double>& rendered, int h)
    {
        double real = 0.0, imaginary = 0.0;

        const int n = (int) rendered.size();

        for (int i = 0; i < n; ++i)
        {
            const double angle = 2.0 * pi * (double) h * hz * (double) i / rate;

            real += rendered[(std::size_t) i] * std::cos (angle);
            imaginary -= rendered[(std::size_t) i] * std::sin (angle);
        }

        return std::sqrt (real * real + imaginary * imaginary) / (double) n;
    };

    for (auto curve : everyCurve)
    {
        std::printf ("    %-6s second harmonic against the first: ", nameOf (curve));

        double previous = -1.0;

        for (double bias : { 0.0, 0.1, 0.3, 0.6 })
        {
            const auto rendered = driveASine (curve, 4.0, hz, points, bias);

            const double ratio = harmonicOf (rendered, 2) / harmonicOf (rendered, 1);

            std::printf ("%s:%s ", show (bias).c_str(), show (ratio).c_str());

            if (bias == 0.0 && ratio > 0.002)
                fail (std::string (nameOf (curve))
                      + " has even harmonics with no bias at all: " + show (ratio));

            if (bias > 0.0 && ratio < previous)
                fail (std::string (nameOf (curve)) + " at bias " + show (bias)
                      + " has fewer even harmonics than at a smaller bias");

            previous = ratio;
        }

        std::printf ("\n");

        if (previous < 0.05)
            fail (std::string (nameOf (curve))
                  + " never brings the even harmonics in: largest was " + show (previous));
    }

    // A bias must not leave a constant behind. One into a filter wastes its
    // headroom and one into a gate is a thump at every note.
    for (auto curve : everyCurve)
        for (double bias : { -0.8, -0.2, 0.2, 0.8 })
        {
            Fx::Saturator saturator;
            saturator.prepareAtRate (rate);
            saturator.setCurve (curve);
            saturator.setDrive (3.0);
            saturator.setBias (bias);

            double worst = 0.0;

            for (int i = 0; i < 2000; ++i)
                worst = std::max (worst,
                                  std::fabs (saturator.processOneAtCurrentRate (0.0)));

            if (worst > 1e-12)
                fail (std::string (nameOf (curve)) + " at bias " + show (bias)
                      + " passes a constant of " + show (worst) + " with nothing going in");
        }

    std::printf ("    nothing in, nothing out, at every bias\n");
}

/** Before trusting any aliasing figure, find out how far down this
    measurement can see at all. A nearly linear saturator produces no stray
    partials, so whatever it reports is the floor of the method rather than a
    property of the saturator. */
void theMeasurementCanSeeDownFarEnough()
{
    std::printf ("the measurement's own floor\n");

    const auto rendered = driveASine (Curve::soft, 0.002, 1109.371, 16384, 0.0, 0.5);

    const double floorDb = aliasFloorDb (rendered, 1109.371);

    std::printf ("    a nearly linear drive measures %s dB\n", show (floorDb).c_str());

    if (! (floorDb < -92.0))
        fail ("the measurement cannot see below " + show (floorDb)
              + " dB, so the figures below are the window's and not the saturator's");
}

/** And the aliasing itself, per curve. */
void itDoesNotAlias()
{
    std::printf ("aliasing\n");

    // A note off the bin grid, high enough that a driven version of it has
    // harmonics well past Nyquist, which is the case worth measuring.
    const double hz = 1109.371;

    // Every curve at every drive measures at the floor of the measurement,
    // which is -95 dB, so this is held to -90: anything a regression does to
    // the antialiasing lifts a stray partial clear of that, and nothing that
    // is working can approach it.
    const double worstAllowed = -90.0;

    for (auto curve : everyCurve)
    {
        std::printf ("    %-6s ", nameOf (curve));

        double worst = -1000.0;

        for (double drive : { 2.0, 4.0, 8.0, 16.0, 32.0 })
        {
            const auto rendered = driveASine (curve, drive, hz, 16384);
            const double db = aliasFloorDb (rendered, hz);

            std::printf ("%s:%s ", show (drive).c_str(), show (db).c_str());

            worst = std::max (worst, db);
        }

        std::printf (" worst %s dB\n", show (worst).c_str());

        if (worst > worstAllowed)
            fail (std::string (nameOf (curve)) + " aliases at " + show (worst)
                  + " dB, past the " + show (worstAllowed) + " dB it is held to");
    }
}

/** The curve can be changed while something is running without a click. */
void changingTheCurveDoesNotClick()
{
    std::printf ("changing the curve while it runs\n");

    for (auto from : everyCurve)
        for (auto to : everyCurve)
        {
            if (from == to)
                continue;

            Fx::Saturator saturator;
            saturator.prepareAtRate (rate);
            saturator.setCurve (from);
            saturator.setDrive (3.0);

            double previous = 0.0;
            double ordinaryStep = 0.0;

            // Let it settle, and learn what a normal sample to sample move
            // looks like on this signal, so the step across the change can be
            // held to the same size rather than to a number picked here.
            for (int i = 0; i < 4000; ++i)
            {
                const double x = 0.8 * std::sin (2.0 * pi * 300.0 * (double) i / rate);
                const double y = saturator.processOneAtCurrentRate (x);

                if (i > 100)
                    ordinaryStep = std::max (ordinaryStep, std::fabs (y - previous));

                previous = y;
            }

            saturator.setCurve (to);

            const double x = 0.8 * std::sin (2.0 * pi * 300.0 * 4000.0 / rate);
            const double after = saturator.processOneAtCurrentRate (x);

            // The curves differ, so the output legitimately moves to a new
            // value. What must not happen is a single sample that is nowhere
            // near either curve, which is what a cleared history produces.
            const double onTheNewCurve = (Fx::Saturator::shape (to, x * 3.0))
                                       / std::fabs (Fx::Saturator::shape (to, 3.0));

            if (std::fabs (after - onTheNewCurve) > 4.0 * ordinaryStep + 0.02)
                fail (std::string ("changing from ") + nameOf (from) + " to " + nameOf (to)
                      + " put out " + show (after) + " where the new curve is at "
                      + show (onTheNewCurve));
        }

    std::printf ("    every change lands on the new curve\n");
}

void nothingUnreasonableBreaksIt()
{
    std::printf ("out of range use\n");

    std::mt19937 rng (4717);
    std::uniform_real_distribution<double> wild (-1e6, 1e6);
    std::uniform_int_distribution<int> whichCurve (0, 2);

    for (double sampleRate : { 0.0, -1.0, 8000.0, 192000.0 })
    {
        Fx::Saturator saturator;
        saturator.prepare (sampleRate, 64);

        std::vector<double> block (64);

        for (int b = 0; b < 200; ++b)
        {
            if (b % 7 == 0)
            {
                saturator.setDrive (wild (rng));
                saturator.setBias (wild (rng));
                saturator.setCurve ((Curve) whichCurve (rng));
            }

            for (auto& v : block)
                v = wild (rng);

            saturator.process (block.data(), 64);

            for (double v : block)
            {
                if (! std::isfinite (v))
                {
                    fail ("the output stopped being finite");
                    return;
                }

                // Every curve is bounded by one, so the only thing that can
                // make the output large is the makeup gain, which is one over
                // the curve at the drive and reaches ten thousand at the
                // smallest drive the setter allows. That ceiling is real and
                // is not a fault: at that drive an ordinary signal is being
                // multiplied by a ten thousandth first, and only an input of
                // absurd size, which is what this test feeds, arrives at the
                // flat part of the curve to be multiplied back up. On top of
                // that the oversampler's interpolating filter can overshoot
                // by about a factor of two when what it is fed is noise
                // rather than a signal, which is what this test feeds.
                if (std::fabs (v) > 1.0e5)
                {
                    fail ("the output reached " + show (v)
                          + ", which is past anything a bounded curve can produce");
                    return;
                }
            }
        }
    }

    std::printf ("    survives nonsense at every sample rate\n");
}

} // namespace

int main()
{
    std::printf ("Fx::Saturator\n\n");

    theCurvesAreWhatTheySay();
    theAntiderivativesAreAntiderivatives();
    aSlowSignalComesBackAsTheCurve();
    itComputesTheAverageItClaimsTo();
    driveDoesNotChangeTheLevel();
    biasBringsInTheEvenHarmonics();
    sweepingTheBiasStaysOnTheCurve();
    theMeasurementCanSeeDownFarEnough();
    itDoesNotAlias();
    changingTheCurveDoesNotClick();
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
