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

/** Returns what it was given, so that a check standing in front of something
    that would read out of bounds if it failed can be written as a guard. */
static bool check (bool ok, const std::string& what)
{
    if (! ok)
        fail (what);

    return ok;
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

//==============================================================================
/** The periodic Hann, written out here rather than read from the class under
    test, so that a check against it is a check and not an echo. */
static double hann (int i, int fftSize)
{
    return 0.5 - 0.5 * std::cos (2.0 * pi * (double) i / (double) fftSize);
}

/** The window sum squared that the synthesis has to divide by, accumulated
    from the window above over the frames the implementation says it has. */
static std::vector<double> envelopeFromTheDefinition (const Spectrogram& spectrogram,
                                                      int numSamples)
{
    const int fftSize = spectrogram.getFftSize();
    const int hop = spectrogram.getHop();
    std::vector<double> envelope ((size_t) numSamples, 0.0);

    for (int f = 0; f < spectrogram.getNumFrames(); ++f)
    {
        const int start = f * hop - fftSize / 2;

        for (int i = 0; i < fftSize; ++i)
        {
            const int at = start + i;

            if (at >= 0 && at < numSamples)
                envelope[(size_t) at] += hann (i, fftSize) * hann (i, fftSize);
        }
    }

    return envelope;
}

static void anEditInOneFrameComesOutShapedLikeTheWindow()
{
    // The check the rest of this file was missing. Every reconstruction above
    // runs over frames nothing has touched, and in that case the analysis
    // window and the accumulated envelope cancel each other: the same number
    // divides out what it multiplied in. So the round trip comes back exact
    // whatever the synthesis stage multiplies by, as long as it divides by the
    // same thing, and the taper on the way out, which is the whole reason this
    // is weighted overlap-add rather than plain overlap-add, goes unchecked.
    // Dropping the synthesis window and the square in the envelope together
    // passed every other check in this file.
    //
    // So: analyse silence, put something in one bin of one frame, and compare
    // what comes out against the closed form. A single bin of a single frame
    // is the one edit whose inverse transform can be written down without
    // transforming anything: bins b and N-b hold a conjugate pair, so the
    // block is (2A/N) cos(2 pi b i / N), and the output is that windowed and
    // divided by the window sum squared. That pins the synthesis window, its
    // exponent, the envelope, and where the frame sits, all against arithmetic
    // written out independently above.
    const int fftSize = 512, hop = 128, length = 8192;
    const int editedBin = 64;
    const double amplitude = 50.0;

    Spectrogram spectrogram;

    if (! check (spectrogram.prepare (fftSize, hop, 48000.0), "prepare was refused"))
        return;

    std::vector<float> silence ((size_t) length, 0.0f);
    spectrogram.analyse (silence.data(), length);

    // The first frame, one in the middle, and the last. The middle one is the
    // edit the arithmetic is simplest for, and it is also the one where the
    // envelope is its flat interior constant of 1.5, so on its own it says
    // nothing about the envelope: hard coding 1.5 would give the same answer.
    // The first and last frames are where the overlap is incomplete and the
    // envelope is 1.25 and climbing, which is also where a repair at the start
    // or end of a clip lands.
    const int frames[] = { 0, 20, spectrogram.getNumFrames() - 1 };

    double worstOverall = 0.0;

    for (int editedFrame : frames)
    {
        if (! check (editedFrame >= 0 && editedFrame < spectrogram.getNumFrames(),
                     "the frame being edited does not exist"))
            return;

        std::vector<float> output ((size_t) length, 0.0f);

        spectrogram.analyse (silence.data(), length);
        spectrogram.frame (editedFrame)[editedBin] = { amplitude, 0.0 };
        spectrogram.synthesise (output.data(), length);

        const auto envelope = envelopeFromTheDefinition (spectrogram, length);
        const int start = editedFrame * hop - fftSize / 2;

        double worst = 0.0, peak = 0.0;

        for (int i = 0; i < length; ++i)
            peak = std::max (peak, std::fabs ((double) output[(size_t) i]));

        check (peak > 1.0e-3, "the edit produced nothing to measure");

        for (int n = 0; n < length; ++n)
        {
            double expected = 0.0;

            if (n >= start && n < start + fftSize)
            {
                const int i = n - start;
                const double block = 2.0 * amplitude
                                   * std::cos (2.0 * pi * (double) editedBin * (double) i / (double) fftSize)
                                   / (double) fftSize;

                expected = block * hann (i, fftSize) / envelope[(size_t) n];
            }

            worst = std::max (worst, std::fabs ((double) output[(size_t) n] - expected));
        }

        check (worst < 1.0e-6 * peak,
               "an edit in frame " + std::to_string (editedFrame) + " did not come out as the window "
               "shaped burst the arithmetic says it should: worst departure " + std::to_string (worst)
               + " against a peak of " + std::to_string (peak));

        worstOverall = std::max (worstOverall, worst);
    }

    std::printf ("    a one frame edit matches the closed form to %.2e, at the head, the middle and the tail\n",
                 worstOverall);
}

static void theEnvelopeTheTestsMeasureIsTheOneSynthesiseDividesBy()
{
    // `overlapEnvelope` exists so the tests can measure the overlap-add
    // constant, and it is a second copy of the accumulation loop inside
    // `synthesise`. That is the shape CLAUDE.md warns about in the playback
    // against export rule: two loops over the same thing, where editing one
    // and not the other is silent. Adding two to the frame count inside
    // `overlapEnvelope` alone passed every other check in this file, and made
    // the edge figure this file prints look better than the truth, which is
    // the diagnostic a reader would trust.
    //
    // So both are tied to one window accumulated from the definition above.
    for (int hop : { 64, 128, 256 })
    {
        Spectrogram spectrogram;

        if (! check (spectrogram.prepare (512, hop, 48000.0), "prepare was refused"))
            return;

        for (int length : { 1, 129, 257, 600, 2048 })
        {
            std::vector<float> silence ((size_t) length, 0.0f);
            spectrogram.analyse (silence.data(), length);

            std::vector<double> measured;
            spectrogram.overlapEnvelope (length, measured);

            const auto expected = envelopeFromTheDefinition (spectrogram, length);

            double worst = 0.0;

            for (int i = 0; i < length; ++i)
                worst = std::max (worst, std::fabs (measured[(size_t) i] - expected[(size_t) i]));

            check (worst < 1.0e-12, "the envelope overlapEnvelope reports and the one accumulated "
                                    "from the window differ by " + std::to_string (worst)
                                    + " at a hop of " + std::to_string (hop) + " over "
                                    + std::to_string (length) + " samples");
        }
    }
}

static void anEditIsTaperedRatherThanDroppedIn()
{
    // The same fault said as what it sounds like, because the closed form
    // above is algebra and this is the complaint. An edit reaches one window's
    // worth of samples. Windowed on the way out it swells in and back out of
    // silence. Dropped in unwindowed it is a rectangle: it arrives at full
    // amplitude on its first sample and stops dead on its last, which is a
    // click at each end of every repair. Measured against a build with the
    // synthesis window taken out, the loudest the burst gets in the outer
    // eighth of its window went from 14.6 per cent of what it reaches in the
    // middle to the full hundred.
    const int fftSize = 512, hop = 128, length = 8192;
    const int editedFrame = 20, editedBin = 64;

    Spectrogram spectrogram;

    if (! check (spectrogram.prepare (fftSize, hop, 48000.0), "prepare was refused"))
        return;

    std::vector<float> silence ((size_t) length, 0.0f), output ((size_t) length, 0.0f);
    spectrogram.analyse (silence.data(), length);

    if (! check (editedFrame < spectrogram.getNumFrames() && editedBin < spectrogram.getNumBins(),
                 "the frame being edited does not exist"))
        return;

    spectrogram.frame (editedFrame)[editedBin] = { 50.0, 0.0 };
    spectrogram.synthesise (output.data(), length);

    const int start = editedFrame * hop - fftSize / 2;

    double peak = 0.0;

    for (int i = 0; i < length; ++i)
        peak = std::max (peak, std::fabs ((double) output[(size_t) i]));

    check (peak > 1.0e-3, "the edit produced nothing to measure");

    // Measured as the loudest the burst gets in each eighth of the window it
    // covers. A step between neighbouring samples is no use here: the burst
    // is a 6 kHz tone, which at this rate turns over in eight samples, so
    // neighbours differ by seventy per cent of the peak with nothing wrong at
    // all. The envelope is what tells the two apart, and under a Hann window
    // the first eighth reaches 0.15 of the middle.
    const auto loudestOverAnEighth = [&] (int eighth)
    {
        double loudest = 0.0;

        for (int i = eighth * fftSize / 8; i < (eighth + 1) * fftSize / 8; ++i)
            loudest = std::max (loudest, std::fabs ((double) output[(size_t) (start + i)]));

        return loudest;
    };

    const double atTheEdge = std::max (loudestOverAnEighth (0), loudestOverAnEighth (7));
    const double inTheMiddle = std::max (loudestOverAnEighth (3), loudestOverAnEighth (4));

    // A band and not just a ceiling. A ceiling alone says "tapered somehow",
    // and the taper has to be this window's and not another: a triangular
    // synthesis window measures 27.7 per cent and the window applied twice
    // measures 2.1, and both leave an untouched round trip exact, so neither
    // is visible anywhere else in this file except through the closed form
    // above. A Hann reaches 14.6, so the band has about a third either side.
    check (atTheEdge < 0.22 * inTheMiddle,
           "the edit is too loud at the edges of its frame, " + std::to_string (atTheEdge)
           + " against " + std::to_string (inTheMiddle)
           + ": it is being dropped in rather than tapered with this window");

    check (atTheEdge > 0.08 * inTheMiddle,
           "the edit is too quiet at the edges of its frame, " + std::to_string (atTheEdge)
           + " against " + std::to_string (inTheMiddle)
           + ": the taper is steeper than the window it is supposed to be");

    // And the first and last samples it reaches are the ends of a window, so
    // they are nearly nothing.
    for (int i : { 0, 1, fftSize - 2, fftSize - 1 })
        check (std::fabs ((double) output[(size_t) (start + i)]) < 0.02 * peak,
               "sample " + std::to_string (i) + " of the edited frame is "
               + std::to_string (std::fabs ((double) output[(size_t) (start + i)]))
               + " against a peak of " + std::to_string (peak)
               + ": the edit does not start and end at silence");

    // Nothing outside the one window the frame covers.
    for (int n = 0; n < length; ++n)
        if (n < start || n >= start + fftSize)
            check (std::fabs ((double) output[(size_t) n]) < 1.0e-9,
                   "an edit in one frame reached sample " + std::to_string (n)
                   + ", which is outside the window that frame covers");

    std::printf ("    a one frame edit reaches %.1f%% of its own peak at the edges of its window\n",
                 100.0 * atTheEdge / inTheMiddle);
}

static void theFrameCountIsOnePastTheLastFrameInsideTheClip()
{
    // Two things have to be true of the count and neither was checked: the
    // tail has to be covered, and nothing may be computed that contributes
    // nothing. One frame too few leaves the last few hundred samples held up
    // by a window that is still closing, which is what the envelope check
    // below it is about. One frame too many is a transform per clip spent on
    // a window that is entirely off the end, and a column a user interface
    // draws with nothing in it. Adding one and subtracting one from the
    // divisor both passed every other check in this file.
    const int hops[] = { 1, 2, 3, 64, 128, 256, 333 };
    const int lengths[] = { 1, 2, 3, 63, 64, 65, 127, 128, 129, 255, 256, 257, 1000, 4096, 10007 };

    for (int hop : hops)
    {
        Spectrogram spectrogram;

        if (! spectrogram.prepare (1024, hop, 48000.0))
            continue;

        for (int length : lengths)
        {
            const int count = spectrogram.frameCountFor (length);
            const std::string where = "a hop of " + std::to_string (hop) + " over "
                                    + std::to_string (length) + " samples";

            // The last frame's centre is strictly past the last sample, so
            // the tail is under a window that is still opening. Strictly, and
            // this is the whole of the check: write length - 1 as q hops and a
            // remainder r. The right count is q + 2, and a count of q + 1
            // satisfies "at or past" whenever r is nothing, which is every
            // length the hop divides into: every clip at a hop of one, every
            // other one at a hop of two. Taking one off the divisor, which is
            // short by a frame at exactly those lengths, passed this check
            // while it read >= and passed every other check in the file.
            check ((long long) (count - 1) * hop > (long long) (length - 1),
                   where + " leaves the last sample at or past the final frame's centre");

            // And it is the first frame of which that is true, so none is
            // wasted. These two together admit one count and no other, for
            // every hop and length below.
            check ((long long) (count - 2) * hop <= (long long) (length - 1),
                   where + " counts a frame whose centre is already past the end of the clip");
        }

        check (spectrogram.frameCountFor (0) == 0, "an empty clip needs no frames");
        check (spectrogram.frameCountFor (-5) == 0, "a negative length needs no frames");
    }
}

static void theEnvelopeFloorIsNeverReachedInsideTheClip()
{
    // `synthesise` guards its division with a floor of 1e-9, and the comment
    // above it claims the floor only ever bites where no frame reached at all.
    // Nothing produced that number, so here it is: the thinnest the sum gets
    // anywhere inside a clip analysed at its own length.
    //
    // The cases are the ones that can reach the minimum, which is not every
    // hop the class accepts. The envelope only grows as frames are added, so
    // for a given sample the thinnest configuration is the shortest clip that
    // contains it, and the fewer frames overlap the thinner it gets: the
    // minimum therefore lives at the largest hop allowed, half the window.
    // Swept exhaustively that way, over every power of two window from 8 to
    // 4096 and every hop from 1 to half the window, the global minimum is
    // exactly 0.5 and it is reached only at a hop of half the window. So these
    // are the hops of a half, at three window sizes, plus two smaller hops to
    // show the figure climbing. A hop of two was measured as well and is left
    // out: it reads 1.9988 and cost a third of this file's whole running time.
    struct Case { int fftSize; int hop; };
    const Case cases[] = { { 512, 256 }, { 256, 128 }, { 2048, 1024 },
                           { 512, 128 }, { 2048, 512 } };

    double thinnest = 1.0e30;

    for (const auto& c : cases)
    {
        Spectrogram spectrogram;
        check (spectrogram.prepare (c.fftSize, c.hop, 48000.0), "prepare was refused");

        for (int length = 1; length <= 2000; ++length)
        {
            std::vector<double> envelope;
            spectrogram.overlapEnvelope (length, envelope);

            for (int i = 0; i < length; ++i)
                thinnest = std::min (thinnest, envelope[(size_t) i]);
        }
    }

    check (thinnest > 0.4, "the overlap thins to " + std::to_string (thinnest)
                           + " somewhere inside a clip, which is close enough to the floor in "
                             "synthesise to be worth knowing about");

    std::printf ("    the thinnest overlap anywhere inside a clip is %.4f, against a floor of 1e-9\n",
                 thinnest);

    // The other half of the claim: a caller that asks for more than it
    // analysed is where the floor does bite, and it gets silence rather than a
    // division by nothing.
    Spectrogram spectrogram;
    spectrogram.prepare (512, 128, 48000.0);

    std::vector<float> input (1000, 0.5f);
    spectrogram.analyse (input.data(), 1000);

    std::vector<float> output (4000, 7.0f);
    spectrogram.synthesise (output.data(), 4000);

    for (int i = 2000; i < 4000; ++i)
        check (output[(size_t) i] == 0.0f,
               "asking for more samples than were analysed gave something other than silence at "
               + std::to_string (i));
}

static void everyHopThatIsAcceptedReconstructs()
{
    // The round trip above runs at one hop, a quarter of the window, and
    // `prepare` accepts anything from one sample up to half the window. A hop
    // of half is the one with the least overlap, and it is where the window
    // sum thins to a half, the closest anything inside a clip comes to the
    // floor `synthesise` divides against. Raising that floor to six tenths,
    // which turns those samples into silence, passed every other check in this
    // file because nothing reconstructed at that hop.
    //
    // Hops that do not divide into the window are in here too. They are not
    // valid overlap-add hops and the envelope they produce is not flat, but
    // dividing by what was actually accumulated means an untouched clip still
    // has to come back exactly, and a caller is free to ask for one.
    std::mt19937 rng (31337);
    std::uniform_real_distribution<float> noise (-0.9f, 0.9f);

    double worstOverall = 0.0;
    int worstHop = 0;

    for (int hop : { 1, 3, 64, 128, 171, 192, 256 })
    {
        Spectrogram spectrogram;
        check (spectrogram.prepare (512, hop, 48000.0),
               "a hop of " + std::to_string (hop) + " was refused");

        for (int length : { 1, 2, 31, 129, 255, 256, 257, 300, 1000, 4096 })
        {
            std::vector<float> input ((size_t) length), output ((size_t) length, 0.0f);

            for (auto& sample : input)
                sample = noise (rng);

            spectrogram.analyse (input.data(), length);
            spectrogram.synthesise (output.data(), length);

            double worst = 0.0;
            int worstAt = 0;

            for (int i = 0; i < length; ++i)
            {
                const double error = std::fabs ((double) output[(size_t) i]
                                                - (double) input[(size_t) i]);

                if (error > worst)
                {
                    worst = error;
                    worstAt = i;
                }
            }

            check (worst < 1.0e-5, "a " + std::to_string (length) + " sample clip at a hop of "
                                   + std::to_string (hop) + " came back wrong by "
                                   + std::to_string (worst) + " at sample "
                                   + std::to_string (worstAt));

            if (worst > worstOverall)
            {
                worstOverall = worst;
                worstHop = hop;
            }
        }
    }

    std::printf ("    worst round trip across every accepted hop: %.2e, at a hop of %d\n",
                 worstOverall, worstHop);
}

static void preparingAgainForgetsTheLastClip()
{
    // The window size is a setting somebody changes while looking at a clip,
    // so this is an ordinary sequence and not a contrived one. Clearing the
    // frames and leaving the count behind reports frames that are not there,
    // and the next caller to walk them reads past the end of an empty vector.
    // It survived every other check in this file because nothing prepared
    // twice.
    Spectrogram spectrogram;
    check (spectrogram.prepare (512, 128, 48000.0), "prepare was refused");

    std::vector<float> input (4000, 0.25f);
    spectrogram.analyse (input.data(), 4000);
    check (spectrogram.getNumFrames() > 0, "a clip produced no frames");

    check (spectrogram.prepare (256, 64, 44100.0), "preparing again was refused");
    check (spectrogram.getNumFrames() == 0, "preparing again left the last clip's frame count behind");
    check (spectrogram.getAnalysedLength() == 0, "preparing again left the last clip's length behind");
    check (spectrogram.getFftSize() == 256, "preparing again did not take the new window size");
    check (spectrogram.getHop() == 64, "preparing again did not take the new hop");
    check (spectrogram.getNumBins() == 129, "preparing again did not take the new bin count");

    // And it is usable at the new size, which is the point of allowing it.
    spectrogram.analyse (input.data(), 4000);
    std::vector<float> output (4000, 0.0f);
    spectrogram.synthesise (output.data(), 4000);

    double worst = 0.0;

    for (int i = 0; i < 4000; ++i)
        worst = std::max (worst, std::fabs ((double) output[(size_t) i] - 0.25));

    check (worst < 1.0e-5, "a clip analysed after preparing again came back wrong by "
                           + std::to_string (worst));

    // And a refused one. "Unprepared" has to mean the same thing whichever way
    // it was arrived at: reporting the previous clip's frame count while
    // isPrepared says no is a state no caller should have to know about.
    check (! spectrogram.prepare (500, 125, 48000.0), "a window that is not a power of two was accepted");
    check (! spectrogram.isPrepared(), "a refused setting left this looking usable");
    check (spectrogram.getNumFrames() == 0, "a refused setting left the last clip's frame count behind");
    check (spectrogram.getAnalysedLength() == 0, "a refused setting left the last clip's length behind");
}

static void aNegativeLengthIsNothingRatherThanNonsense()
{
    // A length arrives from a clip somewhere else, so it is worth being sure
    // that a nonsense one reads back as nothing rather than as itself. The
    // accessor is what a caller sizes a buffer from.
    Spectrogram spectrogram;
    spectrogram.prepare (256, 64, 48000.0);

    std::vector<float> input (64, 1.0f);
    spectrogram.analyse (input.data(), -17);

    check (spectrogram.getAnalysedLength() == 0, "a negative length was reported back as itself");
    check (spectrogram.getNumFrames() == 0, "a negative length produced frames");
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
    anEditInOneFrameComesOutShapedLikeTheWindow();
    anEditIsTaperedRatherThanDroppedIn();
    theFrameCountIsOnePastTheLastFrameInsideTheClip();
    theEnvelopeFloorIsNeverReachedInsideTheClip();
    theEnvelopeTheTestsMeasureIsTheOneSynthesiseDividesBy();
    everyHopThatIsAcceptedReconstructs();
    preparingAgainForgetsTheLastClip();
    aNegativeLengthIsNothingRatherThanNonsense();
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
