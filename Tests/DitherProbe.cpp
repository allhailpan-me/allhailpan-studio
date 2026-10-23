// Not run by run.sh: a *Probe.cpp is a measurement that justified a design
// decision, kept so the decision can be rechecked rather than argued about.
//
//   g++ -std=c++20 -O2 -I Source -o probe Tests/DitherProbe.cpp && ./probe
//
// The decision: what figures Dither.h and DitherTest.cpp are allowed to
// quote for the distortion an undithered word length reduction produces, and
// at what amplitude the test should look for it.
//
// Why it needed measuring, which is the whole point of this file existing.
// The header shipped in draft quoting a 1 kHz sine four steps high with its
// fifth and seventh harmonics at -26 and -31 dB, and the dither bringing
// them under a -74 dB measurement floor. Nothing in the repository produced
// any of those three numbers, and all three are wrong: at four steps the
// worst harmonic is the third at -39 dB, the fifth is at -47, and the
// dithered floor of that measurement is -63. A number that is present,
// plausible and wrong, in the feature whose own header is about that exact
// failure. So the figures now come from here and can be rechecked in a
// second.
//
// What the sweep shows, which is the part worth not relearning. Undithered
// distortion is not monotonic in amplitude and is not even close to it. It
// depends on where the sine's peaks happen to fall relative to the codes,
// so it swings by more than twenty decibels between neighbouring tenths of a
// step: at 3.5 steps the worst harmonic is -25 dB, at 4.0 it is -39, at 4.5
// it is -29. Four steps is a local best case rather than a worst case, which
// is what the draft's comment claimed it was.
//
// That is why the test sweeps rather than picking an amplitude. A single
// chosen amplitude with a fixed threshold is a measurement of where the
// peaks landed, and the one the draft chose sat 0.55 dB from failing: moving
// it to the more representative 5.0 would have turned the suite red with
// nothing wrong in Dither.h at all.

#include "Dither.h"
#include "RealFFT.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

using namespace Dither;

namespace
{
    constexpr double pi = 3.14159265358979323846;

    struct Reading
    {
        double worstDb;     // the largest of harmonics 2 to 7, below the fundamental
        int    worstIndex;
        double floorDb;     // everything that is not a harmonic, as an rms
        double h3, h5, h7;
    };

    // A sine of `amp` quantiser steps, quantised at 16 bit, measured against
    // its own fundamental. Placed on an exact bin so the fundamental does not
    // leak across the harmonics being measured.
    Reading measure (double amp, Mode mode, int n, int bin, std::uint64_t seed)
    {
        const double scale = scaleFor (Depth::int16);
        const double w     = 2.0 * pi * (double) bin / (double) n;

        std::vector<float> out ((size_t) n);
        Quantiser q (Depth::int16, mode, seed);

        for (int i = 0; i < n; ++i)
            out[(size_t) i] = (float) q.code ((float) (amp * std::sin (w * (double) i) / scale));

        RealFFT fft (n);
        std::vector<std::complex<double>> bins ((size_t) fft.getNumBins());
        fft.forward (out.data(), bins.data());

        const double fundamental = std::abs (bins[(size_t) bin]);

        double worst = 0.0, floorSum = 0.0;
        int    worstIndex = 0, floorCount = 0;
        double harmonic[8] = { 0.0 };

        for (int k = 2; k < fft.getNumBins(); ++k)
        {
            const double mag = std::abs (bins[(size_t) k]) / fundamental;

            // The fundamental's own bin and its neighbours carry leakage and
            // are not distortion, so they are in neither total.
            const bool isHarmonic = (k % bin) == 0 || std::abs (k - bin) <= 2;

            if (isHarmonic)
            {
                if (k != bin && k / bin <= 7)
                {
                    if (mag > worst) { worst = mag; worstIndex = k / bin; }

                    if (k % bin == 0)
                        harmonic[k / bin] = mag;
                }
            }
            else
            {
                floorSum += mag * mag;
                ++floorCount;
            }
        }

        auto db = [] (double v) { return v > 0.0 ? 20.0 * std::log10 (v) : -std::numeric_limits<double>::infinity(); };

        return { db (worst), worstIndex,
                 db (std::sqrt (floorSum / (double) floorCount)),
                 db (harmonic[3]), db (harmonic[5]), db (harmonic[7]) };
    }
}

int main()
{
    const int n = 1 << 17;

    // Two placements of the same sine. 1 kHz is the figure the header quotes
    // and bin 97 is what the test uses, and they have to agree: the
    // quantiser is memoryless, so what it does to a sample depends on that
    // sample's value and not on the rate the signal is sweeping through
    // those values. If these two columns ever disagree, something in here
    // has grown a memory it should not have.
    const int lowBin  = 97;      // 35.5 Hz at 48 kHz: an odd bin, harmonics distinct
    const int kiloBin = 2731;    // 1000.1 Hz at 48 kHz, n = 2^17

    std::printf ("16 bit. A sine of `amp` steps, worst of harmonics 2 to 7,\n");
    std::printf ("relative to its own fundamental.\n\n");

    std::printf ("            undithered, bin %-5d          | undithered, bin %-4d | dithered, bin %d\n",
                 lowBin, kiloBin, lowBin);
    std::printf ("   amp |   worst   h       h3       h5       h7 |   worst   h |   worst    floor\n");
    std::printf ("-------+---------------------------------------+-------------+------------------\n");

    for (double amp : { 0.25, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0,
                        4.5, 5.0, 6.0, 8.0, 12.0, 16.0, 32.0, 64.0 })
    {
        const auto u = measure (amp, Mode::none,       n, lowBin,  0xbeef'0003ull);
        const auto k = measure (amp, Mode::none,       n, kiloBin, 0xbeef'0003ull);
        const auto d = measure (amp, Mode::triangular, n, lowBin,  0xbeef'0003ull);

        std::printf ("%6.2f | %7.2f %3d %8.2f %8.2f %8.2f | %7.2f %3d | %7.2f %8.2f\n",
                     amp, u.worstDb, u.worstIndex, u.h3, u.h5, u.h7,
                     k.worstDb, k.worstIndex, d.worstDb, d.floorDb);
    }

    // The property the test asserts, swept, so the margin it runs on is
    // visible rather than taken on trust. Undithered, the worst harmonic
    // stands this far clear of the noise floor; dithered, it should stand
    // nowhere near clear of it.
    std::printf ("\nHow far the worst harmonic stands clear of the record's own noise floor.\n");
    std::printf ("This is what the test asserts, because it is the claim that does not\n");
    std::printf ("depend on the record length the way an absolute threshold does.\n\n");
    std::printf ("   amp | undithered clearance | dithered clearance\n");
    std::printf ("-------+----------------------+--------------------\n");

    double worstUndithered = 1.0e9, worstDithered = -1.0e9;

    for (double amp : { 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0, 6.0, 8.0, 12.0, 16.0 })
    {
        const auto u = measure (amp, Mode::none,       n, lowBin, 0xbeef'0003ull);
        const auto d = measure (amp, Mode::triangular, n, lowBin, 0xbeef'0003ull);

        const double uc = u.worstDb - u.floorDb;
        const double dc = d.worstDb - d.floorDb;

        worstUndithered = std::min (worstUndithered, uc);
        worstDithered   = std::max (worstDithered,   dc);

        std::printf ("%6.2f | %20.2f | %18.2f\n", amp, uc, dc);
    }

    std::printf ("\nover that sweep: undithered clears by at least %.2f dB,\n", worstUndithered);
    std::printf ("                 dithered  clears by at most   %.2f dB\n", worstDithered);

    return 0;
}
