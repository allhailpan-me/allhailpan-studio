#pragma once
#include "RealFFT.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

//==============================================================================
/** Short time Fourier analysis and resynthesis, for looking at a recording as
    time against frequency and putting it back together after an edit.

    The whole point of a spectral editor is that you can reach into a sound,
    take out the cough or the chair creak or the mains hum, and have
    everything around it come back untouched. That second half is the hard
    half, and it is the half that fails quietly: a chain that resynthesises
    with the wrong window or the wrong hop does not error, it returns audio
    that is a little scalloped, and the person using it hears a sound they
    cannot place and blames the edit they just made.

    So the arithmetic here is built from the standard treatment of weighted
    overlap-add rather than from intuition. The requirement, in the notation
    the texts use, is that the shifted windows sum to a constant:

        A(n) = sum over m of w(n - mR) = constant for all n

    with R the hop. Analysis and synthesis each apply a window here, which is
    the weighted form, and there the requirement lands on the product of the
    two: the sum of w(n - mR) squared must be constant. A Hann window
    satisfies the plain condition at any hop of the form (N/2)/k. Squared, it
    does not: the hop that works for Hann at N/2 does not work for Hann
    squared, because the second harmonic the squaring introduces shifts by a
    whole turn per frame at that hop and adds instead of cancelling. At N/4
    both terms cancel over the four overlapping frames and the sum is a flat
    1.5. That is why the default hop here is a quarter of the window and not a
    half, and `Tests/SpectrogramTest.cpp` measures the constant rather than
    trusting this paragraph.

    Two details follow from the same place. The window is the periodic Hann,
    0.5 - 0.5 cos(2 pi i / N), not the symmetric one that ends on the same
    value it starts on: the symmetric window is the right choice for measuring
    a spectrum once and the wrong one for overlap-add, where the repeat of the
    endpoint is exactly what breaks the flat sum. And the output is divided
    through by the window sum that was actually accumulated, sample by sample,
    rather than by the constant 1.5. In the middle those are the same number.
    At the first and last few frames they are not, because the frames that
    would have completed the overlap are off the end of the clip, and dividing
    by what was really accumulated is what makes the head and tail of an
    unedited clip come back exactly rather than fading in and out.

    Frames are centred: frame k is centred on sample k * hop, with the window
    hanging off the front of the clip into zeros for the first few. Centring
    is what makes the time of a frame something a user interface can draw a
    cursor at without an apology.

    References, which agree with each other:
      Smith, Spectral Audio Signal Processing, "Overlap-Add (OLA) STFT
      Processing" and "WOLA Processing Steps":
      https://ccrma.stanford.edu/~jos/sasp/Overlap_Add_OLA_STFT_Processing.html
      https://www.dsprelated.com/freebooks/sasp/WOLA_Processing_Steps.html
      Griffin and Lim, "Signal Estimation from Modified Short-Time Fourier
      Transform", IEEE ASSP-32(2), 1984.

    No JUCE, so the tests can reach it.
*/
class Spectrogram
{
public:
    Spectrogram() = default;

    /** `fftSize` must be a power of two. `hop` must divide into the window at
        least twice, because a hop longer than half the window leaves samples
        that only one frame reaches, and the overlap-add has nothing to sum.
        A request that fails either test leaves this unprepared, which
        `isPrepared` reports, rather than quietly working at some other
        resolution. */
    bool prepare (int newFftSize, int newHop, double newSampleRate)
    {
        fft.setSize (newFftSize);

        if (fft.getSize() != newFftSize || newHop < 1 || newHop > newFftSize / 2
            || newSampleRate <= 0.0)
        {
            fftSize = 0;
            return false;
        }

        fftSize = newFftSize;
        hop = newHop;
        sampleRate = newSampleRate;
        numBins = fft.getNumBins();

        window.resize ((size_t) fftSize);

        for (int i = 0; i < fftSize; ++i)
            window[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * pi * (double) i / (double) fftSize);

        frames.clear();
        numFrames = 0;
        analysedLength = 0;

        return true;
    }

    bool isPrepared() const noexcept { return fftSize > 0; }

    int getFftSize() const noexcept { return fftSize; }
    int getHop() const noexcept { return hop; }
    int getNumBins() const noexcept { return numBins; }
    int getNumFrames() const noexcept { return numFrames; }
    double getSampleRate() const noexcept { return sampleRate; }

    /** The centre frequency of a bin, in hertz. */
    double binToHertz (int bin) const noexcept
    {
        return fftSize > 0 ? (double) bin * sampleRate / (double) fftSize : 0.0;
    }

    /** The bin a frequency falls in, unclamped at the top so a caller can
        tell "above Nyquist" from "the last bin". */
    double hertzToBin (double hertz) const noexcept
    {
        return sampleRate > 0.0 ? hertz * (double) fftSize / sampleRate : 0.0;
    }

    /** The time a frame is centred on, in seconds from the start of the
        clip. */
    double frameToSeconds (int frame) const noexcept
    {
        return sampleRate > 0.0 ? (double) frame * (double) hop / sampleRate : 0.0;
    }

    double secondsToFrame (double seconds) const noexcept
    {
        return hop > 0 ? seconds * sampleRate / (double) hop : 0.0;
    }

    /** How many frames a clip of this length needs. One past the frame
        centred on the last sample, so the tail is covered by a window that is
        still opening rather than by its last few samples. */
    int frameCountFor (int numSamples) const noexcept
    {
        if (fftSize == 0 || numSamples <= 0)
            return 0;

        return (numSamples - 1) / hop + 2;
    }

    /** Analyses a clip. The frames are kept here until the next call. */
    void analyse (const float* signal, int numSamples)
    {
        if (fftSize == 0)
            return;

        analysedLength = numSamples > 0 ? numSamples : 0;
        numFrames = frameCountFor (analysedLength);
        frames.assign ((size_t) numFrames * (size_t) numBins, { 0.0, 0.0 });

        std::vector<float> block ((size_t) fftSize);
        const int halfWindow = fftSize / 2;

        for (int f = 0; f < numFrames; ++f)
        {
            const int start = f * hop - halfWindow;

            for (int i = 0; i < fftSize; ++i)
            {
                const int at = start + i;
                const double sample = (at >= 0 && at < analysedLength) ? (double) signal[at] : 0.0;
                block[(size_t) i] = (float) (sample * window[(size_t) i]);
            }

            fft.forward (block.data(), frames.data() + (size_t) f * (size_t) numBins);
        }
    }

    /** The bins of one frame, lowest frequency first, Nyquist last. */
    std::complex<double>* frame (int index) noexcept
    {
        return frames.data() + (size_t) index * (size_t) numBins;
    }

    const std::complex<double>* frame (int index) const noexcept
    {
        return frames.data() + (size_t) index * (size_t) numBins;
    }

    /** The length of the clip that was analysed, which is what `synthesise`
        writes unless asked for something else. */
    int getAnalysedLength() const noexcept { return analysedLength; }

    /** Overlap-adds the frames back into `output`, which must have room for
        `numSamples`.

        This is the inverse of `analyse` to within floating point when nothing
        has touched the frames in between, head and tail included, which is
        the property `Tests/SpectrogramTest.cpp` asserts over a range of
        lengths and sample rates. */
    void synthesise (float* output, int numSamples) const
    {
        if (fftSize == 0 || numSamples <= 0)
            return;

        std::vector<double> sum ((size_t) numSamples, 0.0);
        std::vector<double> envelope ((size_t) numSamples, 0.0);
        std::vector<float> block ((size_t) fftSize);

        const int halfWindow = fftSize / 2;

        for (int f = 0; f < numFrames; ++f)
        {
            fft.inverse (frame (f), block.data());

            const int start = f * hop - halfWindow;
            const int from = std::max (0, -start);
            const int to = std::min (fftSize, numSamples - start);

            for (int i = from; i < to; ++i)
            {
                const double w = window[(size_t) i];
                sum[(size_t) (start + i)] += (double) block[(size_t) i] * w;
                envelope[(size_t) (start + i)] += w * w;
            }
        }

        for (int i = 0; i < numSamples; ++i)
        {
            // The floor only ever bites where no frame reached at all, which
            // cannot happen for a clip this analysed its own length from. A
            // caller asking for more samples than it analysed gets silence
            // there rather than a division by zero.
            const double e = envelope[(size_t) i];
            output[i] = e > 1.0e-9 ? (float) (sum[(size_t) i] / e) : 0.0f;
        }
    }

    /** The accumulated square of the window at each output sample, which is
        what `synthesise` divides by. Exposed so the tests can measure the
        overlap-add constant directly instead of taking the class comment's
        word for it. */
    void overlapEnvelope (int numSamples, std::vector<double>& out) const
    {
        out.assign ((size_t) std::max (0, numSamples), 0.0);

        if (fftSize == 0 || numSamples <= 0)
            return;

        const int count = frameCountFor (numSamples);
        const int halfWindow = fftSize / 2;

        for (int f = 0; f < count; ++f)
        {
            const int start = f * hop - halfWindow;
            const int from = std::max (0, -start);
            const int to = std::min (fftSize, numSamples - start);

            for (int i = from; i < to; ++i)
                out[(size_t) (start + i)] += window[(size_t) i] * window[(size_t) i];
        }
    }

private:
    static constexpr double pi = 3.14159265358979323846;

    RealFFT fft;
    std::vector<double> window;
    std::vector<std::complex<double>> frames;

    int fftSize = 0;
    int hop = 0;
    int numBins = 0;
    int numFrames = 0;
    int analysedLength = 0;
    double sampleRate = 0.0;
};
