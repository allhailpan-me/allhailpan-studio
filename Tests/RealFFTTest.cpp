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

static void theOutputIsRealWhateverTheBinsSay()
{
    // Direct current and Nyquist have no conjugate partner, so an imaginary
    // part on either describes a spectrum no real signal has. The inverse is
    // supposed to drop it rather than let half of it through as a phantom.
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
    aRoundTripReturnsTheInput();
    aToneLandsInItsOwnBin();
    energyIsConserved();
    impossibleSizesAreRefused();
    theOutputIsRealWhateverTheBinsSay();

    if (failures > 0)
    {
        std::printf ("\n%d fft check(s) failed\n", failures);
        return 1;
    }

    std::printf ("all fft checks passed\n");
    return 0;
}
