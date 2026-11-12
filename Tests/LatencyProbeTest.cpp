// ---------------------------------------------------------------------------
// Checks that the loopback measurement reports the round trip it was given,
// and refuses to report one when it should.
//
// Why this needs a real test rather than a few hand picked cases: the number
// it produces goes straight into where every recorded take lands. A
// measurement that is wrong by ten milliseconds does not error, does not
// crackle and does not move a meter. It moves every take by ten milliseconds,
// in the direction of a player who cannot keep time, and the amount depends
// on the driver, so two sessions disagree and neither looks broken. That is
// the same shape of fault RecordAlign.h exists to fix, with the added
// pleasantness that this one would be introduced by the thing built to fix it.
//
// What makes this testable to a fraction of a sample with no hardware: the
// probe is a sum of thirteen sinusoids with linear phase, so a copy of it
// delayed by any fractional number of samples can be written down exactly
// rather than interpolated. Probe::idealSample is that closed form, and the
// first check here is that the streamed probe agrees with it, because
// everything after it is built on that agreement.
//
// The properties, in the order they are asserted:
//
//   1. The frequency table carries the bit ladder it is supposed to. This is
//      the one thing in the file that cannot be seen by looking: twelve ratios
//      that have to be an odd number over a power of two, and a single wrong
//      digit silently costs one bit of range.
//   2. The streamed probe is the closed form.
//   3. A known delay comes back, integer and fractional, from nothing to most
//      of the range, and over thousands of random ones.
//   4. A reversed cable still comes back, and says it was reversed.
//   5. Nothing patched is refused, and the published threshold is where it
//      says it is.
//   6. A clipping loopback is refused before its residual is believed.
//   7. A delay past the ceiling is refused, and the silent wrap past the range
//      is pinned so nobody believes the range is unlimited.
//   8. Noise: right where it can be, refused rather than wrong where it
//      cannot.
//   9. The trim: the total offset is the same whatever the driver claimed.
//
// LatencyProbe.h has no JUCE in it, so this builds and runs on its own with
// sanitizers in a few seconds. See the header for where the algorithm came
// from and for the three faults in the references it does not copy.
// ---------------------------------------------------------------------------

#include "LatencyProbe.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <limits>
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

static void checkClose (double a, double b, double tol, const std::string& what)
{
    if (! (std::fabs (a - b) <= tol))
        fail (what + "  (" + std::to_string (a) + " vs " + std::to_string (b)
                    + ", tolerance " + std::to_string (tol) + ")");
}

static const char* name (LatencyProbe::Status s)
{
    switch (s)
    {
        case LatencyProbe::Status::ok:          return "ok";
        case LatencyProbe::Status::noSignal:    return "noSignal";
        case LatencyProbe::Status::tooHot:      return "tooHot";
        case LatencyProbe::Status::unreliable:  return "unreliable";
        case LatencyProbe::Status::implausible: return "implausible";
    }

    return "?";
}

static void checkStatus (LatencyProbe::Status got, LatencyProbe::Status want,
                         const std::string& what)
{
    if (got != want)
        fail (what + "  (" + name (got) + ", wanted " + name (want) + ")");
}

//==============================================================================
/** How the loopback is simulated: whatever was played comes back `delay`
    samples later, scaled, possibly inverted, possibly with noise and a DC
    offset on it.

    The returned signal is written from the closed form rather than from a
    delay line over the probe's own output, which is the whole reason this test
    can talk about fractions of a sample. A delay line would need an
    interpolator, and then the interpolator's own error would be mixed into
    every reading and there would be nothing left to measure the probe
    against.
*/
struct Loop
{
    double delay      = 0.0;
    double gain       = 1.0;
    double noiseRms   = 0.0;
    double dcOffset   = 0.0;
    bool   inverted   = false;
    bool   openInput  = true;
};

/** The sample rate the bulk of the randomised work runs at, and the reason it
    is not one the studio uses.

    The algorithm is scale free in exactly the way that makes this safe. Its
    thirteen frequencies are proportional to the sample rate, its averaging is
    `200 / fs` applied every sixteen samples, which is 80 ms a stage at any
    rate, and the answer it produces is a count of samples. So a reading taken
    after a given number of seconds is the same reading at any rate, and
    running the randomised trials at a low one buys thousands of them for the
    cost of hundreds under the sanitizers. The real rates are covered
    separately and exactly, in the sweep in check 3 and in the convergence
    check, which are the two places the rate could actually matter.

    One place it is NOT safe, found by this test failing: noise. The probe
    scales with the rate and added noise does not, so what the demodulator
    averages over is 0.08 * fs samples of it, which is six times fewer at this
    rate than at 48 kHz. A signal to noise figure therefore means nothing
    without a rate attached, and the noise checks in check 8 run at a real one.
*/
static constexpr double bulkRate = 8000.0;

static LatencyProbe::Reading measure (const Loop& loop, double sampleRate, double seconds,
                                      unsigned seed = 1u, int blockSize = 256)
{
    LatencyProbe::Probe probe;
    probe.reset (sampleRate);

    std::vector<float> in  ((size_t) blockSize, 0.0f);
    std::vector<float> out ((size_t) blockSize, 0.0f);

    std::mt19937 rng (seed);
    std::normal_distribution<double> noise (0.0, loop.noiseRms > 0.0 ? loop.noiseRms : 1.0);

    const long long total = (long long) (seconds * sampleRate);

    for (long long n = 0; n < total; n += blockSize)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            double v = loop.gain
                       * LatencyProbe::Probe::idealSample ((double) (n + i) - loop.delay);

            if (loop.inverted)
                v = -v;

            if (loop.noiseRms > 0.0)
                v += noise (rng);

            v += loop.dcOffset;

            in[(size_t) i] = (float) v;
        }

        probe.process (loop.openInput ? in.data() : nullptr, out.data(), blockSize);
    }

    return probe.read();
}

//==============================================================================
int main()
{
    using namespace LatencyProbe;

    std::printf ("LatencyProbe\n");

    //==========================================================================
    // 1. The frequency table carries the bit ladder.
    //
    // Each of the twelve has to be an odd number over a power of two of the
    // fundamental, with the power being exactly its own position in the
    // ladder. That is what makes a change of 2^i cycles in the delay flip bit
    // i and nothing else. Asserted in integers so it is exact: a ratio that is
    // nearly right is a bit that is nearly resolved, which is a reading that is
    // wrong by a multiple of sixteen samples.
    {
        check (toneIncrement[0] == 4096, "the fundamental is fs/16");
        check (samplesPerFundamentalCycle == 16, "a fundamental cycle is sixteen samples");
        check (unambiguousSamples == 65536, "the range is twelve bits of sixteen samples");

        for (int i = 1; i < numTones; ++i)
        {
            // f_i / f_0 = numerator / 2^i, so numerator = f_i * 2^i / f_0.
            const long long denominator = 1LL << i;
            const long long numerator   = (long long) toneIncrement[i] * denominator
                                          / toneIncrement[0];

            check (numerator * toneIncrement[0] == (long long) toneIncrement[i] * denominator,
                   "tone " + std::to_string (i) + " is an exact ratio of the fundamental");
            check ((numerator % 2) != 0,
                   "tone " + std::to_string (i) + " has an odd numerator, so it flips its bit");
        }

        // And every tone is inside the mid band the design keeps them in,
        // which is what stops the probe from being measured through the
        // interface's own high or low end.
        for (int i = 0; i < numTones; ++i)
        {
            const double ratio = (double) toneIncrement[i] / (double) phaseCycle;
            check (ratio <= 1.0 / 16.0 + 1.0e-12 && ratio >= 1.0 / 64.0,
                   "tone " + std::to_string (i) + " is between fs/64 and fs/16");
        }
    }

    //==========================================================================
    // 2. The streamed probe is the closed form.
    //
    // Everything below delays idealSample and asks the probe to recover the
    // delay, so if process generated anything other than idealSample the whole
    // file would be measuring one mistake against itself and agreeing.
    {
        Probe probe;
        probe.reset (48000.0);

        const int block = 64;
        std::vector<float> in  ((size_t) block, 0.0f);
        std::vector<float> out ((size_t) block, 0.0f);

        double worst = 0.0;

        for (long long n = 0; n < 4096; n += block)
        {
            probe.process (in.data(), out.data(), block);

            for (int i = 0; i < block; ++i)
                worst = std::max (worst, std::fabs ((double) out[(size_t) i]
                                                    - Probe::idealSample ((double) (n + i))));
        }

        checkClose (worst, 0.0, 2.0e-6, "the streamed probe matches its closed form");

        // The peak the probe itself puts out, which has to stay well clear of
        // full scale: a probe that clipped on the way out would be measured
        // through its own distortion.
        double peak = 0.0;

        for (int n = 0; n < 40000; ++n)
            peak = std::max (peak, std::fabs (Probe::idealSample ((double) n)));

        check (peak < 0.4, "the probe's own peak leaves headroom");
        check (peak > 0.2, "the probe is loud enough to measure");
    }

    //==========================================================================
    // 3. A known delay comes back.
    {
        // Hand picked first, including the ones most likely to be wrong: zero,
        // a fraction, exactly one fundamental cycle, a bit boundary, and most
        // of the range.
        for (double d : { 0.0, 1.0, 7.25, 15.5, 16.0, 63.5, 128.0, 256.37,
                          1024.0, 4096.75, 16384.0, 32000.5 })
        {
            const auto r = measure ({ d }, 48000.0, 1.0);

            checkStatus (r.status, Status::ok, "delay " + std::to_string (d) + " resolves");
            checkClose (r.roundTripSamples, d, 1.0e-3,
                        "delay " + std::to_string (d) + " comes back");
            check (! r.inverted, "delay " + std::to_string (d) + " is not reported inverted");
        }

        // Every sample rate the studio runs at. The frequencies are
        // proportional to the rate, so this is the check that the scaling is
        // right rather than accidentally correct at 48 kHz.
        for (double sr : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
        {
            const auto r = measure ({ 311.5 }, sr, 1.0);

            checkStatus (r.status, Status::ok, "resolves at " + std::to_string (sr));
            checkClose (r.roundTripSamples, 311.5, 1.0e-3, "measures at " + std::to_string (sr));
        }

        // Randomised: the real check. Any delay, any level the loop might come
        // back at, any block size the device might hand over, a little DC on
        // the input because an interface's input stage has some.
        std::mt19937 rng (20261112u);
        std::uniform_real_distribution<double> delay (0.0, 4000.0);
        std::uniform_real_distribution<double> gain (0.05, 1.2);
        std::uniform_real_distribution<double> dc (-0.02, 0.02);
        std::uniform_int_distribution<int> blocks (0, 4);
        const int blockSizes[] = { 16, 32, 64, 256, 1024 };

        double worst = 0.0;
        int    refused = 0;

        for (int trial = 0; trial < 500; ++trial)
        {
            Loop loop;
            loop.delay    = delay (rng);
            loop.gain     = gain (rng);
            loop.dcOffset = dc (rng);

            const auto r = measure (loop, bulkRate, 0.8, (unsigned) (trial + 1),
                                    blockSizes[blocks (rng)]);

            if (r.status != Status::ok)
            {
                ++refused;
                continue;
            }

            worst = std::max (worst, std::fabs (r.roundTripSamples - loop.delay));
        }

        check (refused == 0, "no ordinary loopback is refused  ("
                             + std::to_string (refused) + " of 500 were)");
        checkClose (worst, 0.0, 0.01, "500 random loopbacks all come back within a hundredth"
                                      " of a sample");

        // A level the probe's own documented threshold says must work, and one
        // it says must not. These two pin the published constant together with
        // the demodulator's gain: get either wrong and one of them moves.
        checkStatus (measure ({ 480.0, 0.001 }, bulkRate, 2.0).status, Status::ok,
                     "a loop gain of -60 dB still resolves");
        checkStatus (measure ({ 480.0, 0.0001 }, bulkRate, 2.0).status, Status::noSignal,
                     "a loop gain of -80 dB is below the threshold");
    }

    //==========================================================================
    // 4. A reversed cable.
    //
    // Inverting the loop puts every tone exactly half a cycle out, which is
    // the worst residual there is, so this is the one failure the algorithm can
    // correct rather than report. It has to come back right AND say it was
    // reversed: silently correcting it would leave somebody with a cable they
    // do not know is wired backwards, which will bite them somewhere else.
    {
        for (double d : { 0.0, 97.5, 1024.0 })
        {
            Loop loop;
            loop.delay    = d;
            loop.inverted = true;

            const auto r = measure (loop, bulkRate, 1.0);

            checkStatus (r.status, Status::ok, "an inverted loop resolves");
            checkClose (r.roundTripSamples, d, 1.0e-3, "an inverted loop measures correctly");
            check (r.inverted, "an inverted loop is reported as inverted");
        }
    }

    //==========================================================================
    // 5. Nothing patched.
    {
        checkStatus (measure ({ 0.0, 0.0 }, bulkRate, 1.0).status, Status::noSignal,
                     "silence on the input is no signal");

        Loop closed;
        closed.openInput = false;
        checkStatus (measure (closed, bulkRate, 1.0).status, Status::noSignal,
                     "a device with no input open is no signal");

        // The probe still has to be written when there is no input, or the
        // absence shows up as silence at both ends and somebody goes looking at
        // their speakers.
        {
            Probe probe;
            probe.reset (48000.0);

            std::vector<float> out (128, -7.0f);
            probe.process (nullptr, out.data(), 128);

            double peak = 0.0;

            for (auto v : out)
                peak = std::max (peak, std::fabs ((double) v));

            check (peak > 0.05, "the probe is played even when nothing is coming back");
        }

        // A sample rate of nothing: a device between being closed and opened.
        // It must not divide by it, and it must still leave the caller's buffer
        // in a state that can be summed into a mix. See LatencyCalibration.h on
        // why an unwritten buffer is not silence.
        {
            Probe probe;
            probe.reset (0.0);

            std::vector<float> in (64, 0.3f);
            std::vector<float> out (64, 13.0f);
            probe.process (in.data(), out.data(), 64);

            bool clear = true;

            for (auto v : out)
                clear = clear && (v == 0.0f);

            check (clear, "a probe with no sample rate clears its output rather than leaving it");
            checkStatus (probe.read().status, Status::noSignal,
                         "a probe with no sample rate reports no signal");
        }

        // Degenerate block sizes and a null output, which a backend can hand
        // over on the block it is shutting down.
        {
            Probe probe;
            probe.reset (48000.0);

            std::vector<float> in (8, 0.1f);
            probe.process (in.data(), nullptr, 8);
            probe.process (nullptr, nullptr, 0);
            probe.process (in.data(), nullptr, -4);

            check (true, "degenerate blocks do not fall over");
        }
    }

    //==========================================================================
    // 6. A clipping loopback.
    //
    // Checked before the residual is believed, because a loopback that is
    // clipping is being measured through a nonlinearity: the tones are no
    // longer the only thing in the band and the residual it produces means
    // nothing. Ardour tests it in this order too.
    {
        Loop hot;
        hot.delay = 512.0;
        hot.gain  = 4.0;                     // the probe peaks at 0.32, so this clips

        const auto r = measure (hot, bulkRate, 1.0);
        checkStatus (r.status, Status::tooHot, "a loopback above -3 dBFS is refused");
        check (r.inputPeak > peakCeiling, "the peak is reported so the gain can be turned down");
        checkClose (r.roundTripSamples, 0.0, 0.0,
                    "a refused reading reports no round trip to write down");

        // Just under the ceiling must still work, or the check is costing
        // somebody a measurement they could have had.
        Loop warm;
        warm.delay = 512.0;
        warm.gain  = 2.0;                    // peak about 0.64, under 0.707

        const auto ok = measure (warm, bulkRate, 1.0);
        checkStatus (ok.status, Status::ok, "a hot but unclipped loopback still resolves");
        checkClose (ok.roundTripSamples, 512.0, 1.0e-3, "and measures correctly");

        // The peak tracker is a magnitude and it holds. Ardour's compares a
        // magnitude and stores the signed value, so one negative excursion
        // poisons every comparison after it, and then holds the last sample of
        // the block instead of the peak. A single negative spike early in the
        // measurement is exactly what that fault loses.
        {
            Probe probe;
            probe.reset (48000.0);

            std::vector<float> in (64, 0.0f);
            std::vector<float> out (64, 0.0f);

            in[3] = -0.9f;                   // the only loud thing, and negative
            probe.process (in.data(), out.data(), 64);

            std::fill (in.begin(), in.end(), 0.01f);

            for (int i = 0; i < 100; ++i)
                probe.process (in.data(), out.data(), 64);

            check (probe.read().inputPeak >= 0.9f,
                   "a negative peak is held as a magnitude, not lost");
        }

        // And a sample that is not a number must not become the peak, or one
        // bad block from a driver refuses every measurement afterwards with
        // nothing to say why.
        {
            Probe probe;
            probe.reset (48000.0);

            std::vector<float> in (32, 0.0f);
            std::vector<float> out (32, 0.0f);

            in[1] = std::nanf ("");
            probe.process (in.data(), out.data(), 32);

            check (! (probe.read().inputPeak > peakCeiling),
                   "a NaN on the input does not become the peak");
        }
    }

    //==========================================================================
    // 7. The ceiling, and the wrap.
    {
        const auto beyond = measure ({ (double) unambiguousSamples * 0.75 }, bulkRate, 1.5);
        checkStatus (beyond.status, Status::implausible,
                     "a delay past the ceiling is refused rather than reported");

        // Pinned rather than fixed, and the header says why: a wrapped answer
        // looks ordinary, so no check on the reading can catch it. What makes
        // it unreachable is that 65536 samples is 1.37 seconds at 48 kHz and
        // the probe does not pass through the mixer. This is here so that
        // nobody reads the range as unlimited, and so that anybody who raises
        // the ceiling finds out what is above it.
        const auto wrapped = measure ({ (double) unambiguousSamples + 4464.0 }, bulkRate, 1.5);

        if (wrapped.status == Status::ok)
            check (std::fabs (wrapped.roundTripSamples - ((double) unambiguousSamples + 4464.0))
                   > 1000.0,
                   "a delay past the range wraps, and is not quietly correct");

        // Nothing at all is the boundary worth checking at the other end: the
        // fundamental's phase is folded into plus or minus half a cycle, so a
        // delay of zero sits right on it and can land either side.
        const auto none = measure ({ 0.0 }, bulkRate, 1.0);
        checkStatus (none.status, Status::ok, "a delay of nothing resolves");
        check (none.roundTripSamples >= 0.0, "and is never reported as negative");
    }

    //==========================================================================
    // 8. Noise.
    //
    // The probe's own RMS is about 0.1435, so these are the signal to noise
    // ratios the header claims. What matters is not only that it is right where
    // it can be, but that it is refused rather than wrong where it cannot:
    // a confident wrong number here moves every take.
    {
        struct Case { double rms; bool mustResolve; };

        // The probe's own RMS is 0.1435, so 0.1 is about +3 dB of signal to
        // noise. The two loud cases are expected to be refused rather than
        // resolved, and for two different reasons worth keeping apart: at an
        // RMS of 0.3 the noise alone crosses the -3 dBFS peak ceiling, which
        // is the clipping check doing its job on an input that really is too
        // hot, and at 1.0 the demodulator cannot agree with itself. Either way
        // the requirement is the same and it is the one that matters: refused,
        // not wrong.
        for (Case c : { Case { 0.001, true }, Case { 0.01, true }, Case { 0.05, true },
                        Case { 0.1, true },   Case { 0.3, false }, Case { 1.0, false } })
        {
            Loop loop;
            loop.delay    = 480.0;
            loop.noiseRms = c.rms;

            const auto r = measure (loop, 48000.0, 1.0, 99u);

            if (c.mustResolve)
            {
                checkStatus (r.status, Status::ok,
                             "noise at " + std::to_string (c.rms) + " still resolves");
                checkClose (r.roundTripSamples, 480.0, 0.1,
                            "noise at " + std::to_string (c.rms) + " measures within a tenth"
                            " of a sample");
            }
            else
            {
                check (r.status != Status::ok,
                       "noise at " + std::to_string (c.rms) + " is refused rather than guessed");
            }
        }

        // The residual has to be a usable proxy for the error, or the refusal
        // threshold is guarding nothing: a reading accepted with a residual
        // under 0.2 must actually be close.
        std::mt19937 rng (7u);
        std::uniform_real_distribution<double> rms (0.0, 1.5);
        std::uniform_real_distribution<double> delay (0.0, 3000.0);

        for (int trial = 0; trial < 120; ++trial)
        {
            Loop loop;
            loop.delay    = delay (rng);
            loop.noiseRms = rms (rng);

            const auto r = measure (loop, 48000.0, 0.8, (unsigned) (trial + 500));

            if (r.status == Status::ok)
            {
                check (r.worstResidual <= unusableResidual,
                       "an accepted reading's residual is inside the threshold");
                checkClose (r.roundTripSamples, loop.delay, 1.0,
                            "an accepted reading is within a sample of the truth");
            }
        }
    }

    //==========================================================================
    // 9. Convergence.
    //
    // A wrong averaging coefficient does not make the measurement wrong, it
    // makes it slow or noisy, which is invisible unless something times it.
    {
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            const auto early = measure ({ 777.25 }, sr, 0.06);
            checkStatus (early.status, Status::ok, "settles within sixty milliseconds");
            checkClose (early.roundTripSamples, 777.25, 0.05,
                        "and is within a twentieth of a sample by then");

            const auto late = measure ({ 777.25 }, sr, 0.6);
            check (late.worstResidual <= early.worstResidual + 1.0e-9,
                   "the residual does not get worse with more averaging");
        }
    }

    //==========================================================================
    // 10. The trim: the total offset is the same whatever the driver claimed.
    //
    // This is the property the whole feature exists to produce, and the one a
    // studio gets wrong. The measurement is turned into a correction to the
    // driver's claim, so a device that reports honestly must get a trim of
    // nothing, a device that reports nothing must get a trim of the whole round
    // trip, and both must end up compensating by the same amount.
    {
        const double sr = 48000.0;

        auto figuresFor = [sr] (int in, int out, int buffer, int engine)
        {
            RecordAlign::Figures f;
            f.inputLatency  = in;
            f.outputLatency = out;
            f.bufferSize    = buffer;
            f.engineLatency = engine;
            f.sampleRate    = sr;
            f.trimMs        = 0.0;
            return f;
        };

        // An honest driver: it claimed 800 and the measurement agrees.
        //
        // Note what "honest" has to mean here, which this check got wrong at
        // first and is worth leaving written down. The claim the compensation
        // adds back is not the driver's two figures, it is
        // claimedRoundTripSamples, which is those two or the two buffer floor
        // when they are smaller. A driver reporting 240 each way on a 256
        // sample buffer is claiming 512 as far as the arithmetic is concerned,
        // so a measurement of 480 against it is a driver that overstated, and
        // the trim is rightly negative. The figures below clear the floor.
        {
            auto f = figuresFor (400, 400, 256, 0);
            const double trim = trimMsFor (800.0, f);

            check (trimFits (trim), "an honest driver's trim fits");
            checkClose (trim, 0.0, 1.0e-9, "an honest driver gets a trim of nothing");
        }

        // A driver that reported nothing, which is the ASIO failure
        // RecordAlign.h already names. Its claim falls back to two buffers, so
        // the trim has to make up the difference.
        {
            auto f = figuresFor (0, 0, 256, 0);
            const double trim = trimMsFor (1200.0, f);

            f.trimMs = trim;
            check (trimFits (trim), "a silent driver's trim fits");
            checkClose ((double) RecordAlign::captureOffsetSamples (f), 1200.0, 1.0,
                        "a silent driver ends up compensating by the whole measured round trip");
        }

        // The headline property, stated directly: two devices with the same
        // real round trip and completely different claims must compensate by
        // the same amount.
        {
            auto honest = figuresFor (240, 240, 256, 64);
            auto silent = figuresFor (0, 0, 256, 64);
            auto liar   = figuresFor (64, 64, 256, 64);

            honest.trimMs = trimMsFor (980.0, honest);
            silent.trimMs = trimMsFor (980.0, silent);
            liar  .trimMs = trimMsFor (980.0, liar);

            const int a = RecordAlign::captureOffsetSamples (honest);
            const int b = RecordAlign::captureOffsetSamples (silent);
            const int c = RecordAlign::captureOffsetSamples (liar);

            check (std::abs (a - b) <= 1 && std::abs (a - c) <= 1,
                   "the total offset does not depend on what the driver claimed  ("
                   + std::to_string (a) + ", " + std::to_string (b) + ", "
                   + std::to_string (c) + ")");
            checkClose ((double) a, 980.0 + 64.0, 1.0,
                        "and it is the measured round trip plus the engine's own latency");
        }

        // Randomised over every shape of claim, every buffer, every rate and
        // both directions of error.
        std::mt19937 rng (4242u);
        std::uniform_int_distribution<int> latency (0, 2400);
        std::uniform_int_distribution<int> buffer (16, 2048);
        std::uniform_int_distribution<int> engine (0, 4096);
        std::uniform_real_distribution<double> measured (0.0, 4000.0);
        const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
        std::uniform_int_distribution<int> rate (0, 4);

        int fitted = 0;

        for (int trial = 0; trial < 20000; ++trial)
        {
            RecordAlign::Figures f;
            f.inputLatency  = latency (rng);
            f.outputLatency = latency (rng);
            f.bufferSize    = buffer (rng);
            f.engineLatency = engine (rng);
            f.sampleRate    = rates[rate (rng)];

            const double m    = measured (rng);
            const double trim = trimMsFor (m, f);

            if (! trimFits (trim))
                continue;                   // refused, and check 11 covers that

            ++fitted;
            f.trimMs = trim;

            // One sample of slack, which is the round trip through
            // milliseconds and back. Nothing else is allowed.
            checkClose ((double) RecordAlign::captureOffsetSamples (f),
                        m + (double) f.engineLatency, 1.0,
                        "a measured round trip compensates by itself plus the engine");
        }

        check (fitted > 5000, "enough of the random cases were applicable to mean something  ("
                              + std::to_string (fitted) + ")");
    }

    //==========================================================================
    // 11. A trim too big to hold is refused, not clamped.
    //
    // A trim clamped to the end of its range is a studio that has applied part
    // of a correction and said it applied all of it. RecordAlign.h made the
    // same choice about the same control for the same reason.
    {
        RecordAlign::Figures f;
        f.inputLatency  = 0;
        f.outputLatency = 0;
        f.bufferSize    = 64;
        f.sampleRate    = 48000.0;

        // 48000 samples is a second, so a measurement of 10000 samples needs
        // 205 ms of correction, which the control cannot hold.
        const double trim = trimMsFor (10000.0, f);

        check (! trimFits (trim), "a correction beyond the control's range does not fit");
        check (trim > RecordAlign::maxTrimMs, "and it is reported as too large, not as zero");

        // The other direction: a driver claiming far more than is really there.
        f.inputLatency  = 4000;
        f.outputLatency = 4000;
        check (! trimFits (trimMsFor (100.0, f)),
               "a driver overstating by more than the range does not fit either");

        // And the degenerate inputs a settings file or a closed device can
        // produce.
        f.sampleRate = 0.0;
        checkClose (trimMsFor (500.0, f), 0.0, 0.0, "no sample rate gives no trim");

        f.sampleRate = 48000.0;
        checkClose (trimMsFor (std::nan (""), f), 0.0, 0.0, "a NaN measurement gives no trim");
        check (! trimFits (std::nan ("")), "a NaN trim does not fit");
        check (! trimFits (std::numeric_limits<double>::infinity()),
               "an infinite trim does not fit");
    }

    //==========================================================================
    // 12. The sign.
    //
    // Backwards here is the classic failure: it doubles the misalignment
    // instead of removing it, and it presents as "latency compensation is
    // broken" rather than as "the trim is the wrong way round".
    {
        RecordAlign::Figures f;
        f.inputLatency  = 240;
        f.outputLatency = 240;
        f.bufferSize    = 128;            // clear of the two buffer floor
        f.sampleRate    = 48000.0;

        const int before = RecordAlign::captureOffsetSamples (f);
        checkClose ((double) before, 480.0, 0.0, "the driver's claim is what it claimed");

        // The real round trip is 480 samples longer than the driver admitted,
        // so takes are landing 480 samples late and the compensation has to
        // grow by 480.
        f.trimMs = trimMsFor (480.0 + 480.0, f);

        const int after = RecordAlign::captureOffsetSamples (f);

        check (f.trimMs > 0.0, "an under-reporting driver gives a positive trim");
        checkClose ((double) (after - before), 480.0, 1.0,
                    "and the take is pulled earlier by exactly what was missing");
    }

    //==========================================================================
    if (failures == 0)
        std::printf ("  all checks passed\n");
    else
        std::printf ("  %d check(s) failed\n", failures);

    return failures == 0 ? 0 : 1;
}
