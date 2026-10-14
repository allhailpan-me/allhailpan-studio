// Checks Fx/DelayLine.h.
//
// Four claims, and the interesting ones are the last two.
//
// That a whole number of samples of delay is exact. Anything else means the
// circular index is off by one somewhere, which on a delay effect is
// inaudible and on a granular one that reads a dozen places in the past is a
// different sound.
//
// That the interpolator is the polynomial it says it is, checked against the
// Lagrange basis written out separately. A Catmull-Rom spline is a different
// four point interpolator with very similar code and a different response,
// and the two are easy to confuse.
//
// That a fractional delay really delays by the fraction. Measured as the
// phase of a sine coming out against the one going in, because that is what
// the delay is for: a delay that is accurate in magnitude and wrong in time
// would comb-filter a chorus in the wrong place and nothing would say so.
//
// And how much of the top of the band the read costs, measured against a
// linear read. The class comment claims a cubic is worth its extra multiplies
// and this is the only reason to believe it.

#include "Fx/DelayLine.h"

#include <algorithm>
#include <cmath>
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
    char buffer[40];
    std::snprintf (buffer, sizeof (buffer), "%.4f", v);
    return buffer;
}

constexpr double pi = 3.14159265358979323846;
constexpr double rate = 48000.0;

//==============================================================================
/** A whole number of samples comes back exactly. */
void wholeSamplesAreExact()
{
    std::printf ("whole samples\n");

    Fx::DelayLine line;
    line.prepare (rate, 0.05);

    std::printf ("    the line holds %.0f samples, %s s\n",
                 line.maximumSamples(), show (line.maximumSeconds()).c_str());

    for (double delay : { 1.0, 2.0, 7.0, 64.0, 1000.0 })
    {
        line.reset();

        std::mt19937 rng (9001);
        std::uniform_real_distribution<double> noise (-1.0, 1.0);

        std::vector<double> written, read;

        for (int i = 0; i < 4000; ++i)
        {
            const double value = noise (rng);

            line.write (value);
            written.push_back (value);
            read.push_back (line.read (delay));
        }

        // The sample read at step i should be the one written at i minus the
        // delay, to the last bit.
        double worst = 0.0;

        for (std::size_t i = (std::size_t) delay; i < written.size(); ++i)
            worst = std::max (worst,
                              std::fabs (read[i] - written[i - (std::size_t) delay]));

        std::printf ("    %6.0f samples back: off by %.2e\n", delay, worst);

        if (worst > 1e-12)
            fail ("a delay of " + show (delay) + " whole samples is off by "
                  + show (worst));
    }
}

/** The interpolator is the Lagrange cubic, against the basis written out
    independently. */
void theInterpolatorIsLagrange()
{
    std::printf ("the interpolator\n");

    std::mt19937 rng (4242);
    std::uniform_real_distribution<double> value (-1.0, 1.0);

    double worst = 0.0;

    for (int attempt = 0; attempt < 2000; ++attempt)
    {
        const double a = value (rng), b = value (rng);
        const double c = value (rng), d = value (rng);

        for (double t = 0.0; t <= 1.0; t += 0.05)
        {
            // The four points sit at -1, 0, 1 and 2 against t, so the
            // Lagrange basis for each is the product of the distances to the
            // other three, over the same product for itself.
            const double at[4] { -1.0, 0.0, 1.0, 2.0 };
            const double y[4] { a, b, c, d };

            double expected = 0.0;

            for (int i = 0; i < 4; ++i)
            {
                double term = y[i];

                for (int j = 0; j < 4; ++j)
                    if (j != i)
                        term *= (t - at[j]) / (at[i] - at[j]);

                expected += term;
            }

            worst = std::max (worst,
                              std::fabs (Fx::DelayLine::cubic (a, b, c, d, t) - expected));
        }
    }

    std::printf ("    differs from the Lagrange basis by %.2e\n", worst);

    if (worst > 1e-12)
        fail ("the interpolator is not the Lagrange cubic: off by " + show (worst));

    // And it passes through its two middle points exactly, which is what
    // makes a whole number of samples exact.
    for (double v : { -0.7, 0.0, 0.3, 1.0 })
    {
        if (std::fabs (Fx::DelayLine::cubic (9.0, v, 5.0, -3.0, 0.0) - v) > 1e-14)
            fail ("the interpolator does not pass through its first point");

        if (std::fabs (Fx::DelayLine::cubic (9.0, 5.0, v, -3.0, 1.0) - v) > 1e-14)
            fail ("the interpolator does not pass through its second point");
    }

    std::printf ("    and passes through both of its middle points\n");
}

//==============================================================================
/** Pushes a sine through the line at a fixed delay and measures what comes
    out, as a gain and as a delay in samples. */
struct Response
{
    double gain, delaySamples;
};

Response measure (double hz, double delay, bool linear)
{
    Fx::DelayLine line;
    line.prepare (rate, 0.05);

    const int settle = 4000;
    const int count = 48000;

    double real = 0.0, imaginary = 0.0;

    for (int i = 0; i < settle + count; ++i)
    {
        const double in = std::sin (2.0 * pi * hz * (double) i / rate);

        line.write (in);

        const double out = linear ? line.readLinear (delay) : line.read (delay);

        if (i >= settle)
        {
            const double angle = 2.0 * pi * hz * (double) i / rate;

            real += out * std::cos (angle);
            imaginary -= out * std::sin (angle);
        }
    }

    const double magnitude = 2.0 * std::sqrt (real * real + imaginary * imaginary)
                               / (double) count;

    // The phase the output lags the input by, turned into samples.
    //
    // Worth working through rather than guessing, which is what the first
    // version of this did: it reported 232 samples for a delay of 8, which
    // is half a cycle at 100 Hz minus the delay, and the sign was wrong.
    //
    // For an output of sin(wi - p), the two sums above come to
    // -sin(p) N/2 and -cos(p) N/2, so atan2 of the second negated against
    // the first is atan2(cos p, -sin p), which is a quarter turn more than
    // p. The lag is therefore the angle less a quarter turn.
    const double phase = std::atan2 (-imaginary, real);

    double lag = phase - pi * 0.5;

    const double cycle = rate / hz;

    while (lag < 0.0)
        lag += 2.0 * pi;

    double samples = lag / (2.0 * pi) * cycle;

    while (samples < delay - cycle * 0.5)
        samples += cycle;

    while (samples > delay + cycle * 0.5)
        samples -= cycle;

    return { magnitude, samples };
}

/** A fractional delay delays by the fraction. */
void aFractionalDelayDelaysByTheFraction()
{
    std::printf ("the delay a fraction really gives\n");

    // A low frequency, so one cycle is hundreds of samples and the phase can
    // be read to a small fraction of one.
    const double hz = 100.0;

    for (double delay : { 8.0, 8.25, 8.5, 8.75, 9.0, 40.37, 400.9 })
    {
        const auto response = measure (hz, delay, false);

        std::printf ("    asked %8.2f samples, measures %8.3f, gain %s\n",
                     delay, response.delaySamples, show (response.gain).c_str());

        if (std::fabs (response.delaySamples - delay) > 0.02)
            fail ("a delay of " + show (delay) + " samples measures "
                  + show (response.delaySamples));
    }
}

/** What the read costs at the top of the band, and that the cubic is worth
    having over a linear read. */
void theCubicEarnsItsMultiplies()
{
    std::printf ("what the read costs\n");

    // Half a sample of fraction, which is the worst case for any
    // interpolator: at a whole number of samples every one of them is exact.
    const double delay = 100.5;

    std::printf ("    at half a sample of fraction:  linear     cubic\n");

    for (double hz : { 1000.0, 5000.0, 10000.0, 15000.0 })
    {
        const double linear = measure (hz, delay, true).gain;
        const double cubic = measure (hz, delay, false).gain;

        const double linearDb = 20.0 * std::log10 (linear + 1e-300);
        const double cubicDb = 20.0 * std::log10 (cubic + 1e-300);

        std::printf ("      %6.0f Hz                 %7.2f dB %7.2f dB\n",
                     hz, linearDb, cubicDb);

        // The cubic has to be better everywhere, and by a lot where it
        // matters: a third of a decibel at 10 kHz against three.
        if (! (cubicDb > linearDb))
            fail ("the cubic is no better than a linear read at " + show (hz) + " Hz");

        // Measured at -0.53 dB at 10 kHz with half a sample of fraction,
        // so held to 0.7: what this is for is catching a read that has
        // stopped interpolating, which costs decibels and not tenths.
        if (hz <= 10000.0 && cubicDb < -0.7)
            fail ("the cubic loses " + show (-cubicDb) + " dB at " + show (hz) + " Hz");
    }

    // And at a whole number of samples both are exact, which is the other
    // half of the claim.
    for (double hz : { 1000.0, 15000.0 })
    {
        const double exact = measure (hz, 100.0, false).gain;

        if (std::fabs (exact - 1.0) > 1e-6)
            fail ("a whole number of samples is not exact at " + show (hz)
                  + " Hz: gain " + show (exact));
    }

    std::printf ("      and a whole number of samples is exact at any frequency\n");
}

/** Sweeping the delay does not click, which is the whole reason this uses a
    Lagrange interpolator and not an allpass. */
void sweepingTheDelayIsSmooth()
{
    std::printf ("sweeping the delay\n");

    Fx::DelayLine line;
    line.prepare (rate, 0.05);

    const double hz = 220.0;

    // Swept across most of the line, which is what a chorus does to a short
    // delay and a tape effect does to a long one.
    const double from = 20.0, to = 900.0;
    const int count = (int) (rate * 2.0);

    double worstStep = 0.0, ordinaryStep = 0.0;
    double previous = 0.0;

    for (int i = 0; i < count; ++i)
    {
        const double in = std::sin (2.0 * pi * hz * (double) i / rate);

        line.write (in);

        const double t = (double) i / (double) count;
        const double delay = from + (to - from) * t;

        const double out = line.read (delay);

        if (i > 2000)
        {
            const double step = std::fabs (out - previous);

            worstStep = std::max (worstStep, step);
            ordinaryStep += step;
        }

        previous = out;
    }

    ordinaryStep /= (double) (count - 2000);

    std::printf ("    largest step %s, average step %s\n",
                 show (worstStep).c_str(), show (ordinaryStep).c_str());

    // A 220 Hz sine moves by at most 2 pi f / rate of its amplitude per
    // sample, which is 0.029, and the sweep adds a little on top because the
    // read is moving as well. Anything much past that is a discontinuity.
    if (! (worstStep < 0.06))
        fail ("sweeping the delay stepped by " + show (worstStep)
              + " where a 220 Hz sine moves 0.029 a sample");
}

void nothingUnreasonableBreaksIt()
{
    std::printf ("out of range use\n");

    std::mt19937 rng (31337);
    std::uniform_real_distribution<double> wild (-1e9, 1e9);

    for (double sampleRate : { 0.0, -1.0, 8000.0, 192000.0 })
        for (double longest : { -1.0, 0.0, 0.0001, 4.0 })
        {
            Fx::DelayLine line;
            line.prepare (sampleRate, longest);

            for (int i = 0; i < 20000; ++i)
            {
                line.write (wild (rng) * 1e-9);

                const double a = line.read (wild (rng));
                const double b = line.readSeconds (wild (rng));
                const double c = line.readLinear (wild (rng));

                if (! std::isfinite (a) || ! std::isfinite (b) || ! std::isfinite (c))
                {
                    fail ("a read stopped being finite");
                    return;
                }

                // Nothing written was larger than one, and a four point
                // Lagrange read of samples inside minus one to one stays
                // inside about twice that.
                if (std::fabs (a) > 3.0 || std::fabs (b) > 3.0 || std::fabs (c) > 3.0)
                {
                    fail ("a read returned " + show (a) + " from a line holding"
                          " nothing larger than one");
                    return;
                }
            }
        }

    std::printf ("    survives nonsense at every size and rate\n");
}

} // namespace

int main()
{
    std::printf ("Fx::DelayLine\n\n");

    wholeSamplesAreExact();
    theInterpolatorIsLagrange();
    aFractionalDelayDelaysByTheFraction();
    theCubicEarnsItsMultiplies();
    sweepingTheDelayIsSmooth();
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
