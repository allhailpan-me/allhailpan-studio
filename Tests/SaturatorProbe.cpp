// A measurement, not a test. Not run by Tests/run.sh: it is kept so that the
// decision it settled can be rechecked.
//
// The question is how much oversampling the saturator needs, and the reason it
// has to be measured rather than reasoned about is that the three curves lean
// on it by very different amounts and the order of the antialiasing differs
// between them. The hyperbolic tangent can only carry the first order form,
// because its second antiderivative contains a dilogarithm and is not
// elementary; the two polynomial curves carry the second order form exactly.
// So the obvious expectation, that the gentler curve needs less help, is not
// safe: the gentler curve is also the one with the weaker antialiasing.
//
// It is also the probe that says where the measurement's own floor is. A four
// term Blackman Harris window has sidelobes 92 dB down, and the figures below
// bottom out around 95, so anything reported near that number is the window
// rather than the saturator and no amount of oversampling will move it.
//
// Build and run it by hand:
//
//     g++ -std=c++20 -O2 -I Source -o /tmp/probe Tests/SaturatorProbe.cpp && /tmp/probe

#include "Fx/Saturator.h"
#include "RealFFT.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

namespace
{

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

/** The same measurement SaturatorTest uses, so the two sets of numbers mean
    the same thing: the loudest thing in the output that is not within eight
    bins of a harmonic of the note, against the loudest harmonic. */
double aliasFloorDb (const std::vector<double>& rendered, double frequency,
                     double highestOfInterest = rate * 0.5)
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

std::vector<double> driveASine (Curve curve, int factor, double drive,
                                 double hz, int count)
{
    Fx::Saturator saturator;
    saturator.prepare (rate, 256, factor);
    saturator.setCurve (curve);
    saturator.setDrive (drive);

    const int discard = 2048;

    std::vector<double> block ((std::size_t) 256);
    std::vector<double> out;
    out.reserve ((std::size_t) count);

    int produced = 0;

    for (int written = 0; written < discard + count; written += 256)
    {
        for (int i = 0; i < 256; ++i)
        {
            const double t = (double) (written + i) / rate;
            block[(std::size_t) i] = std::sin (2.0 * pi * hz * t);
        }

        saturator.process (block.data(), 256);

        for (int i = 0; i < 256 && produced < discard + count; ++i, ++produced)
            if (produced >= discard)
                out.push_back (block[(std::size_t) i]);
    }

    return out;
}

} // namespace

int main()
{
    const double hz = 1109.371;     // off the bin grid on purpose
    const int points = 16384;

    const Curve curves[] = { Curve::soft, Curve::cubic, Curve::hard };
    const int factors[] = { 1, 2, 4, 8, 16 };
    const double drives[] = { 2.0, 4.0, 8.0, 16.0, 32.0 };

    std::printf ("The loudest stray partial, in decibels below the loudest harmonic.\n");
    std::printf ("A sine at %.0f Hz, 48 kHz, measured in the steady state.\n", hz);
    std::printf ("Around -95 is the window's own floor rather than the saturator's.\n\n");

    // Measured twice, over two different bands, because the oversampler's
    // filters are designed to allow folding into the top of the spectrum and
    // not below it: its stopband starts at the oversampled rate minus 20 kHz,
    // so everything that folds lands between 20 kHz and Nyquist on purpose.
    // Counting that as aliasing reads a floor that no amount of oversampling
    // will move and hides what is happening where anybody can hear it.
    for (double highest : { rate * 0.5, 20000.0 })
    {
        std::printf ("=== counting strays up to %.0f Hz ===\n\n", highest);

        for (auto curve : curves)
        {
            std::printf ("%s (%s order antialiasing)\n", nameOf (curve),
                         Fx::Saturator::hasSecondIntegral (curve) ? "second" : "first");

            std::printf ("    drive  ");

            for (int factor : factors)
                std::printf ("%7dx", factor);

            std::printf ("\n");

            for (double drive : drives)
            {
                std::printf ("    %5.0f  ", drive);

                for (int factor : factors)
                {
                    const auto rendered = driveASine (curve, factor, drive, hz, points);
                    std::printf ("%8.1f", aliasFloorDb (rendered, hz, highest));
                }

                std::printf ("\n");
            }

            std::printf ("\n");
        }
    }

    // And what the oversampling costs, in latency, since that is the other
    // half of the decision: a linear phase filter delays what goes through it
    // and an instrument voice pays that on every note.
    std::printf ("Latency, in samples of the base rate:\n");

    for (int factor : factors)
    {
        Fx::Saturator soft, cubic;
        soft.prepare (rate, 256, factor);
        soft.setCurve (Curve::soft);
        cubic.prepare (rate, 256, factor);
        cubic.setCurve (Curve::cubic);

        std::printf ("    %2dx  first order %6.2f,  second order %6.2f\n",
                     factor, soft.latencySamples(), cubic.latencySamples());
    }

    return 0;
}
