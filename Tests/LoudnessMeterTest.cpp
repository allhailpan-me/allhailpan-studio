// ---------------------------------------------------------------------------
// Checks loudness measurement against ITU-R BS.1770-4 and EBU Tech 3341.
//
// A loudness figure is a claim about agreement. It is worth having only
// because every streaming service, broadcaster and other meter computes the
// same number from the same audio, so "-14 LUFS" means one thing. A meter
// that is half a decibel out is not slightly wrong, it is making a claim it
// cannot support, and nothing about its output looks wrong.
//
// There is a known way to get this subtly wrong, which is why the filter
// coefficients are checked against the standard's own table rather than only
// the readings: designing the K-weighting with a general purpose RBJ shelf
// instead of the standard's prototype gives a response about a quarter of a
// decibel low at 1 kHz, which is enough to fail compliance and far too small
// to notice by listening.
//
//   ./Tests/run.sh
//
// The checks below the compliance cases were added after mutation testing, in
// each case because a deliberate breakage of the meter survived everything
// above them. The compliance cases all put the same signal in both channels
// and all measure a steady tone, which leaves a surprising amount unpinned:
// which channels are read, how long the two windows are, where the gates sit,
// whether the peak reading has an upper bound, and what reset does.
//
// Sources:
//   ITU-R BS.1770-4, Tables 1 and 2 (K-weighting coefficients at 48 kHz)
//   ITU-R BS.1770-4, section 2 (the channel sum, each of a stereo pair
//     weighing 1.0) and the 400 ms and 3 s window lengths
//   EBU Tech 3341, Table 1, cases 1, 2, 3 and 9
//   EBU Tech 3342, for the loudness range: the spread between the 95th and
//     the 10th percentile of the short term readings, gated at -70 LUFS
//     absolutely and then 20 LU below the mean of what survives that
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

// --- counting allocations --------------------------------------------------
//
// The meter's header opens by saying everything in it runs on the audio
// thread, which cannot allocate, and that is why the history is kept as fixed
// histograms rather than as growing lists. Nothing checked it. Replacing the
// global operators is the only way to check it from outside, since what is
// being asserted is the absence of a call rather than the value of anything.

// Replacing them means the pairing is new against malloc and delete against
// free, which is the correct pairing for a replacement and which GCC's
// mismatched-new-delete analysis reports anyway because it only sees one side
// of it at a time.
#if defined (__GNUC__) && ! defined (__clang__)
 #pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

static bool allocationsWatched = false;
static int  allocations = 0;

void* operator new (std::size_t bytes)
{
    if (allocationsWatched)
        ++allocations;

    void* p = std::malloc (bytes == 0 ? 1 : bytes);

    if (p == nullptr)
        throw std::bad_alloc();

    return p;
}

void* operator new[] (std::size_t bytes)                   { return operator new (bytes); }
void  operator delete (void* p) noexcept                   { std::free (p); }
void  operator delete[] (void* p) noexcept                 { std::free (p); }
void  operator delete (void* p, std::size_t) noexcept      { std::free (p); }
void  operator delete[] (void* p, std::size_t) noexcept    { std::free (p); }

// --- the JUCE surface LoudnessMeter.h uses ---------------------------------

namespace juce
{
    template <typename T> T jlimit (T lo, T hi, T v)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    template <typename T> struct MathConstants
    {
        static constexpr T pi = (T) 3.141592653589793238462643383279502884L;
    };
}

#include "LoudnessMeter.h"

// --- harness ---------------------------------------------------------------

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

static double dbToAmplitude (double db) { return std::pow (10.0, db / 20.0); }

/** A stereo tone, the same in both channels, as every one of these test
    cases specifies. */
struct Tone
{
    double frequency = 1000.0;
    double levelDb   = -23.0;       // peak level per channel
    double seconds   = 20.0;
};

/** Feeds a sequence of tones through the meter at the given sample rate,
    in blocks, the way the engine does. */
static void feed (LoudnessMeter& meter, const std::vector<Tone>& tones,
                  double sampleRate, int blockSize = 512)
{
    std::vector<float> left, right;

    double phase = 0.0;

    for (const auto& tone : tones)
    {
        const auto n = (size_t) std::llround (tone.seconds * sampleRate);
        const double amplitude = dbToAmplitude (tone.levelDb);
        const double step = 2.0 * pi * tone.frequency / sampleRate;

        for (size_t i = 0; i < n; ++i)
        {
            const auto s = (float) (amplitude * std::sin (phase));
            left.push_back (s);
            right.push_back (s);
            phase += step;

            if (phase > 2.0 * pi)
                phase -= 2.0 * pi;
        }
    }

    for (size_t pos = 0; pos < left.size(); pos += (size_t) blockSize)
    {
        const int n = (int) std::min ((size_t) blockSize, left.size() - pos);
        const float* channels[2] = { left.data() + pos, right.data() + pos };
        meter.process (channels, 2, n);
    }
}

// --- the checks ------------------------------------------------------------

/** The K-weighting response the standard defines, computed from the published
    coefficients at 48 kHz. */
static double publishedKWeightingDb (double frequency)
{
    const std::complex<double> z = std::exp (std::complex<double> (0.0, -2.0 * pi * frequency / 48000.0));

    const auto response = [&z] (double b0, double b1, double b2, double a1, double a2)
    {
        return std::abs ((b0 + b1 * z + b2 * z * z) / (1.0 + a1 * z + a2 * z * z));
    };

    // BS.1770-4 Table 1, stage 1 (the shelf), and Table 2, stage 2 (the
    // highpass), exactly as printed.
    const double shelf = response (1.53512485958697, -2.69169618940638, 1.19839281085285,
                                  -1.69065929318241,  0.73248077421585);

    const double highpass = response (1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621);

    return 20.0 * std::log10 (shelf * highpass);
}

/** Recovers the meter's own weighting curve by measuring it.

    The meter keeps its filters private, and it should: exposing them so a
    test could read them back would only prove the header agrees with itself.
    Driving it with tones and solving for the gain measures what the filters
    actually do, through the same path the engine uses, so a filter that is
    designed correctly but wired up wrong, run at the wrong sample rate, or
    applied once instead of twice has nowhere to hide.

    For a steady tone the reading is
        LUFS = -0.691 + 20*log10(amplitude) + 20*log10(|H(f)|)
    so the weighting at that frequency falls straight out of the number on
    the meter. */
static double measuredKWeightingDb (double frequency, double sampleRate)
{
    const double amplitude = 0.25;

    // One second, not two. The reading is taken over the last 400 ms of it,
    // and a second and two seconds of a steady tone were measured as
    // identical to six decimal places at every frequency here, so the second
    // second was only costing the suite time.
    LoudnessMeter meter;
    meter.prepare (sampleRate);
    feed (meter, { { frequency, 20.0 * std::log10 (amplitude), 1.0 } }, sampleRate);

    return meter.getMomentaryLufs() + 0.691 - 20.0 * std::log10 (amplitude);
}

static void kWeightingMatchesTheStandard()
{
    std::printf ("the meter's own weighting curve matches the standard's\n");

    // Across the band the curve actually shapes: the highpass corner, the
    // shelf's transition, and the flat regions either side of both.
    for (double f : { 40.0, 60.0, 100.0, 200.0, 500.0, 1000.0, 1500.0, 2000.0,
                      3000.0, 5000.0, 8000.0, 12000.0, 16000.0 })
    {
        const double want = publishedKWeightingDb (f);
        const double got  = measuredKWeightingDb (f, 48000.0);

        // A thousandth of a decibel, not the thirtieth this once allowed.
        // Designing from the prototype reproduces the standard's own 48 kHz
        // table to five decimal places, because the table is that prototype
        // bilinear transformed at this rate, so the measurement's own floor is
        // a hundred thousandth of a decibel and a tolerance of 0.03 was
        // throwing away three orders of magnitude of it. Writing sin where tan
        // belongs costs 0.017 dB and used to pass.
        if (std::abs (got - want) > 0.002)
            fail ("at " + std::to_string ((int) f) + " Hz the meter weights by "
                  + std::to_string (got) + " dB, the standard says "
                  + std::to_string (want));
    }
}

/** The same curve at every rate a device can open at.

    The sweep above establishes the curve at 48 kHz only, and the two checks
    that do visit other rates both use a 1 kHz tone, where the 38 Hz highpass
    is flat to within a hundredth of a decibel. So the highpass could have
    been designed at a fixed 48 kHz and nothing would have noticed: at 96 kHz
    its corner would sit at 19 Hz instead of 38, which is more than a decibel
    out at 40 Hz and invisible everywhere the suite was looking.

    Measured relative to each rate's own reading at 1 kHz, because the
    standard's highpass numerator is not normalised, so its passband gain is
    1 + k/q + k squared and moves by about two hundredths of a decibel between
    rates. That is the calibration, which the check below this one pins. What
    is pinned here is the shape, which is what the weighting is. */
static void theWeightingCurveIsTheSameAtEveryRate()
{
    std::printf ("the weighting curve is the same curve at every sample rate\n");

    for (double sampleRate : { 44100.0, 88200.0, 96000.0, 192000.0 })
    {
        const double atOneKilohertz = measuredKWeightingDb (1000.0, sampleRate);

        for (double f : { 40.0, 100.0, 500.0, 1500.0, 3000.0, 8000.0 })
        {
            if (f > 0.4 * sampleRate)
                continue;

            const double want = publishedKWeightingDb (f) - publishedKWeightingDb (1000.0);
            const double got  = measuredKWeightingDb (f, sampleRate) - atOneKilohertz;

            // Two hundredths, which is the bilinear frequency warping between
            // this rate and the 48 kHz the table is printed at. It is largest
            // at 192 kHz, where it measures 0.013 dB.
            if (std::abs (got - want) > 0.02)
                fail ("at " + std::to_string ((int) sampleRate) + " Hz the curve is "
                      + std::to_string (got) + " dB at " + std::to_string ((int) f)
                      + " Hz relative to 1 kHz, where the standard's is " + std::to_string (want));
        }
    }
}

/** The whole measurement is calibrated by one number: the -0.691 offset
    exists so that the K-weighting's own gain at 1 kHz cancels out and a
    1 kHz tone reads its own level. If the gain there is not +0.691 dB, every
    reading is off by the difference, which is the failure that makes a meter
    disagree with every other meter while looking perfectly healthy. */
static void gainAtOneKilohertzIsWhatTheOffsetAssumes()
{
    std::printf ("the gain at 1 kHz is the 0.691 dB the offset cancels, at every rate\n");

    for (double sampleRate : { 44100.0, 48000.0, 88200.0, 96000.0 })
    {
        const double gain = measuredKWeightingDb (1000.0, sampleRate);

        // The 0.691 figure is exact only at the rate the standard tabulates.
        // Designing from the analogue prototype rather than hard coding the
        // 48 kHz table makes it drift by hundredths of a decibel elsewhere,
        // which is well inside the 0.1 LU compliance allows, and far better
        // than the quarter of a decibel a general purpose shelf would cost.
        if (std::abs (gain - 0.691) > 0.03)
            fail ("at " + std::to_string ((int) sampleRate) + " Hz the gain at 1 kHz is "
                  + std::to_string (gain) + " dB, the offset assumes 0.691");
    }
}

static void ebuCase (const char* name, const std::vector<Tone>& tones,
                     bool checkMomentary, bool checkShortTerm, bool checkIntegrated,
                     double expected, double sampleRate = 48000.0)
{
    LoudnessMeter meter;
    meter.prepare (sampleRate);
    feed (meter, tones, sampleRate);

    const double tolerance = 0.1;

    const auto one = [&] (const char* which, double got)
    {
        const bool ok = std::abs (got - expected) <= tolerance;
        std::printf ("    %-28s %s %+7.2f LUFS   %s\n", name, which, got, ok ? "ok" : "OUT OF TOLERANCE");

        if (! ok)
            fail (std::string (name) + " " + which + ": read " + std::to_string (got)
                  + ", the standard requires " + std::to_string (expected) + " +/- 0.1");
    };

    if (checkMomentary)  one ("M", meter.getMomentaryLufs());
    if (checkShortTerm)  one ("S", meter.getShortTermLufs());
    if (checkIntegrated) one ("I", meter.getIntegratedLufs());
}

static void ebuComplianceCases()
{
    std::printf ("EBU Tech 3341 cases 1, 2 and 3\n");

    // Case 1: a 1 kHz stereo tone at -23 dBFS for 20 s reads -23.0 on all
    // three meters. This is the calibration case: if it passes, the chain
    // from filter through mean square to the offset is right.
    ebuCase ("1   1 kHz, -23 dBFS, 20 s", { { 1000.0, -23.0, 20.0 } },
             true, true, true, -23.0);

    // Case 2: the same ten decibels down, which checks the measurement is
    // linear rather than calibrated at one level.
    ebuCase ("2   1 kHz, -33 dBFS, 20 s", { { 1000.0, -33.0, 20.0 } },
             true, true, true, -33.0);

    // Case 3: quiet, loud, quiet. Only the integrated meter is specified
    // here, and it is the gating that is under test: the -36 dBFS passages
    // sit more than 10 LU below the average and must be excluded, so the
    // answer is the loud section's own level rather than a blend.
    ebuCase ("3   -36, -23, -36 gated",
             { { 1000.0, -36.0, 10.0 }, { 1000.0, -23.0, 60.0 }, { 1000.0, -36.0, 10.0 } },
             false, false, true, -23.0);
}

/** Case 9 is the one that tests the short term window itself rather than the
    calibration. Its signal alternates 1.34 s at -20 dBFS with 1.66 s at
    -30 dBFS, which averages to -23 LUFS over exactly three seconds, so a
    correct three second window reads -23.0 and holds there.

    This is the case worth having because the short term reading here is not
    computed over a plain three second window: it is the mean of thirty
    overlapping 400 ms blocks, which spans 3.3 s and weights the middle of
    that span more heavily than the ends. For a signal whose period is
    exactly the window length those two come to the same answer, which this
    confirms rather than assumes. */
static void ebuCase9()
{
    std::printf ("EBU Tech 3341 case 9\n");

    std::vector<Tone> tones;
    for (int i = 0; i < 5; ++i)
    {
        tones.push_back ({ 1000.0, -20.0, 1.34 });
        tones.push_back ({ 1000.0, -30.0, 1.66 });
    }

    LoudnessMeter meter;
    meter.prepare (48000.0);
    feed (meter, tones, 48000.0);

    const double got = meter.getShortTermLufs();
    const bool ok = std::abs (got - (-23.0)) <= 0.1;

    std::printf ("    %-28s S %+7.2f LUFS   %s\n",
                 "9   -20/-30 alternating", got, ok ? "ok" : "OUT OF TOLERANCE");

    if (! ok)
        fail ("case 9 short term: read " + std::to_string (got)
              + ", the standard requires -23.0 +/- 0.1");
}

static void measurementDoesNotDependOnSampleRate()
{
    std::printf ("the same programme reads the same at any sample rate\n");

    for (double sampleRate : { 44100.0, 48000.0, 88200.0, 96000.0 })
    {
        LoudnessMeter meter;
        meter.prepare (sampleRate);
        feed (meter, { { 1000.0, -23.0, 10.0 } }, sampleRate);

        const double got = meter.getIntegratedLufs();

        if (std::abs (got - (-23.0)) > 0.1)
            fail ("at " + std::to_string ((int) sampleRate) + " Hz a -23 dBFS tone read "
                  + std::to_string (got));
    }
}

static void blockSizeDoesNotChangeTheReading()
{
    std::printf ("the reading does not depend on how the device cuts the blocks\n");

    double reference = 0.0;

    for (int blockSize : { 1, 64, 441, 480, 512, 2048 })
    {
        LoudnessMeter meter;
        meter.prepare (48000.0);
        feed (meter, { { 1000.0, -23.0, 10.0 } }, 48000.0, blockSize);

        const double got = meter.getIntegratedLufs();

        if (blockSize == 1)
            reference = got;
        else if (std::abs (got - reference) > 0.01)
            fail ("block size " + std::to_string (blockSize) + " read " + std::to_string (got)
                  + " where single sample blocks read " + std::to_string (reference));
    }
}

static void silenceDoesNotReadAsLoud()
{
    std::printf ("silence does not produce a reading\n");

    LoudnessMeter meter;
    meter.prepare (48000.0);

    std::vector<float> zeros (48000, 0.0f);
    const float* channels[2] = { zeros.data(), zeros.data() };

    for (int i = 0; i < 10; ++i)
        meter.process (channels, 2, (int) zeros.size());

    check (meter.getIntegratedLufs() < -60.0,
           "silence read " + std::to_string (meter.getIntegratedLufs()) + " LUFS");
    check (meter.getTruePeak() == 0.0f, "silence produced a peak");
}

static void truePeakIsWiredUp()
{
    std::printf ("the meter reports true peak, not the largest stored sample\n");

    // The same signal as EBU case 19, which reaches +3 dBTP while no sample
    // in it reaches full scale. A meter reporting sample peak would say this
    // is safely under the ceiling.
    LoudnessMeter meter;
    meter.prepare (48000.0);

    std::vector<float> tone (48000);
    for (size_t i = 0; i < tone.size(); ++i)
        tone[i] = (float) (1.41 * std::sin (2.0 * pi * 0.25 * (double) i + pi / 4.0));

    for (int i = 0; i < 480; ++i)
    {
        const float w = (float) (0.5 - 0.5 * std::cos (pi * i / 480));
        tone[(size_t) i] *= w;
        tone[tone.size() - 1 - (size_t) i] *= w;
    }

    const float* channels[2] = { tone.data(), tone.data() };
    meter.process (channels, 2, (int) tone.size());

    float samplePeak = 0.0f;
    for (float s : tone)
        samplePeak = std::max (samplePeak, std::abs (s));

    const double measured = 20.0 * std::log10 (meter.getTruePeak());
    const double sampled  = 20.0 * std::log10 (samplePeak);

    check (sampled < 0.0, "the test signal should not reach full scale in its samples");
    check (measured > 2.5, "the meter read " + std::to_string (measured)
                           + " dBTP where the signal reaches +3");
    check (measured - sampled > 2.5,
           "true peak and sample peak differed by only " + std::to_string (measured - sampled));
}

/** Loudness range, to EBU Tech 3342.

    Three cases, because the gating is the part that is easy to get wrong and
    impossible to spot from the number. A programme with a silent lead in is
    the one that matters: ungated, the quiet end of the distribution is the
    silence rather than the quietest music, and the meter reports an enormous
    range for a track that is actually squashed flat. That is the shape of
    failure this program has had before: a figure that is present, plausible
    and wrong.
*/
static void loudnessRangeFollowsTech3342()
{
    std::printf ("EBU Tech 3342 loudness range\n");

    const auto rangeOf = [] (const std::vector<Tone>& tones)
    {
        LoudnessMeter meter;
        meter.prepare (48000.0);
        feed (meter, tones, 48000.0);
        return meter.getLoudnessRange();
    };

    // Something that never changes level has no range. Not exactly zero,
    // because the window still slides over the start of the tone, but close.
    const double flat = rangeOf ({ { 1000.0, -23.0, 30.0 } });
    std::printf ("    %-34s %5.2f LU\n", "steady tone", flat);
    check (flat < 1.0, "a steady tone read a loudness range of " + std::to_string (flat));

    // Alternating ten decibels apart, in passages long enough for the three
    // second window to settle on each. The 95th and 10th percentiles then sit
    // on the two levels, so the answer is the ten decibels between them.
    std::vector<Tone> alternating;
    for (int i = 0; i < 5; ++i)
    {
        alternating.push_back ({ 1000.0, -18.0, 6.0 });
        alternating.push_back ({ 1000.0, -28.0, 6.0 });
    }
    const double tenApart = rangeOf (alternating);
    std::printf ("    %-34s %5.2f LU\n", "two levels 10 LU apart", tenApart);
    check (std::abs (tenApart - 10.0) <= 1.0,
           "two levels ten decibels apart read a range of " + std::to_string (tenApart));

    // The gating case, and the reason the gates are in the standard. A steady
    // passage with a quiet fade either side of it, thirty LU down but nowhere
    // near silent, which is what the top and tail of a real track look like.
    // The relative gate is what has to remove it: at -53 LUFS the fade is far
    // above the absolute gate, so nothing else can. Ungated, the quiet end of
    // the distribution is the fade rather than the quietest music and this
    // reads about thirty LU for a track that never changes level at all.
    const double withFades = rangeOf ({ { 1000.0, -50.0, 10.0 },
                                        { 1000.0, -20.0, 40.0 },
                                        { 1000.0, -50.0, 10.0 } });
    std::printf ("    %-34s %5.2f LU\n", "steady, with quiet fades either end", withFades);
    check (withFades < 3.0,
           "a quiet fade either side of a steady passage inflated the range to "
               + std::to_string (withFades));

    // Digital silence either end has to go the same way, since a rendered
    // track usually has some. The bound is looser here because the three
    // second window necessarily straddles the edge of the silence, so a
    // couple of LU of the answer is the window smearing across a 100 dB step
    // rather than the gate failing. Ungated this reads around fifty.
    const double withSilence = rangeOf ({ { 1000.0, -120.0, 8.0 },
                                          { 1000.0, -20.0, 30.0 },
                                          { 1000.0, -120.0, 8.0 } });
    std::printf ("    %-34s %5.2f LU\n", "steady, with silence either end", withSilence);
    check (withSilence < 4.0,
           "silence either side of a steady passage inflated the range to "
               + std::to_string (withSilence));

    // And a quiet passage that is only 8 LU down is music, not silence, so it
    // stays in: the relative gate is at 20 LU, and a gate that swallowed this
    // would flatter every mix.
    std::vector<Tone> gentle;
    for (int i = 0; i < 5; ++i)
    {
        gentle.push_back ({ 1000.0, -16.0, 6.0 });
        gentle.push_back ({ 1000.0, -24.0, 6.0 });
    }
    const double eightApart = rangeOf (gentle);
    std::printf ("    %-34s %5.2f LU\n", "two levels 8 LU apart", eightApart);
    check (eightApart > 5.0,
           "a passage 8 LU down was gated out: the range read " + std::to_string (eightApart));
}

/** The readings must not depend on how long the meter has been running.

    This is here because of how the history is stored. The integrated reading
    and the percentiles come out of fixed histograms rather than a growing
    list of every block, which is what keeps the audio thread free of
    allocation, and the risk that buys is quantisation: a bin is a hundredth
    of an LU wide, and a reading that drifted as bins filled would show up as
    a figure that slowly disagreed with every other meter.
*/
static void theReadingsDoNotDriftWithLength()
{
    std::printf ("a longer programme at the same level reads the same\n");

    double first = 0.0;

    for (double seconds : { 20.0, 60.0, 150.0 })
    {
        LoudnessMeter meter;
        meter.prepare (48000.0);
        feed (meter, { { 1000.0, -23.0, seconds } }, 48000.0);

        const double got = meter.getIntegratedLufs();
        std::printf ("    %6.0f s   I %+7.2f LUFS\n", seconds, got);

        if (first == 0.0)
            first = got;

        check (std::abs (got - (-23.0)) <= 0.1,
               "after " + std::to_string ((int) seconds) + " s the integrated reading is "
                   + std::to_string (got));
        check (std::abs (got - first) <= 0.02,
               "the integrated reading drifted by " + std::to_string (std::abs (got - first))
                   + " LU between a 20 s and a " + std::to_string ((int) seconds) + " s programme");
    }
}

//==============================================================================
// Everything below was added by the hardening audit of 2026-10-29. Each check
// exists because a deliberate breakage of LoudnessMeter.h survived the checks
// above it.

/** Feeds one block sequence with independent content per channel.

    Every case above this point puts the same signal in both channels, which
    is what the EBU compliance cases specify and is also why a meter that
    ignored one channel entirely passed all of them. */
static void feedChannels (LoudnessMeter& meter,
                          const std::vector<float>& left,
                          const std::vector<float>& right,
                          int numChannels = 2, int blockSize = 512)
{
    for (size_t pos = 0; pos < left.size(); pos += (size_t) blockSize)
    {
        const int n = (int) std::min ((size_t) blockSize, left.size() - pos);
        const float* channels[2] = { left.data() + pos, right.data() + pos };
        meter.process (channels, numChannels, n);
    }
}

static std::vector<float> sine (double frequency, double levelDb, double seconds, double sampleRate)
{
    std::vector<float> v ((size_t) std::llround (seconds * sampleRate));
    const double amplitude = dbToAmplitude (levelDb);

    for (size_t i = 0; i < v.size(); ++i)
        v[i] = (float) (amplitude * std::sin (2.0 * pi * frequency * (double) i / sampleRate));

    return v;
}

/** The two channels are summed, each weighing one, which is what BS.1770
    says a stereo programme is.

    Nothing above here could tell that apart from measuring one channel and
    counting it twice, because every case feeds the same signal to both. A
    meter that read the left channel twice would report a mix with its content
    on the right as silent, and the level of anything panned off centre
    wrongly. */
static void bothChannelsAreMeasuredAndSummed()
{
    std::printf ("both channels are measured, and summed with equal weight\n");

    const double sampleRate = 48000.0;
    const auto tone = sine (1000.0, -23.0, 5.0, sampleRate);
    const std::vector<float> silent (tone.size(), 0.0f);

    const auto readingOf = [&] (const std::vector<float>& l, const std::vector<float>& r)
    {
        LoudnessMeter meter;
        meter.prepare (sampleRate);
        feedChannels (meter, l, r);
        return meter.getIntegratedLufs();
    };

    const double leftOnly  = readingOf (tone, silent);
    const double rightOnly = readingOf (silent, tone);
    const double bothSides = readingOf (tone, tone);

    std::printf ("    left only %+7.3f   right only %+7.3f   both %+7.3f LUFS\n",
                 leftOnly, rightOnly, bothSides);

    check (std::abs (leftOnly - rightOnly) < 0.01,
           "a tone in the left channel read " + std::to_string (leftOnly)
               + " and the same tone in the right read " + std::to_string (rightOnly));

    // Two equal channels carry twice the energy of one, which is 3.0103 dB.
    check (std::abs ((bothSides - leftOnly) - 3.0103) < 0.02,
           "the same tone in both channels read " + std::to_string (bothSides - leftOnly)
               + " dB above one channel, where summing two equal channels is 3.0103");

    // A different tone in each channel, so nothing can pass by reading one of
    // them twice, and the two channels' filter states have to be separate.
    // The standard's sum is over channel energies, so the reading follows from
    // the two single channel readings and nothing else.
    const auto onLeft  = sine (1000.0, -20.0, 5.0, sampleRate);
    const auto onRight = sine (3000.0, -26.0, 5.0, sampleRate);
    const std::vector<float> quiet (onLeft.size(), 0.0f);

    const double alone1k = readingOf (onLeft, quiet);
    const double alone3k = readingOf (quiet, onRight);
    const double together = readingOf (onLeft, onRight);

    const double powerSum = -0.691 + 10.0 * std::log10 (std::pow (10.0, (alone1k + 0.691) / 10.0)
                                                      + std::pow (10.0, (alone3k + 0.691) / 10.0));

    std::printf ("    1 kHz left %+7.3f   3 kHz right %+7.3f   together %+7.3f   their power sum %+7.3f\n",
                 alone1k, alone3k, together, powerSum);

    check (std::abs (together - powerSum) < 0.01,
           "a different tone in each channel read " + std::to_string (together)
               + " where the sum of the two channel energies is " + std::to_string (powerSum));

    // And the peak detector is per channel too. EBU case 19's signal, in the
    // right channel only.
    {
        std::vector<float> onlyRight ((size_t) sampleRate);
        for (size_t i = 0; i < onlyRight.size(); ++i)
            onlyRight[i] = (float) (1.41 * std::sin (2.0 * pi * 0.25 * (double) i + pi / 4.0));

        const std::vector<float> nothing (onlyRight.size(), 0.0f);

        LoudnessMeter meter;
        meter.prepare (sampleRate);
        feedChannels (meter, nothing, onlyRight);

        const double peak = 20.0 * std::log10 (meter.getTruePeak());
        std::printf ("    a +3 dBTP signal in the right channel alone reads %+6.3f dBTP\n", peak);
        check (peak > 2.5, "a peak present only in the right channel read "
                               + std::to_string (peak) + " dBTP");
    }
}

/** A mono call is one channel weighing one, and reads nothing it was not
    given.

    `process` takes a channel count and promises to measure at most the first
    two. The only thing standing between a mono bus and a read past the end of
    the caller's array is that promise, so it is asserted here with an array
    that has exactly one entry in it: reading the second would be out of
    bounds, which the sanitizers the suite runs under would stop on. */
static void aMonoCallIsOneChannelAndReadsNothingElse()
{
    std::printf ("a mono call measures one channel and reads no further\n");

    const double sampleRate = 48000.0;
    const auto tone = sine (1000.0, -23.0, 5.0, sampleRate);

    LoudnessMeter mono, stereo;
    mono.prepare (sampleRate);
    stereo.prepare (sampleRate);

    for (size_t pos = 0; pos < tone.size(); pos += 512)
    {
        const int n = (int) std::min ((size_t) 512, tone.size() - pos);
        const float* justTheOne[1] = { tone.data() + pos };
        mono.process (justTheOne, 1, n);
    }

    feedChannels (stereo, tone, tone);

    const double difference = stereo.getIntegratedLufs() - mono.getIntegratedLufs();
    std::printf ("    mono %+7.3f   the same signal in two channels %+7.3f   difference %+6.3f\n",
                 mono.getIntegratedLufs(), stereo.getIntegratedLufs(), difference);

    check (std::abs (difference - 3.0103) < 0.02,
           "a mono programme read " + std::to_string (difference)
               + " dB below the same signal in two channels, where one channel of two is 3.0103");
}

/** The momentary window is 400 ms, which nothing above here pins.

    Every case above measures a steady tone, and a steady tone reads the same
    through a window of any length. A tone that is on for a quarter of every
    400 ms does not: a correct window always holds exactly 100 ms of it and so
    reads 6.0206 dB below the tone's own level, and reads it steadily. A 300 ms
    window holds between none of it and all of it depending on where the window
    happens to sit, and a 500 ms window ripples for the same reason. So both
    the value and its steadiness are checked, at every hop rather than at the
    end. */
static void theMomentaryWindowIsFourHundredMilliseconds()
{
    std::printf ("the momentary window is 400 ms, not merely some length\n");

    const double sampleRate = 48000.0;
    const double levelDb = -23.0;
    const int hopSamples = 4800;                    // 100 ms

    LoudnessMeter meter;
    meter.prepare (sampleRate);

    double lowest = 1.0e9, highest = -1.0e9;
    double phase = 0.0;

    for (int hop = 0; hop < 48; ++hop)
    {
        std::vector<float> chunk ((size_t) hopSamples);
        const bool sounding = (hop % 4) == 0;

        for (size_t i = 0; i < chunk.size(); ++i)
        {
            chunk[i] = sounding ? (float) (dbToAmplitude (levelDb) * std::sin (phase)) : 0.0f;
            phase += 2.0 * pi * 1000.0 / sampleRate;
            if (phase > 2.0 * pi)
                phase -= 2.0 * pi;
        }

        feedChannels (meter, chunk, chunk);

        // Only once the window is past the start of the signal, and past the
        // first whole period of it.
        if (hop >= 8)
        {
            lowest  = std::min (lowest,  meter.getMomentaryLufs());
            highest = std::max (highest, meter.getMomentaryLufs());
        }
    }

    const double want = levelDb - 6.0206;           // a quarter of the energy
    std::printf ("    tone on 100 ms in every 400 ms: momentary stays between %+7.3f and %+7.3f, want %+7.3f\n",
                 lowest, highest, want);

    check (highest - lowest < 0.01,
           "the momentary reading moved by " + std::to_string (highest - lowest)
               + " LU on a signal whose period is exactly the window length");
    check (std::abs (lowest - want) < 0.05 && std::abs (highest - want) < 0.05,
           "a tone sounding a quarter of the time read between " + std::to_string (lowest)
               + " and " + std::to_string (highest) + ", where a quarter of the energy is "
               + std::to_string (want));
}

/** The short term window is three seconds, from both sides, and reports
    nothing until it has three seconds to report on.

    Case 9 above shows that a signal whose period is the window length reads
    correctly, which a window of 1 s or 4 s would also manage. What separates
    them is how long the reading takes to arrive after a step: 30 blocks at a
    100 ms hop span 3.3 s, so a step is fully in the window 3.3 s later and
    only a third of the way in after 1 s. */
static void theShortTermWindowIsThreeSeconds()
{
    std::printf ("the short term window is three seconds, and says nothing before it has them\n");

    const double sampleRate = 48000.0;

    const auto shortTermAfter = [sampleRate] (double quietSeconds, double loudSeconds)
    {
        LoudnessMeter meter;
        meter.prepare (sampleRate);
        const auto quiet = sine (1000.0, -43.0, quietSeconds, sampleRate);
        const auto loud  = sine (1000.0, -23.0, loudSeconds, sampleRate);
        feedChannels (meter, quiet, quiet);
        feedChannels (meter, loud, loud);
        return meter.getShortTermLufs();
    };

    // Before the window has filled there is no reading at all, rather than a
    // reading taken over whatever has arrived so far. The first one lands
    // 3.3 s in: 400 ms to fill the first block, then 29 hops of 100 ms.
    LoudnessMeter young;
    young.prepare (sampleRate);
    const auto brief = sine (1000.0, -23.0, 3.2, sampleRate);
    feedChannels (young, brief, brief);
    std::printf ("    after 3.2 s the short term reading is %+9.3f\n", young.getShortTermLufs());
    check (young.getShortTermLufs() < -70.0,
           "with only 3.2 s measured the short term reading was already "
               + std::to_string (young.getShortTermLufs()));

    const double afterOne  = shortTermAfter (5.0, 1.0);
    const double afterFull = shortTermAfter (5.0, 3.3);

    std::printf ("    1.0 s after a 20 dB step %+8.3f, 3.3 s after %+8.3f, target -23.0\n",
                 afterOne, afterFull);

    check (afterOne < -27.0,
           "one second after a 20 dB step the three second reading had already reached "
               + std::to_string (afterOne) + ", which is a window shorter than three seconds");
    check (std::abs (afterFull - (-23.0)) < 0.05,
           "3.3 s after a 20 dB step the three second reading was " + std::to_string (afterFull)
               + " rather than the new level");
}

/** The absolute gate is at -70 LUFS, and the histogram holds whatever it is
    given.

    Nothing above here ever drives a programme near the gate: the quietest is
    -36 dBFS, where the relative gate does all the work. Two things follow
    from that. The threshold itself was unpinned, and so was the clamp in
    `binFor`, because the relative gate only falls off the bottom of the
    histogram when the programme is quiet enough for the mean minus 10 LU to
    sit below -70. A programme at the top has the same shape of problem. */
static void theAbsoluteGateIsAtSeventyLufs()
{
    std::printf ("the absolute gate is at -70 LUFS, at both ends of the histogram\n");

    const double sampleRate = 48000.0;

    const auto integratedOf = [sampleRate] (double levelDb)
    {
        LoudnessMeter meter;
        meter.prepare (sampleRate);
        const auto tone = sine (1000.0, levelDb, 8.0, sampleRate);
        feedChannels (meter, tone, tone);
        return meter.getIntegratedLufs();
    };

    // A 1 kHz tone in both channels reads its own peak level in LUFS, because
    // the K-weighting's gain at 1 kHz is exactly what the -0.691 offset
    // cancels and two equal channels make up the 3 dB a sine's mean square
    // loses. So a -69.5 dBFS tone is a -69.5 LUFS programme.
    const double justAbove = integratedOf (-69.5);
    const double justBelow = integratedOf (-70.5);

    std::printf ("    a -69.5 LUFS programme reads %+9.3f, a -70.5 LUFS programme reads %+9.3f\n",
                 justAbove, justBelow);

    check (std::abs (justAbove - (-69.5)) < 0.1,
           "a programme just above the absolute gate read " + std::to_string (justAbove));
    check (justBelow < -150.0,
           "a programme below the absolute gate reported " + std::to_string (justBelow)
               + " rather than no reading at all");

    // And the other end. The histogram stops at +30 LUFS, which is thirty
    // decibels past anything a converter could pass, so it is only ever
    // reached by a mistake somewhere upstream. What it must not do is read the
    // loudest possible programme as the quietest possible number, which is
    // what tells somebody their master is silent when it is deafening.
    for (double levelDb : { 31.0, 45.0 })
    {
        const double got = integratedOf (levelDb);
        std::printf ("    a %+.0f LUFS programme reads %+9.3f\n", levelDb, got);
        check (std::abs (got - levelDb) < 0.1,
               "a programme at " + std::to_string ((int) levelDb) + " LUFS read "
                   + std::to_string (got));
    }
}

/** The loudness range's relative gate is 20 LU, and its absolute gate is
    doing something.

    The cases above only ever gate out passages far enough down that a gate
    anywhere between 10 and 30 LU would have removed them, so the threshold
    itself was never pinned. It matters in the ordinary direction: a chorus
    15 LU above a verse is a dynamic record, and a gate at 10 LU would throw
    the verse away and report the record as flat. */
static void theLoudnessRangeGatesAtTwentyLu()
{
    std::printf ("EBU Tech 3342: the relative gate is 20 LU below the mean\n");

    const double sampleRate = 48000.0;

    const auto rangeOf = [sampleRate] (double mainDb, double quietDb)
    {
        LoudnessMeter meter;
        meter.prepare (sampleRate);
        const auto loud  = sine (1000.0, mainDb,  15.0, sampleRate);
        const auto quiet = sine (1000.0, quietDb,  9.0, sampleRate);
        feedChannels (meter, loud, loud);
        feedChannels (meter, quiet, quiet);
        return meter.getLoudnessRange();
    };

    // 15 LU down is music and has to stay in.
    const double fifteenDown = rangeOf (-20.0, -35.0);
    std::printf ("    a passage 15 LU down      %6.3f LU\n", fifteenDown);
    check (std::abs (fifteenDown - 15.0) < 1.0,
           "a passage 15 LU below the main level, which is inside a 20 LU gate, gave a range of "
               + std::to_string (fifteenDown));

    // 25 LU down is below the gate and has to go.
    const double twentyFiveDown = rangeOf (-20.0, -45.0);
    std::printf ("    a passage 25 LU down      %6.3f LU\n", twentyFiveDown);
    check (twentyFiveDown < 8.0,
           "a passage 25 LU below the main level, which is outside a 20 LU gate, left a range of "
               + std::to_string (twentyFiveDown));

    // The absolute gate has one window in which it is the only thing working:
    // a programme quiet enough that 20 LU below its own mean falls under
    // -70 LUFS. Below that the relative gate cannot reach, so a passage
    // between the two is removed by the absolute gate or not at all.
    const double underSeventy = rangeOf (-52.0, -71.0);
    std::printf ("    a -71 LUFS passage in a -52 LUFS programme %6.3f LU\n", underSeventy);
    check (underSeventy < 8.0,
           "a passage below -70 LUFS, which only the absolute gate can remove, left a range of "
               + std::to_string (underSeventy));
}

/** The peak reading is of the unweighted signal, which means it has an upper
    bound as well as a lower one.

    `truePeakIsWiredUp` above only ever asks for more than 2.5 dBTP, so a
    meter measuring the peak after K-weighting passes it comfortably: the
    weighting is about +4 dB at the 12 kHz that case uses, so the reading goes
    further up rather than wrong. It would be wrong everywhere else, and in
    the direction that matters, telling somebody a master is over when it is
    not. A plain sine's true peak is its own amplitude, so this asks for that
    number from both sides, at a frequency where the weighting lifts and one
    where it cuts. */
static void thePeakReadingIsUnweighted()
{
    std::printf ("the peak is of the waveform, not of the weighted signal\n");

    const double sampleRate = 48000.0;

    for (double frequency : { 50.0, 400.0, 1000.0 })
    {
        LoudnessMeter meter;
        meter.prepare (sampleRate);
        const auto tone = sine (frequency, -6.0, 2.0, sampleRate);
        feedChannels (meter, tone, tone);

        const double got = 20.0 * std::log10 (meter.getTruePeak());
        std::printf ("    %6.0f Hz at -6 dBFS reads %+7.4f dBTP\n", frequency, got);

        // The interpolator's own ripple is a few hundredths of a decibel. The
        // K-weighting is +0.69 dB at 1 kHz and -1.4 dB at 50, so either is
        // well outside this.
        check (std::abs (got - (-6.0)) < 0.1,
               "a -6 dBFS sine at " + std::to_string ((int) frequency) + " Hz read "
                   + std::to_string (got) + " dBTP");
    }
}

/** Reset leaves the meter indistinguishable from a new one.

    This is a live path: the mix report's reset button sets a flag that the
    device callback acts on, so a figure the owner is reading was computed by
    a meter that has been reset. Nothing above here called it. The comment on
    it says it may only clear and never resize, because it runs on the audio
    thread, and that half is checked by `nothingAllocatesWhileMeasuring`. */
static void resetLeavesNothingBehind()
{
    std::printf ("reset leaves the meter indistinguishable from a new one\n");

    const double sampleRate = 48000.0;

    // Loud, so that anything kept would be obvious against what follows, and
    // long enough to fill every histogram, the ring and both filters.
    const auto before = sine (1000.0, -2.0, 8.0, sampleRate);

    // Two levels, so the short term histogram and the range have something in
    // them, at a different frequency so the filter states differ too.
    const auto afterLoud  = sine (440.0, -30.0, 7.0, sampleRate);
    const auto afterQuiet = sine (440.0, -40.0, 7.0, sampleRate);

    LoudnessMeter used, brandNew;
    used.prepare (sampleRate);
    brandNew.prepare (sampleRate);

    feedChannels (used, before, before);
    used.reset();

    for (auto* meter : { &used, &brandNew })
    {
        feedChannels (*meter, afterLoud, afterLoud);
        feedChannels (*meter, afterQuiet, afterQuiet);
    }

    std::printf ("    reset  M %+8.3f S %+8.3f I %+8.3f LRA %6.3f peak %.6f\n",
                 used.getMomentaryLufs(), used.getShortTermLufs(), used.getIntegratedLufs(),
                 used.getLoudnessRange(), (double) used.getTruePeak());
    std::printf ("    new    M %+8.3f S %+8.3f I %+8.3f LRA %6.3f peak %.6f\n",
                 brandNew.getMomentaryLufs(), brandNew.getShortTermLufs(), brandNew.getIntegratedLufs(),
                 brandNew.getLoudnessRange(), (double) brandNew.getTruePeak());

    const auto same = [] (const char* which, double a, double b)
    {
        // The same arithmetic in the same order, so this is equality rather
        // than a tolerance. Anything the reset kept moves it.
        if (std::abs (a - b) > 1.0e-9)
            fail (std::string ("after a reset the ") + which + " reading was " + std::to_string (a)
                  + " where a new meter measuring the same programme reads " + std::to_string (b));
    };

    same ("momentary",  used.getMomentaryLufs(),  brandNew.getMomentaryLufs());
    same ("short term", used.getShortTermLufs(),  brandNew.getShortTermLufs());
    same ("integrated", used.getIntegratedLufs(), brandNew.getIntegratedLufs());
    same ("range",      used.getLoudnessRange(),  brandNew.getLoudnessRange());
    same ("peak",       (double) used.getTruePeak(), (double) brandNew.getTruePeak());

    // And immediately after a reset there is nothing to report rather than
    // the old programme's figure. One second in, the three second reading has
    // nothing to say, and the peak is whatever the second holds and not the
    // loud passage before it.
    LoudnessMeter justReset;
    justReset.prepare (sampleRate);
    feedChannels (justReset, before, before);
    justReset.reset();

    const auto oneSecond = sine (1000.0, -30.0, 1.0, sampleRate);
    feedChannels (justReset, oneSecond, oneSecond);

    std::printf ("    one second after a reset: S %+9.3f  I %+8.3f  peak %.6f\n",
                 justReset.getShortTermLufs(), justReset.getIntegratedLufs(),
                 (double) justReset.getTruePeak());

    check (justReset.getShortTermLufs() < -70.0,
           "one second after a reset the three second reading was "
               + std::to_string (justReset.getShortTermLufs()));
    check (std::abs (justReset.getIntegratedLufs() - (-30.0)) < 0.1,
           "one second after a reset the integrated reading was "
               + std::to_string (justReset.getIntegratedLufs())
               + " rather than the level of what has been measured since");
    check (justReset.getTruePeak() < 0.05f,
           "one second after a reset the peak was still "
               + std::to_string (justReset.getTruePeak()));
}

/** Preparing again at another rate does not leave the window indexing past
    its own end.

    The owner changing the device's sample rate in Preferences runs
    `audioDeviceAboutToStart`, which prepares the meter again. `prepare`
    resizes the 400 ms window, so if it did not also reset the position into
    it, a change from 96 kHz down to 44.1 kHz would leave that position
    beyond the shorter buffer and the next sample would be written off the end
    of it. Nothing above here prepared a meter twice. */
static void preparingAgainAtAnotherRateIsSafe()
{
    std::printf ("preparing again at a lower rate does not write off the end of the window\n");

    const auto high = sine (1000.0, -10.0, 3.0, 96000.0);
    const auto low  = sine (1000.0, -23.0, 8.0, 44100.0);

    LoudnessMeter moved, brandNew;

    moved.prepare (96000.0);
    feedChannels (moved, high, high);
    moved.prepare (44100.0);
    feedChannels (moved, low, low);

    brandNew.prepare (44100.0);
    feedChannels (brandNew, low, low);

    std::printf ("    after a change of rate I %+8.4f, a new meter reads %+8.4f\n",
                 moved.getIntegratedLufs(), brandNew.getIntegratedLufs());

    if (std::abs (moved.getIntegratedLufs() - brandNew.getIntegratedLufs()) > 1.0e-9)
        fail ("a meter prepared again at 44100 Hz read "
              + std::to_string (moved.getIntegratedLufs())
              + " where a new one at that rate reads "
              + std::to_string (brandNew.getIntegratedLufs()));
}

/** Measuring allocates nothing, which is what lets it run in the device
    callback at all.

    The header says so at the top and nothing checked it. `reset` is included
    because it is reachable from the audio thread, which is the reason it is
    separate from `prepare` in the first place. */
static void nothingAllocatesWhileMeasuring()
{
    std::printf ("measuring and resetting allocate nothing\n");

    LoudnessMeter meter;
    meter.prepare (48000.0);                 // allocates, on the message thread

    const auto tone = sine (1000.0, -23.0, 1.0, 48000.0);

    allocationsWatched = true;
    allocations = 0;

    for (size_t pos = 0; pos < tone.size(); pos += 512)
    {
        const int n = (int) std::min ((size_t) 512, tone.size() - pos);
        const float* channels[2] = { tone.data() + pos, tone.data() + pos };
        meter.process (channels, 2, n);
    }

    meter.reset();

    const float* channels[2] = { tone.data(), tone.data() };
    meter.process (channels, 2, 512);

    const int counted = allocations;
    allocationsWatched = false;

    std::printf ("    %d allocations across a second of metering, a reset and a block after it\n", counted);

    check (counted == 0,
           "measuring allocated " + std::to_string (counted) + " times on the audio thread");
}

/** Before prepare there is nothing to measure and nothing to report.

    The device callback can reach the meter before `audioDeviceAboutToStart`
    has given it a sample rate, and a 400 ms window of zero samples is the
    one case where every index into it is out of range. */
static void measuringBeforePrepareIsSafe()
{
    std::printf ("a block arriving before prepare is ignored rather than indexed\n");

    LoudnessMeter meter;
    const auto tone = sine (1000.0, -23.0, 1.0, 48000.0);

    meter.reset();
    feedChannels (meter, tone, tone);

    check (meter.getMomentaryLufs() < -150.0,
           "an unprepared meter reported " + std::to_string (meter.getMomentaryLufs()));
    check (meter.getTruePeak() == 0.0f, "an unprepared meter reported a peak");
}

int main()
{
    std::printf ("loudness meter\n");

    kWeightingMatchesTheStandard();
    theWeightingCurveIsTheSameAtEveryRate();
    gainAtOneKilohertzIsWhatTheOffsetAssumes();
    ebuComplianceCases();
    ebuCase9();
    bothChannelsAreMeasuredAndSummed();
    aMonoCallIsOneChannelAndReadsNothingElse();
    theMomentaryWindowIsFourHundredMilliseconds();
    theShortTermWindowIsThreeSeconds();
    theAbsoluteGateIsAtSeventyLufs();
    measurementDoesNotDependOnSampleRate();
    blockSizeDoesNotChangeTheReading();
    silenceDoesNotReadAsLoud();
    truePeakIsWiredUp();
    thePeakReadingIsUnweighted();
    loudnessRangeFollowsTech3342();
    theLoudnessRangeGatesAtTwentyLu();
    theReadingsDoNotDriftWithLength();
    resetLeavesNothingBehind();
    preparingAgainAtAnotherRateIsSafe();
    measuringBeforePrepareIsSafe();
    nothingAllocatesWhileMeasuring();

    if (failures > 0)
    {
        std::printf ("\n%d loudness check(s) failed\n", failures);
        return 1;
    }

    std::printf ("all loudness checks passed\n");
    return 0;
}
