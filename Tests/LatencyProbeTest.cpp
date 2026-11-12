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
//      cannot, and a reversed cable recovered wherever a straight one is.
//   9. The trim: the total offset is the same whatever the driver claimed.
//  10. A loop that comes back twice is refused rather than answered with a
//      number that belongs to neither path. That is the only thing in here
//      that makes the residual threshold the reason a reading is refused.
//  11. A calibration left running for longer than Fons's phase accumulator
//      survives keeps measuring. The fault that would be is undefined
//      behaviour, so this one is reported by the sanitizers rather than by a
//      comparison.
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

    /** A second path back, at its own delay and level, which is what a
        loopback taken through a speaker and a microphone, or through anything
        with a reflection in it, really gives. Zero gain is the ordinary single
        path. */
    double echoDelay  = 0.0;
    double echoGain   = 0.0;

    /** Where the input stage runs out of room. Zero is an input stage with
        headroom to spare, and anything else clips, which is the difference
        between a loop that is merely loud and a loop being measured through a
        nonlinearity. */
    double clipAt     = 0.0;

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

            if (loop.echoGain != 0.0)
                v += loop.echoGain
                     * LatencyProbe::Probe::idealSample ((double) (n + i) - loop.echoDelay);

            if (loop.inverted)
                v = -v;

            if (loop.noiseRms > 0.0)
                v += noise (rng);

            v += loop.dcOffset;

            // Last, because an input stage clips what reaches it rather than
            // what was sent, and the whole point of the hot cases is that what
            // comes back is no longer a sum of thirteen sinusoids.
            if (loop.clipAt > 0.0)
                v = std::max (-loop.clipAt, std::min (loop.clipAt, v));

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

        // Those two are twenty decibels apart, which is room for the floor to
        // move by a factor of two and still pass, and the floor moves with
        // anything that changes the demodulator's gain rather than its
        // threshold. So the floor is bracketed to about a decibel either side
        // of where the header publishes it: a received fundamental of 1.25e-4,
        // which is a loop gain of -64 dB.
        //
        // The quiet side is given two seconds because the threshold is on the
        // settled magnitude, and the loud side only half of one, because a
        // loop the probe claims to work at has to be recognised while somebody
        // is still looking at the panel. That half second is what times the
        // averaging: 80 ms a stage reaches the threshold from a loop this
        // quiet in about three tenths of a second, and twice that does not
        // reach it at all.
        {
            const auto above = measure ({ 480.0, 7.0e-4 }, bulkRate, 0.5);
            checkStatus (above.status, Status::ok,
                         "a loop a decibel above the published floor is recognised, and quickly");
            checkClose (above.roundTripSamples, 480.0, 1.0e-2,
                        "and measures correctly at the floor");

            checkStatus (measure ({ 480.0, 5.5e-4 }, bulkRate, 2.0).status, Status::noSignal,
                         "a loop a decibel below the published floor is refused");
        }
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
        // The last three are shorter than two fundamental cycles, and that is
        // where the bit ladder has nothing to borrow from. Anywhere else, an
        // error of a whole number of cycles in the fractional part is absorbed
        // as a borrow out of the resolved cycle count, with every bit's
        // residual left exactly where it was, so the answer comes out right
        // anyway. A reversed cable on a very short loopback is therefore the
        // only place several ways of getting that fractional part wrong can be
        // seen at all.
        for (double d : { 0.0, 97.5, 1024.0, 9.0, 12.5, 15.75 })
        {
            Loop loop;
            loop.delay    = d;
            loop.inverted = true;

            const auto r = measure (loop, bulkRate, 1.0);

            checkStatus (r.status, Status::ok, "an inverted loop resolves");
            checkClose (r.roundTripSamples, d, 1.0e-3, "an inverted loop measures correctly");
            check (r.inverted, "an inverted loop is reported as inverted");
        }

        // Randomised, and the six above are not enough on their own, which is
        // worth writing down because it is not visible from the code.
        //
        // Correcting the polarity adds half a cycle to the fundamental's
        // phase, and that is the only path that can push it past half a cycle,
        // so it is the only path that reaches the fold at all. Which side of
        // the fold a delay lands on is set by its remainder modulo sixteen
        // samples, and the three delays this check started with all landed in
        // the lower half, so the fold was never once evaluated: folding the
        // wrong way passed every check in the file.
        //
        // Removing the fold altogether still passes, and that is not a gap:
        // by the borrow above, a fractional part one whole cycle out resolves
        // one cycle fewer and comes to the same answer, with every residual
        // unchanged. The fold is defensive, and the delays below are what says
        // so rather than anybody's reasoning about it.
        {
            std::mt19937 rng (9091u);
            std::uniform_real_distribution<double> delay (0.0, 2000.0);

            double worst = 0.0;

            for (int trial = 0; trial < 64; ++trial)
            {
                Loop loop;
                loop.delay    = delay (rng);
                loop.inverted = true;

                const auto r = measure (loop, bulkRate, 0.5, (unsigned) (trial + 7000));

                checkStatus (r.status, Status::ok,
                             "a reversed cable resolves at any delay  ("
                             + std::to_string (loop.delay) + ")");
                check (r.inverted, "a reversed cable says so at any delay");

                if (r.status == Status::ok)
                    worst = std::max (worst, std::fabs (r.roundTripSamples - loop.delay));
            }

            checkClose (worst, 0.0, 0.01,
                        "64 reversed cables all come back within a hundredth of a sample");
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

        // Those two are a factor of two apart, so the ceiling can be moved
        // most of the way to full scale and still pass both. It is bracketed
        // instead: the published ceiling is -3 dBFS, so a loop peaking just
        // under it is measured and a loop peaking just over it is refused.
        // Raising the ceiling is the direction that costs something real,
        // because what it then accepts is a reading taken through the input
        // stage's own nonlinearity.
        {
            Loop under;
            under.delay = 512.0;
            under.gain  = 2.4;                   // peak 0.704, a whisker under

            const auto justOk = measure (under, bulkRate, 1.0);
            checkStatus (justOk.status, Status::ok, "a loop peaking just under -3 dBFS is measured");
            check (justOk.inputPeak < peakCeiling, "and it really was under the ceiling");

            Loop over;
            over.delay = 512.0;
            over.gain  = 2.6;                    // peak 0.763, a whisker over

            const auto justNot = measure (over, bulkRate, 1.0);
            checkStatus (justNot.status, Status::tooHot,
                         "a loop peaking just over -3 dBFS is refused");
        }

        // A loopback that is actually clipping, rather than one that is merely
        // loud, because the loops above are clean sinusoids at a high level and
        // the ceiling exists for the nonlinearity, not for the level.
        //
        // It is not the case that shows the two checks are applied in the right
        // order, which is worth writing down because it looks like it should
        // be: hard clipping is memoryless and odd, so it leaves every tone's
        // phase where it was and the bits still agree to within a twentieth of
        // a half cycle. Loud noise is what tells the order apart, and check 8
        // does that.
        {
            Loop clipped;
            clipped.delay  = 512.0;
            clipped.gain   = 4.0;
            clipped.clipAt = 0.75;               // over the ceiling, and really clipping

            const auto r = measure (clipped, bulkRate, 1.0);
            checkStatus (r.status, Status::tooHot,
                         "a clipping loopback is reported as too hot, not as unreliable");
        }

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

        // And the same for an infinite one, which is the half of that guard a
        // NaN cannot test: a NaN loses every comparison it is in, so the peak
        // survives one with or without the test, while an infinity wins them
        // all and sticks. The consequence is the one the line above is about,
        // every measurement afterwards refused as too hot with nothing to say
        // why, and it is only reachable through this case.
        {
            Probe probe;
            probe.reset (48000.0);

            std::vector<float> in (32, 0.0f);
            std::vector<float> out (32, 0.0f);

            in[1] = std::numeric_limits<float>::infinity();
            probe.process (in.data(), out.data(), 32);

            check (! (probe.read().inputPeak > peakCeiling),
                   "an infinity on the input does not become the peak either");
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
        // resolved, and the requirement that matters is that one: refused, not
        // wrong.
        //
        // They also have to be refused for the right reason, which is what
        // pins the order the two checks are applied in. Noise this loud fails
        // both tests at once, with a peak well over the ceiling and bits that
        // do not agree, and the order decides which of the two the panel
        // shows. Too hot is the one somebody can act on, because the input
        // gain is a knob in front of them, where an unreliable reading is a
        // dead end. The other order passes everything else in this file: a
        // loopback that is clipping, oddly, is not a case that tells them
        // apart, because hard clipping is memoryless and leaves each tone's
        // phase where it was, so a clipped loop demodulates perfectly well and
        // is refused on its level alone.
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
                checkStatus (r.status, Status::tooHot,
                             "noise at " + std::to_string (c.rms) + " is refused for the reason"
                             " somebody can do something about");
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

        // A reversed cable has to be recovered wherever a straight one would
        // have been accepted, which is a stronger statement than the clean
        // case in check 4 and is the one that pins where the retry is
        // triggered.
        //
        // The signature a reversed cable leaves is half a cycle on the worst
        // bit, and noise moves that signature down towards the threshold that
        // decides whether to try the inversion at all. So a threshold set too
        // close to half a cycle goes on recovering the clean cases and stops
        // recovering exactly the marginal ones, which are the ones somebody
        // with a long cable in a room full of equipment actually has. Stated
        // as a comparison rather than as a level, because what counts as
        // marginal is not ours to pick: whatever the straight loop managed,
        // the reversed one has to manage too.
        for (double rms : { 0.05, 0.1 })
        {
            for (unsigned seed = 1; seed <= 6; ++seed)
            {
                Loop straight;
                straight.delay    = 307.25;
                straight.noiseRms = rms;

                Loop reversed = straight;
                reversed.inverted = true;

                // Short on purpose: a reading taken before the averaging has
                // settled is where the two thresholds are closest together.
                const auto s = measure (straight, 48000.0, 0.1, seed);

                if (s.status != Status::ok)
                    continue;                   // nothing to hold the reversed one to

                const auto t = measure (reversed, 48000.0, 0.1, seed);

                checkStatus (t.status, Status::ok,
                             "a reversed cable is recovered wherever a straight one is accepted");
                check (t.inverted, "and is still reported as reversed");
                checkClose (t.roundTripSamples, 307.25, 1.0,
                            "and measures what the straight one measured");
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

        // Both ends of the range are inside it. A correction of exactly what
        // the control holds is a correction the control holds, and refusing it
        // would throw away a measurement for being exactly at the limit rather
        // than past it, which is the same fault as clamping in the other
        // direction: the studio would refuse and the user would have no way to
        // tell why. Constructed so the arithmetic lands on the bound exactly
        // rather than near it, because near it is what every other case here
        // already covers.
        {
            RecordAlign::Figures exact;
            exact.bufferSize = 256;
            exact.sampleRate = 48000.0;

            exact.inputLatency  = 400;
            exact.outputLatency = 400;        // a claim of 800, clear of the floor
            const double up = trimMsFor (800.0 + 2400.0, exact);
            checkClose (up, RecordAlign::maxTrimMs, 0.0, "2400 samples at 48 kHz is 50 ms");
            check (trimFits (up), "a trim of exactly the control's travel fits");

            exact.inputLatency  = 2000;
            exact.outputLatency = 2000;       // a claim of 4000
            const double down = trimMsFor (4000.0 - 2400.0, exact);
            checkClose (down, RecordAlign::minTrimMs, 0.0, "and the same the other way");
            check (trimFits (down), "a trim of exactly the control's travel the other way fits too");
        }
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
    // 13. A loopback that is not one round trip.
    //
    // The residual is the only thing standing between the demodulator and a
    // confident wrong number, and until this check existed nothing in the file
    // ever made it the reason a reading was refused: every other refusal here
    // is for silence, for being too hot, or for a delay past the plausibility
    // ceiling, so the residual threshold could be moved most of the way to
    // half a cycle, or deleted outright, and the whole file still passed.
    //
    // What exercises it is a loop that comes back twice at different delays,
    // which is what a loopback taken through a speaker and a microphone gives,
    // and what any path with a reflection in it gives. The thirteen phases are
    // then not consistent with any single delay, so the bits stop agreeing with
    // each other while the level stays modest. The answer that would come out
    // sits between the two paths and belongs to neither, so the requirement is
    // refusal, not accuracy.
    {
        for (double echoAt : { 530.0, 560.0, 600.0, 700.0, 1024.0 })
        {
            Loop twoPaths;
            twoPaths.delay     = 512.0;
            twoPaths.echoDelay = echoAt;
            twoPaths.echoGain  = 0.8;

            const auto r = measure (twoPaths, bulkRate, 1.0);

            check (r.status != Status::ok,
                   "a loop that comes back twice is refused rather than averaged  ("
                   + std::to_string (echoAt) + " gave " + name (r.status) + ", "
                   + std::to_string (r.roundTripSamples) + ")");

            // And it is the bits disagreeing that refuses it, not the level:
            // a check that passed because the input was too hot would be
            // testing the ceiling over again.
            check (r.inputPeak <= peakCeiling,
                   "and it is refused for disagreeing with itself, not for being loud");
        }

        // One path and a quiet reflection is still one round trip, so this has
        // to stay a measurement rather than becoming a refusal. Otherwise the
        // threshold could be tightened until nothing passed and the check above
        // would be satisfied by a probe that never measures anything.
        {
            Loop faintEcho;
            faintEcho.delay     = 512.0;
            faintEcho.echoDelay = 600.0;
            faintEcho.echoGain  = 0.02;

            const auto r = measure (faintEcho, bulkRate, 1.0);
            checkStatus (r.status, Status::ok, "a faint reflection is still one round trip");
            checkClose (r.roundTripSamples, 512.0, 1.0,
                        "and the round trip is the path the probe came back on");
        }
    }

    //==========================================================================
    // 14. A calibration left running.
    //
    // The header names this as one of the three faults in the references it
    // does not copy: Fons advances a signed int by up to 4096 a sample and
    // masks only where he reads it, which is signed overflow after about eleven
    // seconds at 48 kHz. That is undefined behaviour rather than a wrong
    // answer, so what it does is the compiler's business and not something a
    // comparison of readings can see. Nothing else in this file runs long
    // enough to reach it: the longest measurement here is two seconds.
    //
    // Twelve seconds of it, with nothing patched for the first twelve and the
    // cable plugged in afterwards, which is also the ordinary way somebody uses
    // the panel. The sanitizers are what report the fault, which is why this
    // has to run under them to mean anything.
    {
        Probe probe;
        probe.reset (48000.0);

        const int block = 512;               // divides the run, so the phase stays known
        std::vector<float> in  ((size_t) block, 0.0f);
        std::vector<float> out ((size_t) block, 0.0f);

        const long long waiting = 48000 * 12;

        for (long long n = 0; n < waiting; n += block)
            probe.process (nullptr, out.data(), block);

        check (probe.elapsedSamples() == waiting, "the probe counted the whole wait");

        for (long long n = waiting; n < waiting + 24000; n += block)
        {
            for (int i = 0; i < block; ++i)
                in[(size_t) i] = (float) (0.5 * Probe::idealSample ((double) (n + i) - 311.5));

            probe.process (in.data(), out.data(), block);
        }

        const auto r = probe.read();
        checkStatus (r.status, Status::ok, "a probe left running still measures when the cable arrives");
        checkClose (r.roundTripSamples, 311.5, 1.0e-2, "and measures the right round trip");
    }

    //==========================================================================
    if (failures == 0)
        std::printf ("  all checks passed\n");
    else
        std::printf ("  %d check(s) failed\n", failures);

    return failures == 0 ? 0 : 1;
}
