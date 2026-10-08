// Not run by run.sh: a *Probe.cpp is a measurement that justified a design
// decision, kept so the decision can be rechecked rather than argued about.
//
//   g++ -std=c++20 -O2 -I Source -o probe Tests/WaveTableProbe.cpp && ./probe
//
// The decision: how long a wavetable level has to be relative to the number
// of harmonics it carries, and what interpolation to read it with.
//
// Why it needed measuring. The sampling theorem says a waveform of H
// harmonics is fully described by 2H samples, and the first version of
// WaveTable.h took that as the answer and read the result with linear
// interpolation. Every check about harmonic counts passed, because the
// harmonic counts were right. The oscillator measured 28 dB below its own
// fundamental, which is as bad as the naive sawtooth the whole design exists
// to avoid.
//
// Describing a waveform and reading one are different problems. A read lands
// between stored samples, and what the interpolator does in between is an
// error that is not a harmonic of anything. Enough samples to reconstruct the
// waveform exactly, with an exact reconstruction filter, is not the same as
// enough samples to reconstruct it with four points and a cubic.
//
// So: the same band limited sawtooth is rendered twice, once by reading a
// table and once by summing its harmonics directly, and the difference
// between them is the error. That metric has nothing to tune and nothing to
// interpret. Every read rate is swept, because an index step near a whole
// number of samples flatters any interpolator, and a sound design engine
// sweeps the pitch through all of them.

#include "RealFFT.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

namespace
{

constexpr double pi = 3.14159265358979323846;

std::vector<float> bandLimitedSaw (int length, int harmonics)
{
    RealFFT fft (length);

    std::vector<std::complex<double>> bins ((std::size_t) (length / 2 + 1), { 0.0, 0.0 });

    for (int h = 1; h <= harmonics && h < (int) bins.size(); ++h)
    {
        const double amplitude = 1.0 / (double) h;
        const double phase = (h % 2 == 0) ? pi : 0.0;
        const double scale = amplitude * (double) length * 0.5;

        bins[(std::size_t) h] = { scale * std::cos (phase), scale * std::sin (phase) };
    }

    std::vector<float> cycle ((std::size_t) length);
    fft.inverse (bins.data(), cycle.data());

    return cycle;
}

/** The same waveform computed exactly, with no table and no interpolation. */
double exactly (int harmonics, double phase)
{
    double sum = 0.0;

    for (int h = 1; h <= harmonics; ++h)
        sum += std::cos (2.0 * pi * (double) h * phase + ((h % 2 == 0) ? pi : 0.0)) / (double) h;

    return sum;
}

double at (const std::vector<float>& table, int length, int index)
{
    return table[(std::size_t) (((index % length) + length) % length)];
}

double linear (const std::vector<float>& table, int length, double phase)
{
    const double exact = phase * (double) length;
    const int index = (int) exact;
    const double t = exact - (double) index;

    const double a = at (table, length, index);
    const double b = at (table, length, index + 1);

    return a + (b - a) * t;
}

double cubic (const std::vector<float>& table, int length, double phase)
{
    const double exact = phase * (double) length;
    const int index = (int) exact;
    const double t = exact - (double) index;

    const double a = at (table, length, index - 1);
    const double b = at (table, length, index);
    const double c = at (table, length, index + 1);
    const double d = at (table, length, index + 2);

    const double c1 = 0.5 * (c - a);
    const double c2 = a - 2.5 * b + 2.0 * c - 0.5 * d;
    const double c3 = 0.5 * (d - a) + 1.5 * (b - c);

    return ((c3 * t + c2) * t + c1) * t + b;
}

double sixPoint (const std::vector<float>& table, int length, double phase)
{
    const double exact = phase * (double) length;
    const int index = (int) exact;
    const double t = exact - (double) index;

    double result = 0.0;

    // Lagrange through the six points at -2 .. 3.
    for (int j = 0; j < 6; ++j)
    {
        double weight = 1.0;
        const double xj = (double) j - 2.0;

        for (int m = 0; m < 6; ++m)
            if (m != j)
            {
                const double xm = (double) m - 2.0;
                weight *= (t - xm) / (xj - xm);
            }

        result += weight * at (table, length, index - 2 + j);
    }

    return result;
}

/** Worst case error against the exact waveform, in decibels, over a range of
    read rates. */
struct Errors { double linear, cubic, six; };

Errors measure (int harmonics, int length)
{
    const auto table = bandLimitedSaw (length, harmonics);
    const int samples = 20000;

    Errors worst { 0.0, 0.0, 0.0 };

    // Index steps chosen to be awkward: near a whole sample, near a half,
    // and well away from both. A step that lands on whole samples hides an
    // interpolator's error completely.
    for (double step : { 0.137, 0.41, 0.73, 1.31, 2.7, 7.3 })
    {
        double phase = 0.0;
        const double increment = step / (double) length;

        double errorL = 0.0, errorC = 0.0, errorS = 0.0, reference = 0.0;

        for (int i = 0; i < samples; ++i)
        {
            const double want = exactly (harmonics, phase);

            const double dl = linear (table, length, phase) - want;
            const double dc = cubic (table, length, phase) - want;
            const double ds = sixPoint (table, length, phase) - want;

            errorL += dl * dl;
            errorC += dc * dc;
            errorS += ds * ds;
            reference += want * want;

            phase += increment;

            if (phase >= 1.0)
                phase -= std::floor (phase);
        }

        worst.linear = std::max (worst.linear, std::sqrt (errorL / reference));
        worst.cubic  = std::max (worst.cubic,  std::sqrt (errorC / reference));
        worst.six    = std::max (worst.six,    std::sqrt (errorS / reference));
    }

    const auto dB = [] (double v) { return 20.0 * std::log10 (v + 1e-300); };

    return { dB (worst.linear), dB (worst.cubic), dB (worst.six) };
}

} // namespace

int main()
{
    std::printf ("Error against an exact additive sawtooth, worst case over read rates.\n");
    std::printf ("Oversampling is the table length divided by the harmonic count.\n\n");

    std::printf ("%-11s %-9s %-13s %-11s %-11s %-11s\n",
                 "harmonics", "length", "oversampling", "linear", "cubic", "6 point");

    for (int harmonics : { 4, 16, 64, 256, 431 })
    {
        for (int multiple : { 2, 4, 8, 16 })
        {
            int length = 16;

            while (length < harmonics * multiple)
                length *= 2;

            if (length > 131072)
                continue;

            const auto e = measure (harmonics, length);

            std::printf ("%-11d %-9d %-13d %-11.1f %-11.1f %-11.1f\n",
                         harmonics, length, multiple, e.linear, e.cubic, e.six);
        }

        std::printf ("\n");
    }

    std::printf ("What this settled:\n");
    std::printf ("  Twice the harmonic count, read linearly, is about -40 dB. That is\n");
    std::printf ("  audible shimmer and it is what the first version of WaveTable.h did.\n");
    std::printf ("  Sixteen times with a cubic holds every level below -89 dB.\n");
    std::printf ("  Six point interpolation is better still and costs twice the arithmetic\n");
    std::printf ("  per sample for quality below the noise floor of any converter.\n");
    std::printf ("  Going wider instead of higher order costs four times the memory for\n");
    std::printf ("  the same result.\n");

    return 0;
}
