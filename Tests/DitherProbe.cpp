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
// so it swings by up to 14.1 dB between neighbouring half steps over 1.5 to
// 16, and by 7.2 dB between neighbouring tenths over 1.5 to 8: at 3.5 steps
// the worst harmonic is -25.4 dB, at 4.0 it is -39.5, at 4.5 it is -28.6.
// Four steps is a local best case rather than a worst case, which is what
// the draft's comment claimed it was.
//
// Both figures are swept at the bottom of this file, and both ranges are
// part of the figure rather than a detail of how it was taken: the same
// tenth of a step swings by 14.3 dB if the sweep is carried out to 64. A
// spacing quoted without its range is the same mistake as an aliasing figure
// quoted without its band.
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
#include <limits>
#include <random>
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
                // True harmonics only. The fundamental's neighbours are
                // excluded from the floor as leakage, and they are not
                // distortion either: k / bin is 0 or 1 for all of them, so
                // a test on k / bin alone would let every one of them in.
                if (k % bin == 0 && k / bin >= 2 && k / bin <= 7)
                {
                    if (mag > worst) { worst = mag; worstIndex = k / bin; }
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

    // The path an export took before Dither.h existed, reconstructed from the
    // JUCE 9.0.3 sources rather than from memory, so the claim that it left
    // half a least significant bit of direct current in every file can be
    // rechecked here instead of being taken on trust. JUCE cannot be compiled
    // in the sandbox, which is why this is a reconstruction and why it lives
    // in a probe rather than in the test.
    //
    // `AudioFormatWriter::convertFloatsToInts`, juce_AudioFormatWriter.cpp:
    //
    //     *dest = roundToInt (std::numeric_limits<int>::max() * samp);
    //
    // with the rails pinned at `std::numeric_limits<int>::min()` and `max()`
    // for inputs at or past full scale. The WAV writer then narrows that
    // through `AudioData::Int16::setAsInt32LE`, `(uint16) (newValue >> 16)`,
    // or `AudioData::Int24::setAsInt32LE`, `littleEndian24BitToChars
    // (newValue >> 8, data)`, both in juce_AudioDataConverters.h.
    //
    // `juce::roundToInt` is the magic number trick, the value added to
    // 1.5 * 2^52 and the low word of the double read back as an integer,
    // which is round to nearest with ties to even under the default rounding
    // mode. `std::nearbyint` gives the same answer for every input in range
    // and needs no type punning.
    //
    // Two things about it are the point. The scale is 2^31 - 1 rather than
    // 2^31, so the code is already very slightly low before the shift; and
    // the shift is floor division, so the error is in (-1, 0] rather than
    // centred. Those are what the mean below measures.
    // Takes a float, as the real one does: `convertFloatsToInts` reads a
    // `const float*` and widens each sample to double, so the mix has already
    // been through a 24 bit mantissa before any of this happens. Handing this
    // a double and the quantiser a float would be comparing two different
    // signals, and at 24 bit that alone moves how often they disagree by
    // sixteen points, because near full scale the float grid is only twice as
    // fine as the code grid.
    int oldPathCode (float sample, Depth d)
    {
        const double x = (double) sample;

        const int wide = x <= -1.0 ? std::numeric_limits<int>::min()
                       : x >=  1.0 ? std::numeric_limits<int>::max()
                                   : (int) std::nearbyint ((double) std::numeric_limits<int>::max() * x);

        return wide >> (32 - bitsFor (d));
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

    // How far the undithered distortion moves between one amplitude and the
    // next, which is the figure the comment at the top of this file quotes
    // and the whole reason the test sweeps rather than choosing an amplitude.
    //
    // Swept here rather than read off the tables above, because the answer
    // depends on the range as much as on the spacing and the tables print
    // neither. The same tenth of a step swings by 7.2 dB up to 8 steps and by
    // 14.3 dB up to 64, because the amplitudes at which a sine's peaks land
    // unluckily thin out as it gets louder, so a wider range has further to
    // fall between two of them. Quoting a spacing without its range is the
    // same mistake as quoting an aliasing figure without its band.
    std::printf ("\nThe largest change in the worst harmonic between neighbouring\n");
    std::printf ("amplitudes, undithered. A spacing is only meaningful with its range.\n\n");
    std::printf (" spacing | range          | largest swing | at amp\n");
    std::printf ("---------+----------------+---------------+--------\n");

    const auto largestSwing = [n, lowBin] (double step, double from, double to)
    {
        double worst = 0.0, worstAt = from, previous = 0.0;
        bool   havePrevious = false;

        // Counted in whole steps rather than accumulated, so the grid is the
        // one the range names however many points it takes.
        const int points = (int) std::lround ((to - from) / step) + 1;

        for (int i = 0; i < points; ++i)
        {
            const double amp = from + step * (double) i;
            const double db  = measure (amp, Mode::none, n, lowBin, 0xbeef'0003ull).worstDb;

            if (havePrevious && std::abs (db - previous) > worst)
            {
                worst   = std::abs (db - previous);
                worstAt = amp;
            }

            previous = db;
            havePrevious = true;
        }

        std::printf ("  %5.2f  | %5.2f to %-5.2f | %9.2f dB  | %6.2f\n",
                     step, from, to, worst, worstAt);
    };

    largestSwing (0.5, 1.5, 16.0);
    largestSwing (0.1, 1.5,  8.0);
    largestSwing (0.1, 1.5, 16.0);
    largestSwing (0.1, 1.5, 64.0);

    // What moving the export off JUCE's own narrowing actually changed, which
    // the header claims and nothing else in the repository measures.
    std::printf ("\nThe old path against this one. A direct current sweep for the mean\n");
    std::printf ("error, in least significant bits, and a uniform spread of samples for\n");
    std::printf ("how often the two disagree about the code.\n\n");
    std::printf (" depth | old mean | new mean | codes differing | worst disagreement\n");
    std::printf ("-------+----------+----------+-----------------+-------------------\n");

    for (auto d : { Depth::int16, Depth::int24 })
    {
        const double scale = scaleFor (d);

        double oldSum = 0.0, newSum = 0.0;
        const int steps = 200000;

        // Swept across four steps either side of zero, finely, which is the
        // shape of the worst case for a quantiser's mean: the error of a
        // floor is the fractional part of the position and nothing else, so
        // an even sweep across whole steps is what averages it honestly.
        for (int i = 0; i < steps; ++i)
        {
            const double codePosition = -4.0 + 8.0 * (double) i / (double) steps;
            const double x = codePosition / scale;

            oldSum += (double) oldPathCode ((float) x, d) - codePosition;

            Quantiser plain (d, Mode::none, 1);
            newSum += (double) plain.code ((float) x) - codePosition;
        }

        std::mt19937_64 rng (20261027ull);
        std::uniform_real_distribution<double> anywhere (-1.0, 1.0);

        long differing = 0;
        int  worst = 0;
        const long trials = 2000000;

        for (long i = 0; i < trials; ++i)
        {
            const double x = anywhere (rng);

            Quantiser plain (d, Mode::none, 1);
            const int gap = plain.code ((float) x) - oldPathCode ((float) x, d);

            if (gap != 0)
                ++differing;

            worst = std::max (worst, std::abs (gap));
        }

        std::printf (" %-5s | %8.4f | %8.4f | %13.1f %% | %6d LSB\n",
                     name (d), oldSum / (double) steps, newSum / (double) steps,
                     100.0 * (double) differing / (double) trials, worst);
    }

    return 0;
}
