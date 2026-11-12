#pragma once
#include "AudioDefaults.h"
#include "RecordAlign.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

//==============================================================================
/** Measuring the round trip through the interface, with a cable from an output
    back into an input.

    WHAT THIS IS FOR

    RecordAlign.h puts a recorded take where it was played by taking off
    `input + output + engine + trim`, where the first two are what the driver
    claims. Its own comment says why the trim is there: the claim is only a
    claim. JUCE's ASIO backend reports zero for both when the driver's
    getLatencies call fails, plenty of drivers never implement it, and Ardour's
    manual puts it plainly, "the only way to accurately learn about the total
    (additional) latency is to measure it".

    So the arithmetic was right and its input was a guess. The trim slider used
    to carry an instruction to record a click through a loopback cable, look at
    where the recorded clicks sat against the metronome, and dial milliseconds
    until they lined up. That is a real method and it is the method of last
    resort: it asks a musician to eyeball a waveform against a grid line at
    whatever zoom they happen to be at.

    This measures it instead, and feeds the control that already exists.

    THE ALGORITHM, AND WHY IT IS NOT AN IMPULSE

    This is MTDM, by Fons Adriaensen, which is what `jack_delay` and
    `jack_iodelay` are and what both Ardour and Harrison Mixbus calibrate
    with. Read from his code rather than from anybody's description of it:
    jack2's `tests/iodelay.cpp`, "Copyright (C) 2003-2008 Fons Adriaensen",
    and the same algorithm as a class in Ardour's `libs/ardour/mtdm.cc`. It is
    GPL-2 or later, so it may be taken at GPL-3, which AGPL-3.0 may combine
    with through section 13 of each: the same argument CLAUDE.md already
    records for the ASIO SDK.

    An impulse and a correlation peak is what anybody would reach for, and it
    is the worse design. A correlation peak for a flat spectrum probe is a
    sinc, and the three point parabolic interpolation everybody uses to get a
    sub-sample answer out of one is biased by up to 0.119 samples, measured,
    with the error an S-shape against the true fractional delay that is zero
    at 0 and 0.5 and worst near 0.2. It does not average away over repeated
    measurements. Julius Smith's formula for that interpolation is exact for a
    Gaussian peak read on a log scale, which is a spectral peak and not this.

    MTDM instead plays thirteen steady sine tones at once and reads the phase
    of each by quadrature demodulation. The fundamental sits at `fs / 16` and
    its phase gives the delay to within one of its own cycles, which is
    sixteen samples. Each of the other twelve then contributes exactly one bit
    of the whole number of cycles on top of that: twelve bits of sixteen
    samples is 65536 samples of unambiguous range, with sub-sample precision
    inside it.

    The whole design is in the frequency table, and the property that makes it
    work is not stated anywhere in the source, so it was checked
    arithmetically rather than taken on trust. With Fons's increments, every
    one of the twelve satisfies

        f_i / f_0 = odd / 2^(i+1)

    exactly: 1/2, 3/4, 5/8, 9/16, 17/32, 17/64, 41/128, 97/256, 225/512,
    833/1024, 1793/2048, 3841/4096. A change of 2^i cycles in the delay
    therefore moves tone i's phase by an odd multiple of half a cycle, which
    flips a bit that nothing else can flip. Which odd numerator is used at
    each step is free, and that freedom is spent keeping all thirteen tones
    inside one comfortable mid band, between `fs / 60` and `fs / 16`, which is
    732 Hz to 2.8 kHz at 44.1 kHz. The frequencies are proportional to the
    sample rate rather than fixed in hertz, so the measurement behaves the
    same at every rate the studio runs at.

    Because it is narrowband and averaged over about 160 ms, it also survives
    things an impulse would not. Measured against noise added to the returned
    probe: correct to 0.05 samples at +3 dB signal to noise, and rejected
    rather than wrong at -17 dB.

    THREE FAULTS IN THE REFERENCES, NOT COPIED

    Worth writing down, because the obvious way to build this is to transcribe
    one of them.

    Ardour's peak tracker, which feeds its own clipping check, tests
    `fabsf (vip) > peak` and then stores the signed `vip`, so one negative
    excursion sets the running peak negative and it loses every comparison
    after that; and the hold afterwards assigns `vip`, the last sample of the
    block, rather than `peak`. `get_peak()` therefore returns close to an
    arbitrary sample value. Fons's own code has no peak tracking at all, so
    this is Ardour's addition. The one below is written from scratch and
    tested.

    Fons's phase accumulator is a signed `int` advanced by up to 4096 a sample
    and then masked with `& 65535`. That is signed overflow after about eleven
    seconds at 48 kHz, which is undefined behaviour rather than a wrong
    answer, and a calibration somebody leaves running reaches it. The
    accumulator here is unsigned and masked, which is the same arithmetic
    without the fault.

    And MTDM wraps silently past its range: a true delay of 70000 samples
    reads as 4464 with a clean residual. That is the only failure mode that
    produces a confident wrong answer, and it is the reason for the ceiling
    below. Note honestly what the ceiling can and cannot do: it catches a
    measurement that is absurd, and it cannot catch a wrap, because a wrapped
    answer looks ordinary. What makes the wrap unreachable in practice is that
    65536 samples is 1.37 seconds at 48 kHz and the probe does not pass
    through the mixer, so there is nothing in the loop that could hold it that
    long.

    No JUCE in here, so Tests/LatencyProbeTest.cpp drives all of it through an
    analytically exact fractional delay. That is possible because the probe is
    a sum of sinusoids with linear phase, so a delayed copy of it can be
    written down exactly rather than interpolated, which is what makes the
    test tight enough to catch a single wrong digit in the table.
    LatencyCalibration.h keeps the part that genuinely needs JUCE, which is
    being a device callback.
*/
namespace LatencyProbe
{
    //==========================================================================
    /** The tones, as phase increments in units of 1/65536 of a cycle per
        sample. Fons's table verbatim, and the bit ladder property above is
        what each entry is for, so none of them is free to be adjusted: the
        test asserts the ratios.
    */
    inline constexpr int numTones = 13;
    inline constexpr int numBits  = numTones - 1;

    inline constexpr int toneIncrement[numTones] =
    {
        4096,                                                   // the fundamental, fs/16
        2048, 3072, 2560, 2304, 2176, 1088,                     // bits 0 to 5
        1312, 1552, 1800, 3332, 3586, 3841                      // bits 6 to 11
    };

    /** One whole cycle of phase, which is also what the accumulator is masked
        with. */
    inline constexpr std::uint32_t phaseCycle = 65536;

    /** The initial phase every tone starts at, Fons's `F->p = 128`. It is a
        512th of a cycle and nothing depends on it, but the test compares the
        streamed probe against the closed form of it, so the two have to agree
        about where it starts. */
    inline constexpr std::uint32_t initialPhase = 128;

    /** How many samples the fundamental's cycle is worth, which is the weight
        of the lowest bit the ladder resolves. */
    inline constexpr int samplesPerFundamentalCycle = (int) (phaseCycle / (std::uint32_t) toneIncrement[0]);

    /** The unambiguous range: twelve bits of sixteen samples. 1.37 seconds at
        48 kHz. */
    inline constexpr int unambiguousSamples = samplesPerFundamentalCycle << numBits;

    /** The probe's levels. The fundamental carries the fractional part of the
        answer and the other twelve only have to carry one bit each, so they
        are twenty six decibels quieter, which keeps the whole probe's peak
        under -9.9 dBFS. Fons's figures. */
    inline constexpr float fundamentalAmplitude = 0.20f;
    inline constexpr float bitToneAmplitude     = 0.01f;

    /** The demodulator's averaging, as the one pole coefficient applied every
        sixteen samples: `200 / fs` for two cascaded stages, which is 80 ms a
        stage at any sample rate. Fons's figure, and it is what buys the noise
        rejection above. */
    inline constexpr double averagingNumerator = 200.0;
    inline constexpr int    accumulateSamples  = 16;

    /** Nothing came back. The threshold is on the fundamental's demodulated
        magnitude, whose steady state is eight times the received amplitude of
        that tone, so 0.001 here is a received fundamental of 1.25e-4 and a
        loop gain of -64 dB. Fons's constant, and generous: a measurement at
        -60 dB of loop gain still resolves correctly. */
    inline constexpr double signalThreshold = 0.001;

    /** Too hot to believe. Ardour refuses a result whose input peak is above
        this, and the number is its own: -3 dBFS. A loopback that is clipping
        is being measured through a nonlinearity, and the tones are no longer
        the only thing in the band. */
    inline constexpr float peakCeiling = 0.707f;

    /** The two residual thresholds, both Ardour's and both in units of half a
        cycle, so they run from 0 to 0.5. Past the first, try it inverted;
        past the second, the result is not usable. A reversed cable gives
        exactly 0.5 before inverting, which is a signature rather than a
        failure. */
    inline constexpr double invertResidual   = 0.3;
    inline constexpr double unusableResidual = 0.2;

    /** The ceiling on a believable round trip, and this one is this studio's
        own rule rather than anybody's specification: no reference publishes a
        plausibility bound at all. Half the unambiguous range, which is 0.68
        seconds at 48 kHz, so it is far above anything a device can produce
        and far below where the arithmetic wraps. See the note above about
        what it cannot do. */
    inline constexpr double maxBelievableSamples = unambiguousSamples / 2;

    /** How far below zero a reading is allowed to sit before it is refused.

        A reading can be slightly negative without anything being wrong: the
        fundamental's phase is folded into plus or minus half a cycle, so a
        true delay of nothing reads as a hair either side of it. One whole
        fundamental cycle of slack covers that and still refuses the case
        worth refusing, which is a measurement well under the driver's own
        claim. Focusrite's own article gives the direction: "Some interfaces
        do report the true latency, but many under-report the actual value".
        Under-reporting is what this exists to fix; over-reporting by more
        than a cycle is a different fault.
    */
    inline constexpr double minBelievableSamples = -(double) samplesPerFundamentalCycle;

    //==========================================================================
    /** Why a reading cannot be used, or that it can.

        Ordered so that `ok` is zero and anything else is a refusal, and the
        checks are applied in Ardour's own order: nothing came back, then too
        hot, then inconsistent after an inversion has been tried.
    */
    enum class Status
    {
        ok = 0,
        noSignal,     /**< nothing came back: no cable, or nothing patched */
        tooHot,       /**< the returned probe is above -3 dBFS */
        unreliable,   /**< the twelve bit decisions do not agree with each other */
        implausible   /**< resolved, but the number cannot be a round trip */
    };

    /** What one reading says. */
    struct Reading
    {
        Status status = Status::noSignal;

        /** The round trip in samples, fractional. Only meaningful when the
            status is `ok`, and left at zero otherwise rather than reported as
            a number somebody might write down. */
        double roundTripSamples = 0.0;

        /** The worst of the twelve bit decisions, in half cycles, so 0 is
            perfect and 0.5 is as wrong as it can be. It falls as the
            measurement settles, so it is a progress indicator as much as a
            quality one: it passes 0.2 after about two blocks and keeps
            improving for half a second after that. */
        double worstResidual = 0.5;

        /** The loudest thing seen on the input since the measurement started,
            as a magnitude. Shown so that "no cable" and "the input gain is
            wound up" can be told apart. */
        float inputPeak = 0.0f;

        /** Whether the answer was recovered by inverting the demodulation,
            which means the cable has its polarity reversed somewhere. The
            reading is still correct. Ardour warns and accepts for the same
            reason, with a comment in its source pointing at the pull request
            that established it. */
        bool inverted = false;
    };

    //==========================================================================
    /** The probe: generates the tones, demodulates what comes back, and
        resolves the two into a delay.

        Fixed size and allocation free, because `process` runs on the audio
        thread. It holds no lock and does no logging for the same reason.
    */
    class Probe
    {
    public:
        Probe() = default;

        /** Starts a measurement at a sample rate. Everything the demodulator
            has learned is dropped, so this is also how a measurement is taken
            again after the cable has been moved.

            A sample rate of nothing is accepted and leaves the probe silent
            rather than dividing by it: a device can report zero between
            being closed and being opened.
        */
        void reset (double sampleRateHz) noexcept
        {
            sampleRate = (std::isfinite (sampleRateHz) && sampleRateHz > 0.0) ? sampleRateHz : 0.0;
            averaging  = sampleRate > 0.0 ? averagingNumerator / sampleRate : 0.0;

            for (auto& t : tones)
                t = {};

            accumulated = 0;
            peak        = 0.0f;
            invert      = false;
            elapsed     = 0;
        }

        /** One block: writes the probe into `out` and demodulates `in`.

            Both in one pass, and that is not a convenience. The demodulator
            multiplies the incoming sample by the oscillator's phase *now*,
            and the answer is the lag between what is arriving and what is
            being played at that instant. Generating in one place and
            demodulating in another would mean keeping two phase accumulators
            in step, and a drift between them would read as a delay.

            `in` may be null, which is a device with no input open. The probe
            is still written, so something is heard and the absence shows up
            as no signal rather than as silence at both ends.
        */
        void process (const float* in, float* out, int numSamples) noexcept
        {
            if (numSamples <= 0 || sampleRate <= 0.0)
            {
                // Still the caller's buffer to fill: see LatencyCalibration.h
                // on why an unwritten output buffer is not silence.
                if (out != nullptr)
                    for (int n = 0; n < numSamples; ++n)
                        out[n] = 0.0f;

                return;
            }

            for (int n = 0; n < numSamples; ++n)
            {
                const double sample = in != nullptr ? (double) in[n] : 0.0;

                // A magnitude, held, and that is the whole of the fault in
                // Ardour's version of this: it compared a magnitude and then
                // stored a signed value, so one negative excursion poisoned
                // every comparison after it.
                const float magnitude = std::fabs (in != nullptr ? in[n] : 0.0f);

                if (std::isfinite (magnitude) && magnitude > peak)
                    peak = magnitude;

                double sum = 0.0;

                for (int i = 0; i < numTones; ++i)
                {
                    auto& t = tones[i];

                    const double angle = 2.0 * pi * (double) (t.phase & (phaseCycle - 1))
                                                   / (double) phaseCycle;

                    // Unsigned and masked, where Fons has a signed int that
                    // overflows after about eleven seconds at 48 kHz.
                    t.phase = (t.phase + (std::uint32_t) toneIncrement[i]) & (phaseCycle - 1);

                    const double c =  std::cos (angle);
                    const double s = -std::sin (angle);

                    sum += (i == 0 ? (double) fundamentalAmplitude : (double) bitToneAmplitude) * s;

                    t.xa += s * sample;
                    t.ya += c * sample;
                }

                if (out != nullptr)
                    out[n] = (float) sum;

                if (++accumulated == accumulateSamples)
                {
                    for (auto& t : tones)
                    {
                        // Two cascaded one poles, 80 ms a stage at any rate.
                        t.x1 += averaging * (t.xa - t.x1);
                        t.y1 += averaging * (t.ya - t.y1);
                        t.x2 += averaging * (t.x1 - t.x2);
                        t.y2 += averaging * (t.y1 - t.y2);

                        t.xa = t.ya = 0.0;
                    }

                    accumulated = 0;
                }

                ++elapsed;
            }
        }

        /** Resolves what the demodulator currently holds.

            Const in spirit and not in signature, because recovering a reading
            from a reversed cable means inverting the demodulation and trying
            again, and that choice is kept so the next reading does not have
            to rediscover it. Fons does the same.
        */
        Reading read() noexcept
        {
            Reading r;
            r.inputPeak = peak;

            if (sampleRate <= 0.0)
                return r;

            if (std::hypot (tones[0].x2, tones[0].y2) < signalThreshold)
                return r;                                  // noSignal

            // Before the residual, because a clipping loopback is being
            // measured through a nonlinearity and the residual it produces
            // means nothing. Ardour tests it in this order too.
            if (! (peak <= peakCeiling))
                { r.status = Status::tooHot; return r; }

            auto attempt = [this]
            {
                Resolved out;

                // The fractional part, in cycles of the fundamental, folded
                // into plus or minus half a cycle. That fold is why a delay
                // of nothing can read slightly negative.
                double d = std::atan2 (tones[0].y2, tones[0].x2) / (2.0 * pi);

                if (invert)
                    d += 0.5;

                if (d > 0.5)
                    d -= 1.0;

                const double f0 = (double) toneIncrement[0];
                double       m  = 1.0;

                for (int i = 1; i < numTones; ++i)
                {
                    // What this tone's phase would be if the delay were just
                    // the fractional part, subtracted from what it is. What
                    // is left is half a cycle per set bit, by the ratio
                    // property above.
                    double p = std::atan2 (tones[i].y2, tones[i].x2) / (2.0 * pi)
                             - d * (double) toneIncrement[i] / f0;

                    if (invert)
                        p += 0.5;

                    p -= std::floor (p);
                    p *= 2.0;

                    const double k = std::floor (p + 0.5);
                    const double e = std::fabs (p - k);

                    out.residual = std::max (out.residual, e);

                    // The bit is the parity of k: one half cycle set, two
                    // half cycles back where it started.
                    if (std::fmod (std::fabs (k), 2.0) >= 0.5)
                        d += m;

                    m *= 2.0;
                }

                out.samples = (double) samplesPerFundamentalCycle * d;
                return out;
            };

            auto resolved = attempt();

            // A reversed cable gives a worst residual of exactly 0.5, so this
            // is not a retry in hope: it is the one correction that turns that
            // signature into the right answer.
            if (resolved.residual > invertResidual)
            {
                invert = ! invert;
                const auto second = attempt();

                if (second.residual < resolved.residual)
                    resolved = second;
                else
                    invert = ! invert;                     // no better: put it back
            }

            r.inverted      = invert;
            r.worstResidual = resolved.residual;

            if (resolved.residual > unusableResidual)
                { r.status = Status::unreliable; return r; }

            if (! std::isfinite (resolved.samples)
                || resolved.samples < minBelievableSamples
                || resolved.samples > maxBelievableSamples)
                { r.status = Status::implausible; return r; }

            r.status           = Status::ok;

            // Negative by less than a fundamental cycle is the fold, not a
            // round trip that ran backwards, so it is reported as the nothing
            // it is.
            r.roundTripSamples = std::max (0.0, resolved.samples);
            return r;
        }

        /** How long the measurement has been running, in samples. The caller
            needs it to decide whether a refusal means "no cable" or "not
            settled yet": the residual starts at its worst and comes down. */
        long long elapsedSamples() const noexcept { return elapsed; }

        /** The probe as a function of continuous time, which is what lets a
            test delay it by a fractional number of samples exactly instead of
            interpolating it.

            Here rather than in the test because it is the definition of what
            `process` streams, and the two agreeing at whole samples is itself
            worth asserting: a single wrong digit in the table breaks the
            agreement.
        */
        static double idealSample (double n) noexcept
        {
            double sum = 0.0;

            for (int i = 0; i < numTones; ++i)
            {
                const double cycles = ((double) initialPhase + n * (double) toneIncrement[i])
                                      / (double) phaseCycle;

                sum += (i == 0 ? (double) fundamentalAmplitude : (double) bitToneAmplitude)
                       * -std::sin (2.0 * pi * cycles);
            }

            return sum;
        }

    private:
        static constexpr double pi = 3.14159265358979323846;

        struct Tone
        {
            std::uint32_t phase = initialPhase;
            double xa = 0.0, ya = 0.0;
            double x1 = 0.0, y1 = 0.0;
            double x2 = 0.0, y2 = 0.0;
        };

        struct Resolved
        {
            double samples  = 0.0;
            double residual = 0.0;
        };

        Tone      tones[numTones] {};
        double    sampleRate  = 0.0;
        double    averaging   = 0.0;
        int       accumulated = 0;
        float     peak        = 0.0f;
        bool      invert      = false;
        long long elapsed     = 0;
    };

    //==========================================================================
    /** The trim that makes the compensation land on a measured round trip.

        This is the whole point of the measurement, so it is worth being exact
        about what it is. `RecordAlign::captureOffsetSamples` takes off
        `claimed + engine + trim`, where `claimed` is what the driver said or
        the two buffer floor when it said nothing. A measurement replaces the
        claim, and the way to replace it through a control that is defined as a
        correction to a claim is to make the trim the difference.

        So the property this has to produce, and the one the test asserts, is
        that **the total is independent of how honest the driver was**. A
        device whose claim is right gives a trim of zero. A device claiming
        nothing, which is the ASIO failure RecordAlign.h already names, gives a
        trim of the whole round trip. Both land on the same total.

        Storing the difference rather than the absolute is also what Ardour
        does on its Windows path, where what it subtracts from the measurement
        before storing is PortAudio's reported input and output latency, which
        is the driver's own claim. It has a practical virtue: the claim moves
        with the buffer size and so does the real round trip, so the
        difference mostly survives a change of buffer size, where an absolute
        figure would not. Mostly, not exactly, which is why the panel says to
        measure again after changing it.

        The engine's own latency is deliberately not in here. The probe is a
        device callback and does not pass through the mixer, so there is
        nothing of the engine's in what it measured, and `captureOffsetSamples`
        adds the engine's figure separately.
    */
    inline double trimMsFor (double measuredRoundTripSamples,
                             const RecordAlign::Figures& figures) noexcept
    {
        if (! std::isfinite (measuredRoundTripSamples) || figures.sampleRate <= 0.0)
            return 0.0;

        const double claimed = (double) RecordAlign::claimedRoundTripSamples (figures);

        return 1000.0 * (measuredRoundTripSamples - claimed) / figures.sampleRate;
    }

    /** Whether that trim is one the control can actually hold.

        Asked separately, and refused rather than clamped, because a trim
        clamped to the end of its range is a studio that has applied some of a
        correction and said it applied all of it. RecordAlign.h made the same
        choice about the trim's own bounds and for the same reason: "a studio
        that silently ignores half of a control's range is a studio that looks
        broken".

        A measurement that needs more than fifty milliseconds of correction is
        not a misreporting driver anyway. It is a different fault, and papering
        over it with a trim would hide it.
    */
    inline bool trimFits (double trimMs) noexcept
    {
        return std::isfinite (trimMs)
            && trimMs >= RecordAlign::minTrimMs
            && trimMs <= RecordAlign::maxTrimMs;
    }
}
