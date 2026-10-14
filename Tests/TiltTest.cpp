// Checks Fx/Tilt.h.
//
// There is one claim here and it is a quantitative one: the response falls at
// the number of decibels per octave it was asked for, across the band, at any
// sample rate. So the check is a measurement and a line fit rather than a
// comparison against a formula: feed it sines, measure what comes out, fit a
// straight line through the result in log frequency, and look at both the
// slope of that line and how far the response wanders from it.
//
// That matters more than usual because pink noise has an established
// definition, minus three decibels an octave, and three different wrong
// things all look right from the code: a single pole falls at six, a shelf
// flattens out, and a crossfade between white and a filtered copy has no
// particular slope at all. None of those would announce itself. A measured
// slope does.
//
// And at every sample rate, because the whole reason this is designed rather
// than copied is that the coefficient sets in circulation are specified at
// 44.1 kHz.

#include "Fx/Tilt.h"
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
    char buffer[40];
    std::snprintf (buffer, sizeof (buffer), "%.3f", v);
    return buffer;
}

constexpr double pi = 3.14159265358979323846;

/** The gain the filter really has at a frequency, by running a sine through
    it and measuring the mean square of the steady state.

    A mean square rather than a peak, because at the top of the band there
    are only a few samples to a cycle and a fractional delay moves every
    sampled point off the top of the wave: the wavefolder's probe learned
    that the hard way and read 0.69 dB low with nothing wrong.
*/
double gainAt (double rate, double slope, double hz)
{
    Fx::Tilt tilt;
    tilt.prepare (rate);
    tilt.setSlope (slope);

    const int settle = (int) (rate * 0.3);
    const int measure = (int) (rate * 0.3);

    double sum = 0.0;

    for (int i = 0; i < settle + measure; ++i)
    {
        const double in = std::sin (2.0 * pi * hz * (double) i / rate);
        const double out = tilt.process (in);

        if (i >= settle)
            sum += out * out;
    }

    // Against the input's own mean square, which for a unit sine is a half.
    return std::sqrt (sum / (double) measure / 0.5);
}

//==============================================================================
/** The slope is the slope, measured and fitted, at every sample rate. */
void theSlopeIsTheSlope()
{
    std::printf ("the slope, measured\n");

    for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        std::printf ("  at %.0f Hz\n", rate);

        for (double wanted : { 0.0, -1.5, -3.0, -4.5, -6.0, 3.0 })
        {
            // Across the range anybody listens in, kept clear of the ends of
            // the designed span where the response is legitimately turning
            // over into flat.
            std::vector<double> octaves, decibels;

            for (double hz = 40.0; hz < rate * 0.25; hz *= 1.3)
            {
                const double gain = gainAt (rate, wanted, hz);

                if (! (gain > 0.0))
                {
                    fail ("the filter produced no output at " + show (hz) + " Hz");
                    return;
                }

                octaves.push_back (std::log2 (hz));
                decibels.push_back (20.0 * std::log10 (gain));
            }

            // Least squares, which is the whole of the measurement: the
            // gradient is the slope being claimed and the scatter about the
            // line is the ripple the staircase leaves.
            const double n = (double) octaves.size();

            double sumX = 0.0, sumY = 0.0, sumXY = 0.0, sumXX = 0.0;

            for (std::size_t i = 0; i < octaves.size(); ++i)
            {
                sumX += octaves[i];
                sumY += decibels[i];
                sumXY += octaves[i] * decibels[i];
                sumXX += octaves[i] * octaves[i];
            }

            const double gradient = (n * sumXY - sumX * sumY) / (n * sumXX - sumX * sumX);
            const double intercept = (sumY - gradient * sumX) / n;

            double worstRipple = 0.0;

            for (std::size_t i = 0; i < octaves.size(); ++i)
                worstRipple = std::max (worstRipple,
                                        std::fabs (decibels[i]
                                                     - (gradient * octaves[i] + intercept)));

            std::printf ("    asked %+5.1f dB an octave, measures %+6.3f,"
                         " ripple %s dB\n",
                         wanted, gradient, show (worstRipple).c_str());

            if (std::fabs (gradient - wanted) > 0.25)
                fail ("asked for " + show (wanted) + " dB an octave at "
                      + show (rate) + " Hz and measured " + show (gradient));

            // Held by direction, because the two are not the same and
            // pretending they are would mean one threshold loose enough to
            // say nothing about the other.
            //
            // A falling slope, which is what pink and brown are and what
            // the noise colour uses, stays inside 0.17 dB of its own line
            // at every rate. A rising one is about three times that, and
            // more sections do not help: one every octave and one every
            // three quarters of an octave both measure 0.30, so what is
            // left is not the staircase but the shelf transition sitting
            // slightly differently when it rises. Said plainly in the
            // header rather than averaged away here.
            const double allowed = wanted > 0.0 ? 0.35 : 0.2;

            if (worstRipple > allowed)
                fail ("the response wanders " + show (worstRipple)
                      + " dB from its own straight line at " + show (wanted)
                      + " dB an octave");
        }
    }
}

/** Minus three decibels an octave is pink, which has a definition worth
    checking against directly: equal energy in every octave.

    Measured by transform rather than by sampling a handful of frequencies,
    and that is not a detail. A single reading of a noise spectrum at one
    frequency is a random quantity with as much spread as it has size, so
    twelve of them per octave give an estimate good to about thirty per
    cent. Measured that way, white noise through a flat filter read 5.4
    where doubling says 8, and the check failed a filter that was right.

    A transform gives every bin at once, so an octave at the bottom of the
    band has hundreds of them, and averaging several blocks brings the
    spread down to a couple of per cent.
*/
void minusThreeIsPink()
{
    std::printf ("pink\n");

    const double rate = 48000.0;

    constexpr int size = 1 << 16;
    constexpr int blocks = 8;

    RealFFT fft (size);

    std::vector<std::complex<double>> bins ((std::size_t) fft.getNumBins());
    std::vector<double> power ((std::size_t) fft.getNumBins(), 0.0);

    // Both colours measured from one source, so that the comparison between
    // them is a fact about the filter and not about two noise sequences.
    for (double slope : { 0.0, -3.0 })
    {
        Fx::Tilt tilt;
        tilt.prepare (rate);
        tilt.setSlope (slope);

        std::mt19937 rng (31415);
        std::uniform_real_distribution<double> white (-1.0, 1.0);

        std::fill (power.begin(), power.end(), 0.0);

        std::vector<float> block ((std::size_t) size);

        // Thrown away, so the filter's own state is settled before the
        // first block is kept.
        for (int i = 0; i < 20000; ++i)
            tilt.process (white (rng));

        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < size; ++i)
                block[(std::size_t) i] = (float) tilt.process (white (rng));

            fft.forward (block.data(), bins.data());

            for (std::size_t k = 0; k < power.size(); ++k)
                power[k] += std::norm (bins[k]);
        }

        const double binHz = rate / (double) size;

        const auto energyBetween = [&] (double low, double high)
        {
            double total = 0.0;

            const int from = (int) std::ceil (low / binHz);
            const int to = (int) std::floor (high / binHz);

            for (int k = from; k <= to && k < (int) power.size(); ++k)
                total += power[(std::size_t) k];

            return total;
        };

        const double reference = energyBetween (250.0, 500.0);

        std::printf ("    %+.1f dB an octave, energy per octave against 250 to 500:",
                     slope);

        for (double low : { 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0 })
        {
            const double ratio = energyBetween (low, low * 2.0) / reference;

            std::printf (" %.2f", ratio);

            if (slope == 0.0)
            {
                // White noise has the same energy in every hertz, so an
                // octave twice as wide carries twice as much. Checked
                // because it is what says the measurement itself is sound
                // before it is used to judge the filter.
                const double doubling = low / 250.0;

                if (std::fabs (ratio - doubling) > doubling * 0.12)
                    fail ("the measurement is not sound: white noise from "
                          + show (low) + " Hz reads " + show (ratio)
                          + " where twice the bandwidth says " + show (doubling));
            }
            else
            {
                // And pink has the same energy in every octave, which is
                // its definition.
                if (! (ratio > 0.85 && ratio < 1.18))
                    fail ("the octave from " + show (low) + " Hz carries "
                          + show (ratio) + " of the energy of the one from 250,"
                            " where pink noise says one");
            }
        }

        std::printf ("\n");
    }
}

/** Turning the slope does not turn the level, which is what makes it a tone
    control rather than a volume control with a side effect. */
void theSlopeDoesNotMoveTheLevel()
{
    std::printf ("level against slope\n");

    const double rate = 48000.0;
    const double centre = std::sqrt (10.0 * rate * 0.45);

    std::printf ("    the geometric middle of the designed span is %.0f Hz\n", centre);

    for (double slope : { 0.0, -1.5, -3.0, -6.0, 3.0, 6.0 })
    {
        const double gain = gainAt (rate, slope, centre);

        std::printf ("      %+5.1f dB an octave: gain there %s\n",
                     slope, show (gain).c_str());

        if (std::fabs (20.0 * std::log10 (gain)) > 0.6)
            fail ("at " + show (slope) + " dB an octave the middle of the band is at "
                  + show (20.0 * std::log10 (gain)) + " dB rather than unity");
    }
}

/** A slope of nothing is a filter that is not there. */
void flatIsFlat()
{
    std::printf ("flat\n");

    const double rate = 48000.0;

    Fx::Tilt tilt;
    tilt.prepare (rate);
    tilt.setSlope (0.0);

    double worst = 0.0;

    for (int i = 0; i < 20000; ++i)
    {
        const double in = std::sin (2.0 * pi * 997.0 * (double) i / rate)
                            * 0.5
                        + std::sin (2.0 * pi * 83.0 * (double) i / rate) * 0.4;

        worst = std::max (worst, std::fabs (tilt.process (in) - in));
    }

    std::printf ("    at no slope the output differs from the input by %.2e\n", worst);

    if (worst > 1e-9)
        fail ("a slope of nothing is not transparent: off by " + show (worst));
}

void nothingUnreasonableBreaksIt()
{
    std::printf ("out of range use\n");

    std::mt19937 rng (777);
    std::uniform_real_distribution<double> wild (-1e6, 1e6);

    for (double rate : { 0.0, -1.0, 8000.0, 192000.0 })
    {
        Fx::Tilt tilt;
        tilt.prepare (rate);

        for (int i = 0; i < 60000; ++i)
        {
            if (i % 500 == 0)
                tilt.setSlope (wild (rng));

            const double v = tilt.process (wild (rng) * 1e-6);

            if (! std::isfinite (v))
            {
                fail ("the output stopped being finite");
                return;
            }
        }
    }

    std::printf ("    survives nonsense at every sample rate\n");
}

} // namespace

int main()
{
    std::printf ("Fx::Tilt\n\n");

    flatIsFlat();
    theSlopeIsTheSlope();
    theSlopeDoesNotMoveTheLevel();
    minusThreeIsPink();
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
