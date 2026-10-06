#pragma once
#include <cmath>
#include <complex>
#include <cstddef>
#include <utility>
#include <vector>

//==============================================================================
/** A radix-2 fast Fourier transform for real signals.

    JUCE has its own FFT, and on some platforms it is a wrapper around a vendor
    library that will beat this one. This exists anyway because the spectral
    code above it is the kind of arithmetic that is wrong silently: an edit
    lands a few bins or a few frames away from where it was drawn, or the
    signal comes back from an untouched round trip slightly scaled, and none
    of that announces itself. Keeping the transform here, with no JUCE, means
    `Tests/` can run a real signal through the whole chain in a second with
    sanitizers on, which is worth more than the cycles it costs.

    The decomposition is the usual one, Cooley and Tukey's: a transform of
    length N is two transforms of length N/2, one over the even indexed
    samples and one over the odd, recombined with a twiddle factor. Done
    iteratively that is a bit reversal permutation followed by log2(N) passes
    of butterflies, which is what `transform` below does. The twiddles are
    computed once at construction, because recomputing a sine and cosine per
    butterfly is both slower and less accurate than reading a table that was
    built from exact arguments.

    A real signal of length N is transformed by running the complex transform
    with the imaginary part set to zero and keeping bins 0 to N/2, since the
    rest are the conjugates of those and carry nothing new. That wastes
    somewhat less than half the work, and the usual packing trick, which runs
    a real transform of length N as a complex one of length N/2 and then
    untangles the two interleaved spectra, would recover it. It is not used
    here: the untangling is fiddly and a classic source of off-by-one errors
    in the middle bins, and this transform runs offline over a clip somebody
    is repairing rather than in the device callback. Correct and plainly
    readable is the right trade at that speed.

    Sizes must be powers of two. No JUCE, so the tests can reach it.
*/
class RealFFT
{
public:
    RealFFT() = default;

    explicit RealFFT (int size) { setSize (size); }

    /** Sizes that are not a power of two, or below two, are rejected and
        leave the object unusable rather than silently rounded: a transform
        quietly running at a different length than the caller believes would
        put every bin at the wrong frequency. */
    void setSize (int size)
    {
        if (size < 2 || (size & (size - 1)) != 0)
        {
            n = 0;
            return;
        }

        n = size;

        // One twiddle per butterfly offset, which is N/2 of them, shared by
        // every pass: pass p uses stride N / (2 * halfSpan).
        twiddles.resize ((size_t) (n / 2));

        for (int k = 0; k < n / 2; ++k)
        {
            const double angle = -2.0 * pi * (double) k / (double) n;
            twiddles[(size_t) k] = { std::cos (angle), std::sin (angle) };
        }

        scratch.resize ((size_t) n);
    }

    int getSize() const noexcept { return n; }

    /** The number of distinct bins a real transform of this size produces,
        which is N/2 + 1: direct current up to and including Nyquist. */
    int getNumBins() const noexcept { return n > 0 ? n / 2 + 1 : 0; }

    /** Transforms `n` real samples into `getNumBins()` complex bins.

        Bin k is centred on k * sampleRate / n hertz. Nothing is scaled here,
        so a unit amplitude sine on bin k reads n/2 in magnitude. The scaling
        lives entirely in `inverse`, so that a forward followed by an inverse
        returns the input. */
    void forward (const float* input, std::complex<double>* bins) const
    {
        if (n == 0)
            return;

        for (int i = 0; i < n; ++i)
            scratch[(size_t) i] = { (double) input[i], 0.0 };

        transform (scratch.data(), false);

        for (int k = 0; k <= n / 2; ++k)
            bins[k] = scratch[(size_t) k];
    }

    /** The inverse of `forward`, including its scaling.

        The bins above Nyquist are rebuilt here from the conjugates of the
        ones below it rather than asked for, which is what makes the output
        real by construction. Any imaginary part the caller left on the direct
        current or Nyquist bin is dropped for the same reason: those two bins
        have no conjugate partner, so a non-zero imaginary part on either is
        not a signal, it is a spectrum that does not describe a real waveform.
        Dropping it is the nearest real spectrum rather than a half of one
        quietly leaking into the output. */
    void inverse (const std::complex<double>* bins, float* output) const
    {
        if (n == 0)
            return;

        scratch[0] = { bins[0].real(), 0.0 };

        for (int k = 1; k < n / 2; ++k)
        {
            scratch[(size_t) k] = bins[k];
            scratch[(size_t) (n - k)] = std::conj (bins[k]);
        }

        scratch[(size_t) (n / 2)] = { bins[n / 2].real(), 0.0 };

        transform (scratch.data(), true);

        const double scale = 1.0 / (double) n;

        for (int i = 0; i < n; ++i)
            output[i] = (float) (scratch[(size_t) i].real() * scale);
    }

private:
    static constexpr double pi = 3.14159265358979323846;

    /** In place, decimation in time, unscaled. `conjugated` runs the inverse
        by conjugating the twiddles, which is the same transform with the sign
        of every angle flipped. */
    void transform (std::complex<double>* data, bool conjugated) const
    {
        // Bit reversal. Only swapping when i < j visits each pair once, and
        // leaves the fixed points alone.
        for (int i = 1, j = 0; i < n; ++i)
        {
            int bit = n >> 1;

            for (; j & bit; bit >>= 1)
                j ^= bit;

            j ^= bit;

            if (i < j)
                std::swap (data[i], data[j]);
        }

        for (int span = 2; span <= n; span <<= 1)
        {
            const int half = span / 2;
            const int stride = n / span;

            for (int start = 0; start < n; start += span)
            {
                for (int k = 0; k < half; ++k)
                {
                    const std::complex<double> w = conjugated
                        ? std::conj (twiddles[(size_t) (k * stride)])
                        : twiddles[(size_t) (k * stride)];

                    const std::complex<double> upper = data[start + k + half] * w;
                    const std::complex<double> lower = data[start + k];

                    data[start + k] = lower + upper;
                    data[start + k + half] = lower - upper;
                }
            }
        }
    }

    int n = 0;
    std::vector<std::complex<double>> twiddles;
    mutable std::vector<std::complex<double>> scratch;
};
