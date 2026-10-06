// ---------------------------------------------------------------------------
// Checks that a clip taken apart into frames and put back together comes back
// unchanged.
//
// This is the property the whole spectral editor rests on. Everything a repair
// does is "change this small part and leave the rest", and the "leave the
// rest" half is the half that fails silently: a chain with the wrong window or
// the wrong hop returns audio that is scalloped by a few percent at a rate
// nobody can name, and the person using it hears something slightly wrong and
// blames the edit.
//
// So the first check is the overlap-add constant, measured rather than
// assumed. The texts say a Hann window satisfies the plain overlap-add
// condition at any hop of (N/2)/k, and that applying a window on analysis and
// again on synthesis moves the condition onto the square of the window. Those
// two together say that a hop of half the window, which is fine for one Hann,
// is not fine for two. The test asserts both halves of that: flat at a quarter
// and an eighth, and demonstrably not flat at a half or a third. The second
// half is the point. A test that only checked the hops that work would pass
// just as happily against an implementation that measured nothing at all.
//
// Then perfect reconstruction, over sample rates from 22.05 to 192 kHz and
// lengths from one sample to several windows, with the head and the tail
// checked separately, because the first and last frames are where the overlap
// is incomplete and where an implementation that divides by the textbook
// constant instead of by what it actually accumulated fades in and out.
//
//   ./Tests/run.sh
//
// References:
//   Smith, Spectral Audio Signal Processing, "Overlap-Add (OLA) STFT
//   Processing" and "WOLA Processing Steps":
//   https://ccrma.stanford.edu/~jos/sasp/Overlap_Add_OLA_STFT_Processing.html
//   https://www.dsprelated.com/freebooks/sasp/WOLA_Processing_Steps.html
//   Griffin and Lim, IEEE ASSP-32(2), 1984.
// ---------------------------------------------------------------------------

#include "Spectrogram.h"

#include <cmath>
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
/** The spread of the accumulated window square away from the edges, where
    every frame that should overlap does. A flat interior is the overlap-add
    condition holding. */
static void interiorEnvelope (const Spectrogram& spectrogram, int length,
                              double& lowest, double& highest)
{
    std::vector<double> envelope;
    spectrogram.overlapEnvelope (length, envelope);

    lowest = 1.0e30;
    highest = -1.0e30;

    const int margin = spectrogram.getFftSize();

    for (int i = margin; i < length - margin; ++i)
    {
        lowest = std::min (lowest, envelope[(size_t) i]);
        highest = std::max (highest, envelope[(size_t) i]);
    }
}

static void theOverlapAddConstantIsWhatTheTextsSay()
{
    const int fftSize = 256;
    const int length = 8192;

    struct Case { int hop; bool flat; double constant; const char* name; };

    // A quarter and an eighth are the hops where the square of a Hann window
    // overlaps to a constant. The numbers are the sum of that square over the
    // frames that reach a sample, which is four frames of 0.375 average at a
    // quarter and eight at an eighth.
    const Case cases[] =
    {
        { fftSize / 2, false, 0.0, "half" },
        { fftSize / 4, true,  1.5, "a quarter" },
        { fftSize / 8, true,  3.0, "an eighth" },
        { fftSize / 16, true, 6.0, "a sixteenth" },
    };

    for (const auto& c : cases)
    {
        Spectrogram spectrogram;
        check (spectrogram.prepare (fftSize, c.hop, 48000.0),
               std::string ("a hop of ") + c.name + " of the window was refused");

        double lowest = 0.0, highest = 0.0;
        interiorEnvelope (spectrogram, length, lowest, highest);

        const double spread = highest - lowest;

        if (c.flat)
        {
            check (spread < 1.0e-9, std::string ("a hop of ") + c.name
                                    + " of the window should overlap to a constant, but it varies by "
                                    + std::to_string (spread));

            check (std::fabs (lowest - c.constant) < 1.0e-9,
                   std::string ("a hop of ") + c.name + " overlaps to " + std::to_string (lowest)
                   + " where " + std::to_string (c.constant) + " was expected");

            std::printf ("    hop %-12s overlaps to a flat %.4f\n", c.name, lowest);
        }
        else
        {
            // Stated as a requirement so that an implementation which somehow
            // flattened everything, including by measuring nothing, fails here.
            check (spread > 0.1, std::string ("a hop of ") + c.name
                                 + " of the window is not supposed to overlap to a constant, "
                                   "yet it varies by only " + std::to_string (spread));

            std::printf ("    hop %-12s varies from %.4f to %.4f, as it should\n",
                         c.name, lowest, highest);
        }
    }

    // A third of a window is not of the form (N/2)/k, so it is not a valid
    // overlap-add hop for a Hann window either way round.
    Spectrogram awkward;
    awkward.prepare (fftSize, fftSize / 3, 48000.0);

    double lowest = 0.0, highest = 0.0;
    interiorEnvelope (awkward, length, lowest, highest);

    check (highest - lowest > 1.0e-4, "a hop of a third of the window is not a valid overlap, "
                                      "yet it measured flat");
}

static void aClipComesBackExactly()
{
    std::mt19937 rng (4242);
    std::uniform_real_distribution<float> noise (-0.9f, 0.9f);

    double worstOverall = 0.0;

    for (double rate : { 22050.0, 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        for (int length : { 1, 2, 31, 255, 256, 257, 1000, 4096, 10007 })
        {
            Spectrogram spectrogram;
            check (spectrogram.prepare (512, 128, rate), "prepare was refused");

            std::vector<float> input ((size_t) length), output ((size_t) length, 0.0f);

            for (auto& s : input)
                s = noise (rng);

            spectrogram.analyse (input.data(), length);
            spectrogram.synthesise (output.data(), length);

            double worst = 0.0;
            int worstAt = 0;

            for (int i = 0; i < length; ++i)
            {
                check (std::isfinite (output[(size_t) i]),
                       "sample " + std::to_string (i) + " of a " + std::to_string (length)
                       + " sample clip came back as something that is not finite");

                const double error = std::fabs ((double) output[(size_t) i] - (double) input[(size_t) i]);

                if (error > worst)
                {
                    worst = error;
                    worstAt = i;
                }
            }

            // Single precision samples, summed over the frames that overlap
            // each one, so a few parts in ten million is the floor. Anything
            // structurally wrong is percent.
            check (worst < 1.0e-5, "a " + std::to_string (length) + " sample clip at "
                                   + std::to_string ((int) rate) + " Hz came back wrong by "
                                   + std::to_string (worst) + " at sample " + std::to_string (worstAt));

            worstOverall = std::max (worstOverall, worst);
        }
    }

    std::printf ("    worst round trip error across every rate and length: %.2e\n", worstOverall);
}

static void theHeadAndTailComeBackToo()
{
    // Called out separately because this is where an implementation that
    // divides by the textbook constant rather than by the window sum it
    // actually accumulated goes wrong, and where a clip-wide average error
    // would hide it.
    const int length = 4096;
    const int edge = 600;

    Spectrogram spectrogram;
    spectrogram.prepare (512, 128, 48000.0);

    std::vector<float> input ((size_t) length), output ((size_t) length, 0.0f);

    for (int i = 0; i < length; ++i)
        input[(size_t) i] = (float) (0.7 * std::sin (2.0 * pi * 440.0 * (double) i / 48000.0));

    spectrogram.analyse (input.data(), length);
    spectrogram.synthesise (output.data(), length);

    double head = 0.0, tail = 0.0;

    for (int i = 0; i < edge; ++i)
        head = std::max (head, std::fabs ((double) output[(size_t) i] - (double) input[(size_t) i]));

    for (int i = length - edge; i < length; ++i)
        tail = std::max (tail, std::fabs ((double) output[(size_t) i] - (double) input[(size_t) i]));

    check (head < 1.0e-5, "the first " + std::to_string (edge) + " samples came back wrong by "
                          + std::to_string (head));
    check (tail < 1.0e-5, "the last " + std::to_string (edge) + " samples came back wrong by "
                          + std::to_string (tail));

    std::printf ("    head %.2e, tail %.2e\n", head, tail);
}

static void theOverlapNeverGetsThinAtTheEdges()
{
    // Perfect reconstruction alone does not pin this down. Dividing by the
    // window sum that was actually accumulated returns an untouched clip
    // exactly however thin that sum gets, because the same thin number
    // divides out what it multiplied in. The moment an edit changes a frame
    // that cancellation stops holding, and whatever the edit did not get
    // exactly right is divided by that thin number and amplified. At the end
    // of a clip, which is where the frames that would have completed the
    // overlap are missing, that is the difference between a repair and a
    // thump on the last beat.
    //
    // So the requirement is that the sum never falls far below what it
    // reaches in the middle. Running the frames out one hop past the last
    // sample is what buys that; stopping at the last sample leaves the final
    // few hundred samples covered by a sixth of the window sum the rest of
    // the clip gets.
    struct Case { int fftSize; int hop; double constant; };

    const Case cases[] = { { 512, 128, 1.5 }, { 512, 64, 3.0 }, { 2048, 512, 1.5 } };

    for (const auto& c : cases)
    {
        Spectrogram spectrogram;
        spectrogram.prepare (c.fftSize, c.hop, 48000.0);

        double worstRatio = 1.0e30;
        int worstLength = 0;

        for (int length = 1; length <= 3000; ++length)
        {
            std::vector<double> envelope;
            spectrogram.overlapEnvelope (length, envelope);

            for (int i = 0; i < length; ++i)
            {
                const double ratio = envelope[(size_t) i] / c.constant;

                if (ratio < worstRatio)
                {
                    worstRatio = ratio;
                    worstLength = length;
                }
            }
        }

        check (worstRatio > 0.5, "with a window of " + std::to_string (c.fftSize) + " and a hop of "
                                 + std::to_string (c.hop) + " the overlap thins to "
                                 + std::to_string (worstRatio) + " of its interior value, in a clip of "
                                 + std::to_string (worstLength) + " samples");

        std::printf ("    window %4d hop %3d: the overlap never falls below %.3f of its interior value\n",
                     c.fftSize, c.hop, worstRatio);
    }
}

static void binsAndFramesMapBothWays()
{
    Spectrogram spectrogram;
    spectrogram.prepare (1024, 256, 44100.0);

    check (spectrogram.getNumBins() == 513, "wrong bin count");
    check (std::fabs (spectrogram.binToHertz (0)) < 1.0e-12, "bin zero is not direct current");
    check (std::fabs (spectrogram.binToHertz (512) - 22050.0) < 1.0e-9, "the last bin is not Nyquist");

    for (int bin : { 0, 1, 50, 300, 512 })
        check (std::fabs (spectrogram.hertzToBin (spectrogram.binToHertz (bin)) - (double) bin) < 1.0e-9,
               "bin " + std::to_string (bin) + " does not survive a round trip through hertz");

    for (int frame : { 0, 1, 10, 500 })
        check (std::fabs (spectrogram.secondsToFrame (spectrogram.frameToSeconds (frame)) - (double) frame) < 1.0e-9,
               "frame " + std::to_string (frame) + " does not survive a round trip through seconds");

    // Frames are centred, so frame k sits on sample k * hop and the first one
    // sits at time zero. A user interface draws a cursor at these.
    check (std::fabs (spectrogram.frameToSeconds (0)) < 1.0e-12, "the first frame is not at time zero");
    check (std::fabs (spectrogram.frameToSeconds (1) - 256.0 / 44100.0) < 1.0e-12,
           "the second frame is not one hop in");
}

static void aToneIsWhereItShouldBe()
{
    const double rate = 48000.0;
    const int fftSize = 2048;
    const int length = 48000;

    Spectrogram spectrogram;
    spectrogram.prepare (fftSize, fftSize / 4, rate);

    // Placed exactly on a bin so there is nothing to argue about: 93.75 Hz per
    // bin at this size and rate, so bin 32 is 3000 Hz.
    const int expectedBin = 32;
    const double hertz = (double) expectedBin * rate / (double) fftSize;

    std::vector<float> input ((size_t) length);

    for (int i = 0; i < length; ++i)
        input[(size_t) i] = (float) std::sin (2.0 * pi * hertz * (double) i / rate);

    spectrogram.analyse (input.data(), length);

    const int middle = spectrogram.getNumFrames() / 2;
    const auto* bins = spectrogram.frame (middle);

    int loudest = 0;

    for (int b = 0; b < spectrogram.getNumBins(); ++b)
        if (std::abs (bins[b]) > std::abs (bins[loudest]))
            loudest = b;

    check (loudest == expectedBin, "a " + std::to_string ((int) hertz) + " Hz tone read loudest in bin "
                                   + std::to_string (loudest) + " rather than "
                                   + std::to_string (expectedBin));
}

static void impossibleSettingsAreRefused()
{
    Spectrogram spectrogram;

    // A hop longer than half the window leaves samples only one frame reaches,
    // so there is no overlap to add and the window shape survives into the
    // output as an amplitude ripple.
    check (! spectrogram.prepare (512, 300, 48000.0), "a hop longer than half the window was accepted");
    check (! spectrogram.prepare (512, 0, 48000.0), "a hop of nothing was accepted");
    check (! spectrogram.prepare (500, 125, 48000.0), "a window that is not a power of two was accepted");
    check (! spectrogram.prepare (512, 128, 0.0), "a sample rate of nothing was accepted");
    check (! spectrogram.isPrepared(), "a refused setting left this looking usable");

    // And an unusable one has to be safe to drive, for the caller who did not
    // read the answer.
    std::vector<float> buffer (64, 1.0f);
    spectrogram.analyse (buffer.data(), 64);
    spectrogram.synthesise (buffer.data(), 64);
    check (spectrogram.getNumFrames() == 0, "an unusable spectrogram produced frames");

    check (spectrogram.prepare (512, 256, 48000.0), "a hop of exactly half the window was refused");
}

static void nothingIsAnalysedFromNothing()
{
    Spectrogram spectrogram;
    spectrogram.prepare (256, 64, 48000.0);

    spectrogram.analyse (nullptr, 0);
    check (spectrogram.getNumFrames() == 0, "an empty clip produced frames");

    std::vector<float> output (16, 7.0f);
    spectrogram.synthesise (output.data(), 0);
    check (output[0] == 7.0f, "synthesising nothing wrote something");
}

int main()
{
    std::printf ("spectrogram\n");

    theOverlapAddConstantIsWhatTheTextsSay();
    aClipComesBackExactly();
    theHeadAndTailComeBackToo();
    theOverlapNeverGetsThinAtTheEdges();
    binsAndFramesMapBothWays();
    aToneIsWhereItShouldBe();
    impossibleSettingsAreRefused();
    nothingIsAnalysedFromNothing();

    if (failures > 0)
    {
        std::printf ("\n%d spectrogram check(s) failed\n", failures);
        return 1;
    }

    std::printf ("all spectrogram checks passed\n");
    return 0;
}
