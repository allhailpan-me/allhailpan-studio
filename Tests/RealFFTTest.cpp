// ---------------------------------------------------------------------------
// Checks the fast Fourier transform against the slow one.
//
// A fast transform is an identity rearranged for speed, so the only honest
// way to test one is to compute the same answer the expensive way and compare.
// That is what the first check does: a direct evaluation of the defining sum,
// which is obviously correct and far too slow to ship, against the radix-2
// version, over sizes from two to a thousand and change.
//
// The rest is about the properties the spectral code above it leans on. A
// forward followed by an inverse has to return the input, because an edit that
// touches nothing has to change nothing. A sine placed exactly on a bin has to
// land in that bin and nowhere else, because that is what makes a bin index
// mean a frequency. And the sizes that cannot work have to be refused rather
// than rounded, because a transform silently running at a different length
// puts every bin at the wrong frequency, which is a bug that looks like a
// tuning problem three files away.
//
// WHY `inverse` IS CHECKED AGAINST A SUM OF ITS OWN, AND NOT ONLY ROUND TRIPPED
//
// A round trip calls `forward` first, and `forward` leaves the complete
// spectrum in the very buffer the rebuild inside `inverse` is supposed to
// fill. So a bin the rebuild never writes is already sitting there correct
// from the call before it, and the round trip cannot see the omission. Two
// deliberate breakages of that loop, skipping the lowest mirrored bin and
// stopping one short of the highest, passed the whole of this file when it had
// only the round trip.
//
// The production path is the other one. `Synth/WaveTable.h` builds every
// mipmap level of every wavetable in the instrument by handing `inverse` a
// spectrum it assembled harmonic by harmonic, and never calls `forward` at
// all, so the untested path was the one the instrument uses. The check below
// drives `inverse` that way and compares against additive synthesis: the real
// inverse written out as its sum of cosines and sines, in terms of only the
// bins a caller supplies, with no mirroring step of its own to agree with the
// code about.
//
//   ./Tests/run.sh
//
// Reference for the decomposition:
//   https://ccrma.stanford.edu/~jos/mdft/Fast_Fourier_Transform_FFT.html
// ---------------------------------------------------------------------------

#include "RealFFT.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

static int failures = 0;

static void fail (const std::string& what)
{
    std::printf ("  FAIL  %s\n", what.c_str());
    ++failures;
}

static void check (bool ok, const std::string& what)
{
    if (! ok)
        fail (what);
}

static constexpr double pi = 3.14159265358979323846;

//==============================================================================
/** The defining sum, written out. Slow on purpose. */
static std::complex<double> naiveBin (const std::vector<float>& x, int k)
{
    const int n = (int) x.size();
    std::complex<double> acc { 0.0, 0.0 };

    for (int i = 0; i < n; ++i)
    {
        const double angle = -2.0 * pi * (double) k * (double) i / (double) n;
        acc += std::complex<double> ((double) x[(size_t) i], 0.0)
             * std::complex<double> (std::cos (angle), std::sin (angle));
    }

    return acc;
}

static void agreesWithTheDefinition()
{
    std::mt19937 rng (20261006);
    std::uniform_real_distribution<float> noise (-1.0f, 1.0f);

    for (int n : { 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024 })
    {
        RealFFT fft (n);
        check (fft.getSize() == n, "size " + std::to_string (n) + " was not accepted");
        check (fft.getNumBins() == n / 2 + 1, "wrong bin count at size " + std::to_string (n));

        std::vector<float> input ((size_t) n);

        for (auto& s : input)
            s = noise (rng);

        std::vector<std::complex<double>> bins ((size_t) fft.getNumBins());
        fft.forward (input.data(), bins.data());

        double worst = 0.0;

        for (int k = 0; k <= n / 2; ++k)
            worst = std::max (worst, std::abs (naiveBin (input, k) - bins[(size_t) k]));

        // The tolerance grows with the size because the error does: each pass
        // of butterflies is another rounding. Scaling it by the length keeps
        // one threshold honest across the range.
        const double tolerance = 1.0e-12 * (double) n;

        check (worst < tolerance, "size " + std::to_string (n) + " disagrees with the direct sum by "
                                  + std::to_string (worst));
    }
}

static void aRoundTripReturnsTheInput()
{
    std::mt19937 rng (7);
    std::uniform_real_distribution<float> noise (-1.0f, 1.0f);

    for (int n : { 2, 8, 64, 512, 2048 })
    {
        RealFFT fft (n);

        std::vector<float> input ((size_t) n), output ((size_t) n);

        for (auto& s : input)
            s = noise (rng);

        std::vector<std::complex<double>> bins ((size_t) fft.getNumBins());
        fft.forward (input.data(), bins.data());
        fft.inverse (bins.data(), output.data());

        double worst = 0.0;

        for (int i = 0; i < n; ++i)
            worst = std::max (worst, (double) std::fabs (output[(size_t) i] - input[(size_t) i]));

        check (worst < 1.0e-6, "round trip at size " + std::to_string (n) + " drifted by "
                               + std::to_string (worst));

        for (int i = 0; i < n; ++i)
            check (std::isfinite (output[(size_t) i]), "round trip at size " + std::to_string (n)
                                                       + " produced something that is not finite");
    }
}

static void aToneLandsInItsOwnBin()
{
    const int n = 1024;
    RealFFT fft (n);

    for (int k : { 0, 1, 17, 200, 511, 512 })
    {
        std::vector<float> input ((size_t) n);

        for (int i = 0; i < n; ++i)
            input[(size_t) i] = (float) std::cos (2.0 * pi * (double) k * (double) i / (double) n);

        std::vector<std::complex<double>> bins ((size_t) fft.getNumBins());
        fft.forward (input.data(), bins.data());

        // A unit cosine exactly on a bin reads n/2 there, except at direct
        // current and Nyquist, which have no partner above Nyquist to share
        // the energy with and so read n.
        const double expected = (k == 0 || k == n / 2) ? (double) n : (double) n / 2.0;

        // The tolerances are relative, and loose enough to absorb the single
        // precision the input is stored in: rounding each sample of the tone
        // to a float puts a noise floor of its own under the spectrum, a few
        // parts in a hundred million of the peak. A real error here, a
        // misplaced factor of two or a tone in the wrong bin, is nowhere near
        // that small.
        check (std::fabs (std::abs (bins[(size_t) k]) - expected) < 1.0e-7 * expected,
               "bin " + std::to_string (k) + " read " + std::to_string (std::abs (bins[(size_t) k]))
               + " where " + std::to_string (expected) + " was expected");

        double spill = 0.0;

        for (int b = 0; b <= n / 2; ++b)
            if (b != k)
                spill = std::max (spill, std::abs (bins[(size_t) b]));

        check (spill < 1.0e-6 * expected, "a tone on bin " + std::to_string (k) + " spilled "
                                          + std::to_string (spill) + " into its neighbours");
    }
}

/** The real inverse transform, written out as the sum it is defined to be.

    Deliberately not the mirror-and-transform the code does. This is the
    textbook synthesis formula for a real signal, in terms of the bins a
    caller actually supplies: direct current, a cosine and a sine per bin in
    between, and an alternating term at Nyquist. It shares no step with
    `RealFFT::inverse`, which is the point. A reference that rebuilt the upper
    half the same way the code does would agree with the code about the one
    thing most likely to be wrong.

    Both of the bins with no conjugate partner contribute only their real
    part, which is the documented contract: a spectrum with an imaginary part
    on either of those does not describe a real waveform, and the nearest one
    that does is the one with it dropped.
*/
static std::vector<double> naiveInverse (const std::vector<std::complex<double>>& bins, int n)
{
    std::vector<double> out ((size_t) n, 0.0);

    for (int i = 0; i < n; ++i)
    {
        double acc = bins[0].real();

        for (int k = 1; k < n / 2; ++k)
        {
            const double angle = 2.0 * pi * (double) k * (double) i / (double) n;
            acc += 2.0 * (bins[(size_t) k].real() * std::cos (angle)
                          - bins[(size_t) k].imag() * std::sin (angle));
        }

        acc += bins[(size_t) (n / 2)].real() * ((i % 2 == 0) ? 1.0 : -1.0);

        out[(size_t) i] = acc / (double) n;
    }

    return out;
}

static void theInverseAgreesWithTheDefinition()
{
    std::mt19937 rng (20261120);
    std::uniform_real_distribution<double> spread (-4.0, 4.0);

    for (int n : { 2, 4, 8, 16, 64, 256, 1024 })
    {
        const std::string at = " at size " + std::to_string (n);

        std::vector<std::complex<double>> bins ((size_t) (n / 2 + 1));

        for (auto& b : bins)
            b = { spread (rng), spread (rng) };

        const auto expected = naiveInverse (bins, n);

        // Three callers, because what the transform is carrying from its last
        // use is the thing the round trip was hiding.
        //
        //  - a transform used for nothing else, where a skipped bin reads
        //    whatever the buffer started as,
        //  - one that has just transformed something else forward, which is
        //    the arrangement that made two real breakages invisible,
        //  - one that has just inverted a different spectrum, which is what
        //    WaveTable does on every frame after the first.
        RealFFT cold (n);

        RealFFT afterForward (n);
        {
            std::vector<float> other ((size_t) n);

            for (int i = 0; i < n; ++i)
                other[(size_t) i] = (float) std::sin (0.37 * (double) i + 1.1);

            std::vector<std::complex<double>> ignored ((size_t) (n / 2 + 1));
            afterForward.forward (other.data(), ignored.data());
        }

        RealFFT afterInverse (n);
        {
            std::vector<std::complex<double>> other ((size_t) (n / 2 + 1));

            for (size_t k = 0; k < other.size(); ++k)
                other[k] = { 9.0 + (double) k, -5.0 - (double) k };

            std::vector<float> ignored ((size_t) n);
            afterInverse.inverse (other.data(), ignored.data());
        }

        RealFFT* callers[] = { &cold, &afterForward, &afterInverse };
        const char* names[] = { "on a cold transform", "after a forward", "after another inverse" };

        for (int c = 0; c < 3; ++c)
        {
            std::vector<float> output ((size_t) n);
            callers[c]->inverse (bins.data(), output.data());

            double worst = 0.0;

            for (int i = 0; i < n; ++i)
                worst = std::max (worst, std::fabs ((double) output[(size_t) i] - expected[(size_t) i]));

            // Relative to the size of what comes out: the bins are drawn over
            // a fixed range, so the output grows with the number of them that
            // are summed, and a fixed threshold would tighten with the size
            // for no reason.
            double scale = 0.0;

            for (int i = 0; i < n; ++i)
                scale = std::max (scale, std::fabs (expected[(size_t) i]));

            check (worst < 1.0e-6 * std::max (scale, 1.0e-3),
                   "the inverse " + std::string (names[c]) + at + " disagrees with the sum by "
                   + std::to_string (worst) + " where the output peaks at " + std::to_string (scale));
        }
    }
}

static void oneHarmonicComesBackAtTheAmplitudeItAsksFor()
{
    // The convention `Synth/WaveTable.h` builds every wavetable on: a harmonic
    // of amplitude A at bin h is a bin of A * n / 2, because the inverse
    // scales by 1/n and the conjugate pair contributes twice. It is asserted
    // there too, against the table that comes out. It is asserted here as
    // well, against the transform that owns it, so that a change to the
    // scaling fails in the file that made it rather than three files away in
    // the one that trusted it.
    for (int n : { 512, 2048 })
        for (int h : { 1, 2, 7, 64 })
            for (double phase : { 0.0, 0.7, pi / 2.0 })
            {
                RealFFT fft (n);

                std::vector<std::complex<double>> bins ((size_t) (n / 2 + 1), { 0.0, 0.0 });

                const double amplitude = 0.25;
                const double scale = amplitude * (double) n * 0.5;
                bins[(size_t) h] = { scale * std::cos (phase), scale * std::sin (phase) };

                std::vector<float> cycle ((size_t) n);
                fft.inverse (bins.data(), cycle.data());

                double worst = 0.0;

                for (int i = 0; i < n; ++i)
                {
                    const double want = amplitude
                                      * std::cos (2.0 * pi * (double) h * (double) i / (double) n + phase);
                    worst = std::max (worst, std::fabs ((double) cycle[(size_t) i] - want));
                }

                check (worst < 1.0e-6,
                       "harmonic " + std::to_string (h) + " at size " + std::to_string (n)
                       + " came back wrong by " + std::to_string (worst));
            }
}

static void theProductionSizesAreCovered()
{
    // The transform runs at 512 up to 8192 for the wavetables and at 16384
    // for every waveform WaveBank defines as a function of phase, and nothing
    // here used to go above 2048. Checked against a closed form rather than
    // against the direct sum, because the direct sum at 16384 is a hundred
    // million trigonometric calls and this suite is supposed to take seconds:
    // a handful of harmonics has an exact answer and still exercises every
    // pass of butterflies at the real length.
    struct Partial { int bin; double amplitude, phase; };
    const Partial all[] = { { 1, 1.0, 0.0 }, { 5, 0.5, 0.9 },
                            { 97, 0.25, -2.0 }, { 1000, 0.125, 1.7 } };

    for (int n : { 512, 2048, 8192, 16384 })
    {
        RealFFT fft (n);
        check (fft.getSize() == n, "size " + std::to_string (n) + " was not accepted");

        // A partial above the Nyquist bin of the shorter lengths is not a
        // partial of that length at all, so the highest one drops out below
        // 2048 rather than being asked for where it cannot exist.
        std::vector<Partial> partials;

        for (const auto& p : all)
            if (p.bin < n / 2)
                partials.push_back (p);

        std::vector<float> input ((size_t) n, 0.0f);

        for (const auto& p : partials)
            for (int i = 0; i < n; ++i)
                input[(size_t) i] += (float) (p.amplitude
                                     * std::cos (2.0 * pi * (double) p.bin * (double) i / (double) n + p.phase));

        std::vector<std::complex<double>> bins ((size_t) fft.getNumBins());
        fft.forward (input.data(), bins.data());

        // A cosine of amplitude A on bin k reads A * n / 2 there, at the
        // phase it was given.
        double worstPeak = 0.0;

        for (const auto& p : partials)
        {
            const std::complex<double> want { p.amplitude * (double) n * 0.5 * std::cos (p.phase),
                                              p.amplitude * (double) n * 0.5 * std::sin (p.phase) };
            worstPeak = std::max (worstPeak, std::abs (bins[(size_t) p.bin] - want)
                                             / (p.amplitude * (double) n * 0.5));
        }

        check (worstPeak < 1.0e-5, "a partial at size " + std::to_string (n) + " is out by "
                                   + std::to_string (worstPeak) + " of itself");

        double spill = 0.0;

        for (int k = 0; k <= n / 2; ++k)
        {
            bool wanted = false;

            for (const auto& p : partials)
                wanted = wanted || p.bin == k;

            if (! wanted)
                spill = std::max (spill, std::abs (bins[(size_t) k]) / (double) n);
        }

        check (spill < 1.0e-5, "a spectrum of four partials at size " + std::to_string (n)
                               + " spilled " + std::to_string (spill) + " elsewhere");
    }
}

static void energyIsConserved()
{
    // Parseval: the energy in the samples and the energy in the bins are the
    // same number, once the bins that stand for two conjugate halves are
    // counted twice. A transform with a misplaced factor passes the round
    // trip, because the error cancels, and fails this.
    const int n = 256;
    RealFFT fft (n);

    std::mt19937 rng (99);
    std::uniform_real_distribution<float> noise (-1.0f, 1.0f);

    std::vector<float> input ((size_t) n);

    for (auto& s : input)
        s = noise (rng);

    std::vector<std::complex<double>> bins ((size_t) fft.getNumBins());
    fft.forward (input.data(), bins.data());

    double timeEnergy = 0.0;

    for (int i = 0; i < n; ++i)
        timeEnergy += (double) input[(size_t) i] * (double) input[(size_t) i];

    double binEnergy = 0.0;

    for (int k = 0; k <= n / 2; ++k)
    {
        const double magnitudeSquared = std::norm (bins[(size_t) k]);
        binEnergy += (k == 0 || k == n / 2) ? magnitudeSquared : 2.0 * magnitudeSquared;
    }

    binEnergy /= (double) n;

    check (std::fabs (binEnergy - timeEnergy) < 1.0e-9 * timeEnergy,
           "energy is not conserved: " + std::to_string (timeEnergy) + " in time against "
           + std::to_string (binEnergy) + " in frequency");
}

static void impossibleSizesAreRefused()
{
    for (int n : { -8, 0, 1, 3, 6, 100, 1000 })
    {
        RealFFT fft (n);
        check (fft.getSize() == 0, "size " + std::to_string (n) + " was accepted");
        check (fft.getNumBins() == 0, "size " + std::to_string (n) + " reported bins");
    }

    // An unusable transform has to be safe to call, because the caller that
    // ignored the size it got back is exactly the caller that will. Only
    // reached once the refusal above holds: calling into a transform that
    // wrongly believes it has a size reads past the end of these two
    // variables, and a sanitizer abort is a worse way to learn that than the
    // failure printed above it.
    RealFFT fft (7);

    if (fft.getSize() == 0)
    {
        float sample = 1.0f;
        std::complex<double> bin { 1.0, 1.0 };
        fft.forward (&sample, &bin);
        fft.inverse (&bin, &sample);
    }
}

static void aRefusedSizeClearsATransformThatWorked()
{
    // Refusing a size has to clear the transform, not merely decline to set
    // it up. Every check above builds a fresh object, where there was nothing
    // to clear, so dropping the line that does it survived the whole file.
    //
    // It is a real caller's arrangement and not a hypothetical one.
    // `Spectrogram::prepare` calls `setSize` on the same transform every time
    // the window length changes, and decides whether the length was accepted
    // by comparing `getSize()` against what it asked for. A transform that
    // kept the old length would answer that comparison with the old length
    // and then run at it, which is the thing the header says must not happen:
    // every bin at the wrong frequency, silently.
    RealFFT fft (1024);
    check (fft.getSize() == 1024, "a transform would not take a size that is a power of two");

    for (int bad : { 0, 1, 3, 100, 1000, -8 })
    {
        fft.setSize (bad);

        check (fft.getSize() == 0, "size " + std::to_string (bad)
                                   + " was refused but left the transform at its old length");
        check (fft.getNumBins() == 0, "size " + std::to_string (bad)
                                      + " was refused but the transform still reports bins");

        // Reached only once the clearing holds, for the same reason as above.
        if (fft.getSize() == 0)
        {
            float sample = 1.0f;
            std::complex<double> bin { 1.0, 1.0 };
            fft.forward (&sample, &bin);
            fft.inverse (&bin, &sample);
        }

        fft.setSize (1024);
        check (fft.getSize() == 1024, "a transform could not be set up again after a refusal");
    }
}

static void oneTransformReusedAcrossSizesMatchesAFreshOne()
{
    // What `Spectrogram::prepare` does: the same transform, set to one length
    // after another. The twiddle table and the scratch both have to follow,
    // and growing then shrinking then growing again is the order that finds a
    // table left at the wrong length.
    std::mt19937 rng (555);
    std::uniform_real_distribution<float> noise (-1.0f, 1.0f);

    RealFFT reused;

    for (int n : { 8, 1024, 16, 2048, 4, 256, 2 })
    {
        reused.setSize (n);
        check (reused.getSize() == n, "a reused transform would not take size " + std::to_string (n));

        RealFFT fresh (n);

        std::vector<float> input ((size_t) n);

        for (auto& v : input)
            v = noise (rng);

        std::vector<std::complex<double>> a ((size_t) (n / 2 + 1)), b ((size_t) (n / 2 + 1));
        reused.forward (input.data(), a.data());
        fresh.forward (input.data(), b.data());

        double worst = 0.0;

        for (int k = 0; k <= n / 2; ++k)
            worst = std::max (worst, std::abs (a[(size_t) k] - b[(size_t) k]));

        check (worst == 0.0, "a transform reused at size " + std::to_string (n)
                             + " disagrees with a fresh one by " + std::to_string (worst));

        std::vector<float> back ((size_t) n), freshBack ((size_t) n);
        reused.inverse (a.data(), back.data());
        fresh.inverse (b.data(), freshBack.data());

        for (int i = 0; i < n; ++i)
            check (back[(size_t) i] == freshBack[(size_t) i],
                   "a reused transform inverts differently at size " + std::to_string (n));
    }
}

static void theOutputIsRealWhateverTheBinsSay()
{
    // Direct current and Nyquist have no conjugate partner, so an imaginary
    // part on either describes a spectrum no real signal has, and the inverse
    // gives back the nearest spectrum that does: the same one with that
    // imaginary part dropped.
    //
    // Worth knowing what this check can and cannot see, because it reads like
    // a check on the two assignments in `inverse` that drop those parts and it
    // is not one. Both of those were deliberately removed and this passed
    // unchanged, and so did the real thing it would protect against, because
    // what delivers the guarantee is `inverse` taking `.real()` of every
    // sample on the way out. Direct current and Nyquist are the two bins whose
    // basis functions are real, 1 and minus one to the i, so a purely
    // imaginary component on either contributes nothing to the real part of
    // any sample and could never have reached the output. The assignments say
    // the intent at the point it applies; they are not the mechanism.
    //
    // So this stays as a check on the promise rather than on the lines, and
    // the promise is worth pinning: `Spectrogram`'s resynthesis hands back
    // frames a user has edited, and an edit lands on the Nyquist bin as
    // readily as anywhere else.
    const int n = 64;
    RealFFT fft (n);

    std::vector<std::complex<double>> bins ((size_t) fft.getNumBins(), { 0.0, 0.0 });
    bins[0] = { 1.0, 5.0 };
    bins[(size_t) (n / 2)] = { 0.0, 5.0 };

    std::vector<float> output ((size_t) n);
    fft.inverse (bins.data(), output.data());

    for (int i = 0; i < n; ++i)
        check (std::fabs (output[(size_t) i] - 1.0f / (float) n) < 1.0e-6f,
               "an imaginary part on direct current or Nyquist reached the output");
}

int main()
{
    std::printf ("real fft\n");

    agreesWithTheDefinition();
    theInverseAgreesWithTheDefinition();
    oneHarmonicComesBackAtTheAmplitudeItAsksFor();
    theProductionSizesAreCovered();
    aRoundTripReturnsTheInput();
    aToneLandsInItsOwnBin();
    energyIsConserved();
    impossibleSizesAreRefused();
    aRefusedSizeClearsATransformThatWorked();
    oneTransformReusedAcrossSizesMatchesAFreshOne();
    theOutputIsRealWhateverTheBinsSay();

    if (failures > 0)
    {
        std::printf ("\n%d fft check(s) failed\n", failures);
        return 1;
    }

    std::printf ("all fft checks passed\n");
    return 0;
}
