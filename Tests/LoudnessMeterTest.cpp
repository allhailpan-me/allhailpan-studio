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
// Sources:
//   ITU-R BS.1770-4, Tables 1 and 2 (K-weighting coefficients at 48 kHz)
//   EBU Tech 3341, Table 1, cases 1, 2, 3 and 9
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

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

    LoudnessMeter meter;
    meter.prepare (sampleRate);
    feed (meter, { { frequency, 20.0 * std::log10 (amplitude), 2.0 } }, sampleRate);

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

        // Tight enough to catch the quarter decibel error that a general
        // purpose shelf design produces, loose enough for the ripple of
        // measuring a finite tone through a 400 ms window.
        if (std::abs (got - want) > 0.03)
            fail ("at " + std::to_string ((int) f) + " Hz the meter weights by "
                  + std::to_string (got) + " dB, the standard says "
                  + std::to_string (want));
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

int main()
{
    std::printf ("loudness meter\n");

    kWeightingMatchesTheStandard();
    gainAtOneKilohertzIsWhatTheOffsetAssumes();
    ebuComplianceCases();
    ebuCase9();
    measurementDoesNotDependOnSampleRate();
    blockSizeDoesNotChangeTheReading();
    silenceDoesNotReadAsLoud();
    truePeakIsWiredUp();

    if (failures > 0)
    {
        std::printf ("\n%d loudness check(s) failed\n", failures);
        return 1;
    }

    std::printf ("all loudness checks passed\n");
    return 0;
}
