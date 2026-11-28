// ---------------------------------------------------------------------------
// Checks how an audio clip is read when the read position is not a whole
// sample, which is almost always.
//
// What this replaced was two point linear interpolation, and the single most
// important thing about testing it is in ClipResampler.h's header comment:
// **at a step of exactly one source sample per output sample with a constant
// fractional offset, which is the trimmed clip and by far the most common
// case, linear interpolation is linear and time invariant.** It produces no
// aliasing, no images and no distortion whatsoever. So a test built around
// any of those three passes the old code with full marks on the case the
// product hits most, and a suite written that way would have been a file full
// of correct figures about cases nobody meets.
//
// So the central check here is not a spur measurement. It is the **residual
// against the analytic ideal**: read a sine, and compare every output sample
// against `sin(2 pi f p / R)` evaluated at the position that sample was read
// from. That is the strongest oracle available, for two reasons. The kernel is
// symmetric about the read position, so the read is linear phase with no group
// delay and the ideal needs no alignment. And an error is an error wherever it
// lands, so one number catches a magnitude tilt, an image, a phase error and a
// stray partial without having to guess in advance which one the next mistake
// will be. It is the same argument WaveTable.h's test arrived at the expensive
// way: "a check that compares against additive synthesis sees everything
// wherever it lands".
//
// Measured, and the figures are the ones this file prints rather than rounder
// ones: the residual is -109.06 dB at worst for the bandlimited read and
// -12.28 dB for the lerp at 10 kHz, a separation of 96.8 dB. An earlier draft
// of this comment said "more than a hundred decibels" and "-110 dB at worst",
// which is the shape of mistake this project keeps meeting in its own prose
// and is why the figures below are printed by the program rather than only
// written here.
//
// The properties, each a claim the header makes rather than a restatement of
// its arithmetic:
//
//   * The residual against the ideal, over randomised fractional offsets and
//     the sample rate pairs this studio actually meets. This is the check that
//     fails if the read goes back to a lerp, and the only one that does.
//   * **That measurement is pointed at linear interpolation and required to
//     notice**, because a measurement that cannot fail is not evidence.
//   * Aliasing when the read runs fast, at the one frequency where it can be
//     seen, which is the source tone that folds onto exactly 20 kHz. Also
//     pointed at a read with the time scaling removed and required to notice,
//     because that single omission is the whole reason Smith's min{Fs,Fs'}
//     rule exists and nothing else in this file can see it.
//   * The Kaiser window measured against its own specification by transform,
//     which is what catches a beta from the wrong branch of the piecewise
//     formula, a Bessel series that stops too early, and the 6.28 that comes
//     of confusing radians per sample with cycles per sample.
//   * Every phase's taps sum to one, and the mirror symmetry that makes a per
//     entry scale a legitimate per phase scale, asserted to the last bit.
//   * **A read at an exactly integer position returns the stored sample bit for
//     bit**, at any step of one or less, which only holds because a sinc of a
//     whole number of samples is written as exactly zero rather than
//     evaluated. That is a property of the kernel and not a promise about the
//     product: the engine's read position comes from an accumulated beat, so it
//     lands near a whole sample rather than on one, and ClipResampler.h says
//     what that is worth.
//   * The step, which decides the cutoff, and which until an audit said so
//     lived in the engine where nothing could reach it.
//   * The loop covering exactly the taps that can contribute, asserted by
//     putting an impulse at the outermost one and at the first one beyond it.
//   * What a read can produce from full scale material, which is not one,
//     against the kernel's own worst case gain and with the signal that
//     attains it constructed.
//   * The read is linear, which is asserted as superposition and homogeneity
//     over randomised signals rather than assumed from the shape of the code.
//   * Reads off both ends of a buffer, over randomised short buffers, where
//     the sanitizers are doing most of the work.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include "ClipResampler.h"
#include "RealFFT.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;

static void check (bool ok, const std::string& what)
{
    if (! ok)
    {
        std::printf ("  FAIL: %s\n", what.c_str());
        ++failures;
    }
}

static constexpr double pi = 3.14159265358979323846;

// Splitmix64 and not <random>, for the reason PanOneBreed.h gives: the
// standard distributions are not specified exactly, so one seed gives a
// different sequence on a different compiler and a failure then cannot be
// reproduced from the seed printed beside it.
struct Rng
{
    std::uint64_t state;

    explicit Rng (std::uint64_t seed) : state (seed) {}

    std::uint64_t next() noexcept
    {
        state += 0x9e3779b97f4a7c15ull;
        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    /** In [0, 1). */
    double unit() noexcept { return (double) (next() >> 11) * (1.0 / 9007199254740992.0); }

    double between (double lo, double hi) noexcept { return lo + (hi - lo) * unit(); }

    int upTo (int n) noexcept { return n <= 0 ? 0 : (int) (next() % (std::uint64_t) n); }
};

//==============================================================================
// The two reads being compared, over a generated sine.

enum class Read { bandlimited, linear, unscaled };

/** A read that omits Smith's time and amplitude scaling, kept so the aliasing
    measurement can be shown to notice its absence. This is not a copy of the
    shipped read with a flag: it is deliberately the broken version, and the
    only thing it is used for is to fail. */
static double readUnscaled (const ClipResampler::Kernel& k, const float* src, int frames,
                            double position) noexcept
{
    const int    count = k.entryCount();
    const double tableScale = (double) k.entriesPerCrossing;
    const double floored = std::floor (position);
    const int    centre  = (int) floored;
    const double offset  = position - floored;

    double sum = 0.0;

    for (int i = -k.crossings; i <= k.crossings + 1; ++i)
    {
        const int sample = centre + i;

        if (sample < 0 || sample >= frames)
            continue;

        const double distance = std::abs ((double) i - offset) * tableScale;
        const int    entry    = (int) distance;

        if (entry >= count - 1)
            continue;

        const double eta = distance - (double) entry;
        sum += ((double) k.taps[(std::size_t) entry] + eta * (double) k.differences[(std::size_t) entry])
             * (double) src[sample];
    }

    return sum;
}

struct Pass
{
    double residualDb = 0.0;   /**< error power against the ideal, relative */
    double levelDb    = 0.0;   /**< output level against the input's */
};

/** Reads a generated sine and reports both the residual against the analytic
    ideal and the output's own level. One pass gives both because the two
    answer different questions: the residual is the quality figure, and the
    level is what says whether a tone above the output's Nyquist was rejected
    or let through. */
static Pass sweep (const ClipResampler::Kernel& k, Read how, double tone, double sourceRate,
                   double step, double offset, int outputSamples = 6000)
{
    const int pad    = 4096;
    const int frames = (int) (outputSamples * step) + 2 * pad + 8;

    std::vector<float> source ((std::size_t) frames);

    for (int i = 0; i < frames; ++i)
        source[(std::size_t) i] = (float) std::sin (2.0 * pi * tone * i / sourceRate);

    double errorPower = 0.0, idealPower = 0.0, outputPower = 0.0;
    double position = (double) pad + offset;

    for (int i = 0; i < outputSamples; ++i)
    {
        double value = 0.0;

        switch (how)
        {
            case Read::bandlimited:
                value = ClipResampler::read (k, source.data(), source.data(), frames,
                                             position, step).left;
                break;

            case Read::linear:
            {
                const int   f0 = (int) position;
                const double d = position - f0;
                value = source[(std::size_t) f0]
                      + (source[(std::size_t) (f0 + 1)] - source[(std::size_t) f0]) * d;
                break;
            }

            case Read::unscaled:
                value = readUnscaled (k, source.data(), frames, position);
                break;
        }

        const double ideal = std::sin (2.0 * pi * tone * position / sourceRate);

        errorPower  += (value - ideal) * (value - ideal);
        idealPower  += ideal * ideal;
        outputPower += value * value;
        position    += step;
    }

    Pass out;
    out.residualDb = 10.0 * std::log10 (errorPower / idealPower + 1.0e-300);
    out.levelDb    = 10.0 * std::log10 (outputPower / ((double) outputSamples * 0.5) + 1.0e-300);
    return out;
}

//==============================================================================
/** The sample rate pairs a session here actually meets, as a source rate and
    the step that reading it at the session's rate produces. */
struct Conversion
{
    const char* what;
    double sourceRate;
    double sessionRate;
    double step;
};

static const Conversion conversions[] =
{
    { "a clip at the session rate, trimmed off a sample boundary", 48000.0, 48000.0, 1.0 },
    { "the same at 44.1 kHz",                                      44100.0, 44100.0, 1.0 },
    { "a 44.1 kHz file in a 48 kHz session",      44100.0, 48000.0,  44100.0 / 48000.0 },
    { "a 48 kHz file in a 44.1 kHz session",      48000.0, 44100.0,  48000.0 / 44100.0 },
    { "an 88.2 kHz file in a 44.1 kHz session",   88200.0, 44100.0,  2.0 },
    { "a 96 kHz file in a 48 kHz session",        96000.0, 48000.0,  2.0 },
    { "a 192 kHz file in a 48 kHz session",      192000.0, 48000.0,  4.0 },
    { "a clip stretched to half speed",           48000.0, 48000.0,  0.5 },
    { "a clip played 1.2 times fast",             44100.0, 44100.0,  1.2 },
};

//==============================================================================
int main()
{
    const ClipResampler::Kernel kernel;

    // -----------------------------------------------------------------------
    // The table's own shape, which three later properties rest on.
    // -----------------------------------------------------------------------
    {
        const int L = kernel.entriesPerCrossing;

        check (kernel.entryCount() == kernel.crossings * L + 1,
               "the table holds one entry per step of the right wing plus the far end");

        // Phase zero is an exact impulse. This is the whole reason a sinc of a
        // whole number of samples is written down rather than evaluated, and
        // the bit exactness check below depends on it.
        check (kernel.taps[0] == 1.0f, "the centre tap is exactly one");

        for (int crossing = 1; crossing <= kernel.crossings; ++crossing)
            check (kernel.taps[(std::size_t) (crossing * L)] == 0.0f,
                   "the tap a whole " + std::to_string (crossing)
                       + " samples out is exactly zero, so phase zero is an impulse");

        // Every phase's taps sum to one, which is that phase's DC gain. A
        // residual here is a gain that varies with the fractional position,
        // which is a modulation of a steady signal.
        double worstSum = 0.0;

        for (int phase = 0; phase <= L; ++phase)
            worstSum = std::max (worstSum, std::abs (kernel.phaseSum (phase) - 1.0));

        check (worstSum < 1.0e-6,
               "every phase's taps sum to one, worst error " + std::to_string (worstSum));

        // And the mirror symmetry that makes one scale per table entry a
        // legitimate scale per phase. Entry j is reached as phase j mod L by
        // one wing and as phase L - (j mod L) by the other, and those are
        // different phases: scaling the entry is only valid because the two
        // use the identical set of entries. Asserted to the last bit, because
        // if it is ever approximately true the normalisation is quietly wrong.
        for (int phase = 1; phase < L; ++phase)
            check (kernel.phaseSum (phase) == kernel.phaseSum (L - phase),
                   "phase " + std::to_string (phase) + " and phase "
                       + std::to_string (L - phase) + " sum identically");

        // The window reaches its own edge rather than being truncated early,
        // and the kernel decays. A Bessel series that stops too soon, or a
        // beta from the wrong branch, shows up in the transform check below;
        // this catches an off by one at the end of the table.
        check (std::abs (kernel.taps[(std::size_t) (kernel.entryCount() - 1)]) < 1.0e-6,
               "the far end of the table has decayed");
        check (kernel.differences[(std::size_t) (kernel.entryCount() - 1)] == 0.0f,
               "the last difference is zero so the final entry cannot be read past");
    }

    // -----------------------------------------------------------------------
    // The Kaiser window against its own specification, by transform.
    //
    // The tabulated kernel is the prototype lowpass sampled at L times the
    // source rate, so transforming the whole of it measures the filter the
    // design asked for. The cutoff sits at half the source rate, which is
    // 1/(2L) of the oversampled rate, and the transition is the designed width
    // divided by L. This is the check that catches the three mistakes nothing
    // else here can see: beta from the wrong branch of the piecewise formula,
    // a Bessel series truncated too early, and a transition width given in
    // radians per sample where the formula wanted cycles per sample, which is
    // a factor of 6.28 in the length.
    // -----------------------------------------------------------------------
    {
        const int L = kernel.entriesPerCrossing;
        const int size = 1 << 17;

        std::vector<float> impulse ((std::size_t) size, 0.0f);

        // Both wings, from the stored right one.
        for (int j = 0; j < kernel.entryCount(); ++j)
        {
            impulse[(std::size_t) j] = kernel.taps[(std::size_t) j];

            if (j > 0)
                impulse[(std::size_t) (size - j)] = kernel.taps[(std::size_t) j];
        }

        RealFFT fft;
        fft.setSize (size);

        std::vector<std::complex<double>> bins ((std::size_t) (size / 2 + 1));
        fft.forward (impulse.data(), bins.data());

        const double dc = std::abs (bins[0]);
        check (dc > 0.0, "the kernel has a DC gain to normalise by");

        // The designed transition width, in cycles per source sample, from the
        // formula the header quotes. Divided by L to put it in the oversampled
        // domain this transform lives in.
        const double transition = (kernel.attenuationDb - 7.95)
                                / (14.36 * (double) (2 * kernel.crossings));

        const double cutoff        = 0.5 / (double) L;
        const double stopbandEdge  = cutoff + 0.5 * transition / (double) L;
        const double passbandEdge  = cutoff - 0.5 * transition / (double) L;

        double worstStopband = -1.0e300, worstPassband = 0.0;

        for (int bin = 0; bin <= size / 2; ++bin)
        {
            const double frequency = (double) bin / (double) size;
            const double level = 20.0 * std::log10 (std::abs (bins[(std::size_t) bin]) / dc + 1.0e-300);

            if (frequency >= stopbandEdge)
                worstStopband = std::max (worstStopband, level);
            else if (frequency <= passbandEdge)
                worstPassband = std::max (worstPassband, std::abs (level));
        }

        check (worstStopband <= -kernel.attenuationDb,
               "the designed stopband reaches its specification of "
                   + std::to_string (kernel.attenuationDb) + " dB, worst "
                   + std::to_string (worstStopband) + " dB");

        check (worstPassband < 0.01,
               "the passband is flat to 0.01 dB, worst " + std::to_string (worstPassband) + " dB");

        // And the shape parameter itself against the figures the quoted
        // piecewise formula gives, on all three of its branches. Pinning a
        // published constant is not the same thing as recomputing the header's
        // arithmetic: the formula IS the specification here, and a beta from
        // the wrong branch or with the length formula's numerator in it lands
        // close enough to pass every measurement above while being a different
        // filter from the one the header says it is.
        check (std::abs (ClipResampler::kaiserBeta (120.0) - 12.26526) < 1.0e-5,
               "beta at 120 dB is 12.26526, got "
                   + std::to_string (ClipResampler::kaiserBeta (120.0)));
        check (std::abs (ClipResampler::kaiserBeta (40.0) - 3.39532) < 1.0e-5,
               "beta at 40 dB comes off the middle branch, got "
                   + std::to_string (ClipResampler::kaiserBeta (40.0)));
        check (ClipResampler::kaiserBeta (15.0) == 0.0,
               "beta below 21 dB is zero, which is no window at all");

        std::printf ("  window: stopband %.2f dB against %.0f asked for, passband ripple %.6f dB\n",
                     worstStopband, kernel.attenuationDb, worstPassband);

        // And the unit confusion, stated as a property rather than left to the
        // comment: the same width in radians per sample would be 2 pi times
        // smaller a number, so a kernel designed from it would be 6.28 times
        // longer. Checking that the two differ by that factor is what makes
        // the choice in the header visible to something that runs.
        const double inRadians = (kernel.attenuationDb - 7.95) / (2.285 * (2.0 * pi * transition));
        const double inCycles  = ClipResampler::kaiserTaps (kernel.attenuationDb, transition);

        check (std::abs (inRadians / inCycles - 1.0) < 1.0e-3,
               "the radians per sample and cycles per sample forms of the length formula "
               "agree once the units are converted, so the constant in use is the right one "
               "for its argument");
    }

    // -----------------------------------------------------------------------
    // The step, which decides the cutoff and used to be two expressions in the
    // engine that nothing could reach. An audit pointed out that inverting
    // either of them left all 57 programs green while a 96 kHz file read into a
    // 48 kHz session passed everything above 24 kHz through, folded, at full
    // level: the kernel was thoroughly tested and the two lines feeding it were
    // not tested at all.
    // -----------------------------------------------------------------------
    {
        for (const auto& conversion : conversions)
        {
            const double step = ClipResampler::stepFor (1.0, conversion.sourceRate,
                                                        conversion.sessionRate);
            const double expected = conversion.sourceRate / conversion.sessionRate;

            check (std::abs (step - expected) < 1.0e-12,
                   std::string ("the step for ") + conversion.what + " is "
                       + std::to_string (expected) + ", got " + std::to_string (step));
        }

        // The direction, stated as a property rather than as a number, because
        // an inverted ratio is the mistake and it still looks like a ratio. A
        // file at a higher rate than the device has to be read at more than one
        // source sample per output sample, or it plays slow.
        check (ClipResampler::stepFor (1.0, 96000.0, 48000.0) > 1.0,
               "a file at a higher rate than the device reads at more than one source "
               "sample per output sample");
        check (ClipResampler::stepFor (1.0, 44100.0, 48000.0) < 1.0,
               "a file at a lower rate than the device reads at less than one");
        check (ClipResampler::stepFor (2.0, 48000.0, 48000.0) > 1.0,
               "a clip stretched to twice the speed reads at more than one");

        // And the two forms have to agree, which is the check that catches an
        // inversion or a dropped conversion in either: a warped clip whose
        // segments all carry the slope an unwarped clip would have must read at
        // exactly the same step.
        for (double bpm : { 60.0, 120.0, 128.0, 174.0 })
        {
            for (const auto& conversion : conversions)
            {
                const double beatsPerSample  = bpm / 60.0 / conversion.sessionRate;
                const double secondsPerBeat  = 60.0 / bpm;
                const double rate            = 1.3;

                const double plain = ClipResampler::stepFor (rate, conversion.sourceRate,
                                                             conversion.sessionRate);
                const double warped = ClipResampler::stepForWarpSlope (secondsPerBeat * rate,
                                                                       beatsPerSample,
                                                                       conversion.sourceRate);

                check (std::abs (plain - warped) < 1.0e-12 * std::max (1.0, plain),
                       "the warped and unwarped forms of the step agree at "
                           + std::to_string ((int) bpm) + " bpm for "
                           + std::string (conversion.what));
            }
        }
    }

    // -----------------------------------------------------------------------
    // The loop runs over exactly the taps that can contribute. An impulse at
    // the outermost sample that should reach the read has to reach it, and one
    // beyond it must not.
    //
    // This is here because dropping one tap off either end of the loop is a
    // change of about -125 dB, which every threshold in this file is too loose
    // to see, and because the one of those two mutations that the suite did
    // catch was caught by 0.04 dB, which is a coin flip on the last bits of
    // libm rather than a test. Asserted structurally, where it is exact.
    // -----------------------------------------------------------------------
    {
        Rng rng (0x6a09e667ull);

        for (int trial = 0; trial < 400; ++trial)
        {
            const double step   = rng.unit() < 0.3 ? 1.0 : rng.between (0.25, 4.0);
            const double offset = rng.between (0.02, 0.98);
            const double scale  = ClipResampler::scaleForStep (kernel, step);

            // A tap contributes while its distance from the read position is
            // inside the kernel's half width, which the table expresses as
            // crossings / scale source samples.
            const double width  = (double) kernel.crossings / scale;
            const int    centre = 4000;
            const int    frames = 8000;

            const int outermostRight = (int) std::floor (offset + width - 1.0e-9);
            const int outermostLeft  = (int) std::ceil  (offset - width + 1.0e-9);

            for (int which = 0; which < 4; ++which)
            {
                const bool inside = which < 2;
                const int  at = which == 0 ? outermostRight
                              : which == 1 ? outermostLeft
                              : which == 2 ? outermostRight + 1
                                           : outermostLeft - 1;

                std::vector<float> impulse ((std::size_t) frames, 0.0f);
                impulse[(std::size_t) (centre + at)] = 1.0f;

                const auto frame = ClipResampler::read (kernel, impulse.data(), impulse.data(),
                                                        frames, (double) centre + offset, step);

                if (inside)
                    check (frame.left != 0.0f,
                           "the tap " + std::to_string (at) + " samples from the read, at step "
                               + std::to_string (step) + " offset " + std::to_string (offset)
                               + ", is inside the kernel and reaches the output");
                else
                    check (frame.left == 0.0f,
                           "the tap " + std::to_string (at) + " samples from the read, at step "
                               + std::to_string (step) + " offset " + std::to_string (offset)
                               + ", is outside the kernel and contributes nothing");
            }
        }
    }

    // -----------------------------------------------------------------------
    // What a read can produce from full scale material, which is not one.
    //
    // Linear interpolation was a convex combination of two samples and so could
    // never leave the input's range. A windowed sinc overshoots, which is the
    // one place this change alters the signal rather than improving it, so the
    // bound is asserted against the quantity it actually is, and the worst case
    // signal is constructed and required to come close to it. An audit found
    // this check asserting 1.5 with a comment naming a quantity that is 2.95.
    // -----------------------------------------------------------------------
    {
        double worstPhaseGain = 0.0;

        for (int phase = 0; phase <= kernel.entriesPerCrossing; ++phase)
            worstPhaseGain = std::max (worstPhaseGain, kernel.phaseGain (phase));

        check (worstPhaseGain > 1.0,
               "a windowed sinc overshoots, so the worst case gain is above one, got "
                   + std::to_string (worstPhaseGain));

        // The signal that attains it: the sign of each tap. Legal audio, full
        // scale, and nothing can read higher.
        const int frames = 4000, centre = 2000;
        const double offset = 0.5, step = 1.0;

        std::vector<float> adversarial ((std::size_t) frames, 0.0f);

        for (int i = -kernel.crossings; i <= kernel.crossings + 1; ++i)
        {
            std::vector<float> probe ((std::size_t) frames, 0.0f);
            probe[(std::size_t) (centre + i)] = 1.0f;

            const auto one = ClipResampler::read (kernel, probe.data(), probe.data(), frames,
                                                  (double) centre + offset, step);
            adversarial[(std::size_t) (centre + i)] = one.left >= 0.0f ? 1.0f : -1.0f;
        }

        const auto peak = ClipResampler::read (kernel, adversarial.data(), adversarial.data(),
                                               frames, (double) centre + offset, step);

        check (std::abs (peak.left) <= worstPhaseGain + 1.0e-4,
               "the worst case signal cannot exceed the worst case gain, got "
                   + std::to_string (peak.left) + " against " + std::to_string (worstPhaseGain));

        check (std::abs (peak.left) > 0.9 * worstPhaseGain,
               "the worst case signal comes close to the worst case gain, so the bound is "
               "the real one rather than a loose one: got " + std::to_string (peak.left)
                   + " against " + std::to_string (worstPhaseGain));

        // And something ordinary, so the figure in the header is checkable: a
        // full scale square wave is the overshoot anybody will actually meet.
        std::vector<float> square ((std::size_t) frames);

        for (int i = 0; i < frames; ++i)
            square[(std::size_t) i] = ((i / 48) % 2) == 0 ? 1.0f : -1.0f;

        double squarePeak = 0.0;

        // Swept across a whole period rather than a couple of samples: the
        // overshoot is largest right at an edge, and a short sweep that misses
        // the edge reads it as nothing. The first version of this check swept
        // two samples and measured 0.10 dB where the real figure is 2.1.
        for (int i = 0; i < 9600; ++i)
        {
            const double position = (double) centre + i * 0.01;
            const auto frame = ClipResampler::read (kernel, square.data(), square.data(),
                                                    frames, position, 1.0);
            squarePeak = std::max (squarePeak, std::abs ((double) frame.left));
        }

        check (squarePeak > 1.25 && squarePeak < 1.32,
               "a full scale square wave reads above full scale, by the 2.1 dB the header "
               "quotes: got " + std::to_string (squarePeak));

        std::printf ("  overshoot: worst case gain %.4f, square wave %.4f (%+.2f dB)\n",
                     worstPhaseGain, squarePeak, 20.0 * std::log10 (squarePeak));
    }

    // -----------------------------------------------------------------------
    // A clip at the device's own rate, trimmed to a sample boundary, reads bit
    // for bit. This is what makes a bounce of an untouched take reproduce the
    // file, and it holds only because phase zero is an exact impulse.
    // -----------------------------------------------------------------------
    {
        Rng rng (0x9d2c5680ull);
        int checked = 0;

        for (int trial = 0; trial < 2000; ++trial)
        {
            const int frames = 400 + rng.upTo (600);

            std::vector<float> left ((std::size_t) frames), right ((std::size_t) frames);

            for (int i = 0; i < frames; ++i)
            {
                left[(std::size_t) i]  = (float) rng.between (-1.0, 1.0);
                right[(std::size_t) i] = (float) rng.between (-1.0, 1.0);
            }

            // Well inside the buffer, so the kernel's whole width has real
            // samples under it and there is nothing for the edge rule to do.
            const int at = kernel.crossings + 2 + rng.upTo (frames - 2 * kernel.crossings - 6);
            const double step = rng.unit() < 0.5 ? 1.0 : rng.between (0.25, 1.0);

            const auto frame = ClipResampler::read (kernel, left.data(), right.data(), frames,
                                                    (double) at, step);

            if (frame.left != left[(std::size_t) at] || frame.right != right[(std::size_t) at])
            {
                check (false, "a read at whole sample " + std::to_string (at)
                           + " at step " + std::to_string (step)
                           + " returned the stored sample exactly");
                break;
            }

            ++checked;
        }

        std::printf ("  bit exact at a sample boundary: %d randomised reads\n", checked);
    }

    // -----------------------------------------------------------------------
    // THE CENTRAL CHECK. The residual against the analytic ideal, over
    // randomised fractional offsets, for every conversion a session here meets
    // and every frequency in the audio band.
    // -----------------------------------------------------------------------
    {
        Rng rng (0x2545f491ull);
        double worst = -1.0e300;
        std::string worstWhere;

        for (const auto& conversion : conversions)
        {
            const double outputNyquist = 0.5 * conversion.sourceRate / conversion.step;
            const double highest = std::min (20000.0, std::min (0.5 * conversion.sourceRate,
                                                                outputNyquist) - 1500.0);

            for (int trial = 0; trial < 12; ++trial)
            {
                const double tone   = rng.between (20.0, highest);
                const double offset = rng.unit();

                const auto pass = sweep (kernel, Read::bandlimited, tone, conversion.sourceRate,
                                         conversion.step, offset);

                if (pass.residualDb > worst)
                {
                    worst = pass.residualDb;
                    worstWhere = std::string (conversion.what) + " at "
                               + std::to_string ((int) tone) + " Hz, offset "
                               + std::to_string (offset);
                }
            }
        }

        check (worst < -100.0,
               "the residual against the ideal stays under -100 dB everywhere, worst "
                   + std::to_string (worst) + " dB for " + worstWhere);

        std::printf ("  residual against the ideal: worst %.2f dB (%s)\n",
                     worst, worstWhere.c_str());
    }

    // -----------------------------------------------------------------------
    // And that measurement pointed at the read it replaced, which it has to
    // notice. A measurement that cannot fail is not evidence, and this one in
    // particular is the only check in the file that the old code fails.
    // -----------------------------------------------------------------------
    {
        double worstLinear = -1.0e300;

        for (const auto& conversion : conversions)
        {
            const double outputNyquist = 0.5 * conversion.sourceRate / conversion.step;
            const double tone = std::min (10000.0, std::min (0.5 * conversion.sourceRate,
                                                             outputNyquist) - 1500.0);

            const auto pass = sweep (kernel, Read::linear, tone, conversion.sourceRate,
                                     conversion.step, 0.5);
            worstLinear = std::max (worstLinear, pass.residualDb);
        }

        check (worstLinear > -25.0,
               "the residual measurement sees linear interpolation, which reads "
                   + std::to_string (worstLinear) + " dB at 10 kHz and would have to be "
                   "under -100 to pass the check above");

        std::printf ("  the same measurement on the old lerp: %.2f dB at 10 kHz\n", worstLinear);
    }

    // -----------------------------------------------------------------------
    // Aliasing when the read runs fast, at the one frequency where it can be
    // seen: the source tone that folds onto exactly 20 kHz, the top of the
    // protected band. Say where an alias lands or the figure means nothing.
    // -----------------------------------------------------------------------
    {
        // The protected band is 0 to 20 kHz, so for each conversion the tone
        // worth testing is the one source frequency whose alias lands on
        // exactly 20 kHz. It is computed rather than written down, because a
        // hardcoded tone and a hardcoded rate drift apart the moment either is
        // edited and the test then measures a frequency that is not at the
        // edge of anything.
        struct Fold
        {
            const char* what;
            double sourceRate;
            double sessionRate;
            double step;
        };

        const Fold folds[] =
        {
            { "an 88.2 kHz file in a 44.1 kHz session",  88200.0, 44100.0, 2.0 },
            { "a 176.4 kHz file in a 44.1 kHz session", 176400.0, 44100.0, 4.0 },
            { "a 96 kHz file in a 48 kHz session",       96000.0, 48000.0, 2.0 },
            { "a 192 kHz file in a 48 kHz session",     192000.0, 48000.0, 4.0 },
            { "a 44.1 kHz clip played 1.2 times fast",   44100.0, 44100.0, 1.2 },
            { "a 48 kHz clip played twice as fast",      48000.0, 48000.0, 2.0 },
        };

        const double protectedTo = 20000.0;

        // The sourced negative, asserted rather than asserted in a comment:
        // **a 48 kHz file in a 44.1 kHz session cannot fold anything into the
        // protected band at all.** The source frequency that would be needed
        // is above what a 48 kHz file can carry, so the two rates this studio
        // most often mixes are the easy case and the tap count is decided by
        // 88.2 kHz material and by clips played fast. If somebody later widens
        // the protected band this check is what tells them that claim has
        // stopped being true.
        {
            const double needed = (1.0 - protectedTo / 44100.0) * 48000.0 / (48000.0 / 44100.0);
            check (needed > 0.5 * 48000.0,
                   "a 48 kHz file in a 44.1 kHz session would need a source tone at "
                       + std::to_string (needed) + " Hz to fold onto 20 kHz, which is above "
                       "its own Nyquist, so it cannot fold into the protected band");
        }

        double worst = -1.0e300;
        std::string worstWhere;

        for (const auto& fold : folds)
        {
            const double outputCycles = 1.0 - protectedTo / fold.sessionRate;
            const double tone = outputCycles * fold.sourceRate / fold.step;

            // Every claim the case rests on, checked rather than trusted: the
            // tone exists in a file at that rate, its output frequency is above
            // the session's Nyquist so a correct read gives silence, and the
            // alias it would otherwise produce lands at the top of the band
            // being protected.
            check (tone < 0.5 * fold.sourceRate,
                   std::string ("the test tone for ") + fold.what + " at "
                       + std::to_string (tone) + " Hz is one a file at that rate can carry");

            check (outputCycles > 0.5 && outputCycles < 1.0,
                   std::string ("the test tone for ") + fold.what + " really does fold, at "
                       + std::to_string (outputCycles) + " cycles per output sample");

            const double landsAt = (1.0 - tone * fold.step / fold.sourceRate) * fold.sessionRate;

            check (std::abs (landsAt - protectedTo) < 1.0,
                   std::string ("the alias for ") + fold.what + " lands on 20 kHz, got "
                       + std::to_string (landsAt) + " Hz");

            const auto pass = sweep (kernel, Read::bandlimited, tone, fold.sourceRate,
                                     fold.step, 0.37, 12000);

            if (pass.levelDb > worst)
            {
                worst = pass.levelDb;
                worstWhere = fold.what;
            }
        }

        check (worst <= -120.0,
               "nothing folds into the protected band above -120 dB, worst "
                   + std::to_string (worst) + " dB for " + worstWhere);

        std::printf ("  worst alias folding onto 20 kHz: %.2f dB (%s)\n", worst, worstWhere.c_str());

        // The same measurement with Smith's time scaling removed, which it has
        // to notice. That one omission is the entire reason the min{Fs,Fs'}
        // rule exists: without it the cutoff stays at the source's Nyquist and
        // everything between the two Nyquists comes through, folded, at full
        // level. Nothing else in this file can see it, because at a step of one
        // or less the scaling is the identity.
        const auto broken = sweep (kernel, Read::unscaled, 28000.0, 96000.0, 2.0, 0.37, 12000);

        check (broken.levelDb > -6.0,
               "the aliasing measurement sees the time scaling removed, which lets a "
               "28 kHz tone fold onto 20 kHz at " + std::to_string (broken.levelDb)
                   + " dB where the scaled read rejects it below -120");

        std::printf ("  the same measurement with the time scaling removed: %.2f dB\n",
                     broken.levelDb);

        // And the honest limit. Past the step the kernel's cutoff follows, the
        // protection stops, and that is documented rather than silent. Asserted
        // so that nobody later reads the clamp as covering everything.
        const double beyond = ClipResampler::scaleForStep (kernel, 16.0);
        check (beyond == 1.0 / ClipResampler::Kernel::maxTrackedStep,
               "past the tracked step the scale is clamped rather than continuing, which is "
               "the documented limit");
    }

    // -----------------------------------------------------------------------
    // DC gain is one at every fractional position, not only at the ones that
    // land on a table entry. A residual here modulates a steady signal.
    // -----------------------------------------------------------------------
    {
        Rng rng (0x1f123bb5ull);
        const int frames = 3000;
        std::vector<float> flat ((std::size_t) frames, 1.0f);

        double worst = 0.0, worstStep = 0.0;

        for (int trial = 0; trial < 4000; ++trial)
        {
            const double step = rng.unit() < 0.25 ? 1.0 : rng.between (0.25, 4.0);
            const double position = rng.between (1000.0, 1500.0);

            const auto frame = ClipResampler::read (kernel, flat.data(), flat.data(), frames,
                                                    position, step);

            if (std::abs ((double) frame.left - 1.0) > worst)
            {
                worst = std::abs ((double) frame.left - 1.0);
                worstStep = step;
            }

            check (frame.left == frame.right,
                   "the two channels of the same buffer read identically");
        }

        check (worst < 1.0e-5,
               "DC gain is one at every fractional position, worst error "
                   + std::to_string (worst) + " at step " + std::to_string (worstStep));

        std::printf ("  DC gain: worst error %.3e over 4000 randomised positions and steps\n", worst);
    }

    // -----------------------------------------------------------------------
    // The read is linear. Asserted rather than assumed from the shape of the
    // code, because the whole of the quality argument above is an argument
    // about a filter and a filter that is not linear is not one.
    // -----------------------------------------------------------------------
    {
        Rng rng (0x27d4eb2full);
        double worstHomogeneity = 0.0, worstSuperposition = 0.0;

        for (int trial = 0; trial < 300; ++trial)
        {
            const int frames = 600;
            std::vector<float> a ((std::size_t) frames), b ((std::size_t) frames),
                               sum ((std::size_t) frames), scaled ((std::size_t) frames);

            const double gain = rng.between (-4.0, 4.0);

            for (int i = 0; i < frames; ++i)
            {
                a[(std::size_t) i] = (float) rng.between (-0.5, 0.5);
                b[(std::size_t) i] = (float) rng.between (-0.5, 0.5);
                sum[(std::size_t) i] = a[(std::size_t) i] + b[(std::size_t) i];
                scaled[(std::size_t) i] = (float) (gain * (double) a[(std::size_t) i]);
            }

            const double step = rng.between (0.25, 4.0);
            const double position = rng.between (200.0, 400.0);

            const double ra = ClipResampler::read (kernel, a.data(), a.data(), frames, position, step).left;
            const double rb = ClipResampler::read (kernel, b.data(), b.data(), frames, position, step).left;
            const double rsum = ClipResampler::read (kernel, sum.data(), sum.data(), frames, position, step).left;
            const double rscaled = ClipResampler::read (kernel, scaled.data(), scaled.data(), frames, position, step).left;

            worstSuperposition = std::max (worstSuperposition, std::abs (rsum - (ra + rb)));
            worstHomogeneity   = std::max (worstHomogeneity, std::abs (rscaled - gain * ra));
        }

        check (worstSuperposition < 1.0e-5,
               "reading the sum of two signals gives the sum of the two reads, worst "
                   + std::to_string (worstSuperposition));
        check (worstHomogeneity < 1.0e-5,
               "reading a scaled signal gives the scaled read, worst "
                   + std::to_string (worstHomogeneity));
    }

    // -----------------------------------------------------------------------
    // The edges. A file is silence either side of itself, and the kernel is 48
    // samples wide, so every read within 48 samples of either end has part of
    // its width outside the buffer. The sanitizers are doing most of the work
    // here, which is why the positions are randomised well past both ends.
    // -----------------------------------------------------------------------
    {
        Rng rng (0x165667b1ull);

        for (int trial = 0; trial < 3000; ++trial)
        {
            const int frames = 1 + rng.upTo (120);

            std::vector<float> left ((std::size_t) frames), right ((std::size_t) frames);

            for (int i = 0; i < frames; ++i)
            {
                left[(std::size_t) i]  = (float) rng.between (-1.0, 1.0);
                right[(std::size_t) i] = (float) rng.between (-1.0, 1.0);
            }

            const double position = rng.between (-400.0, (double) frames + 400.0);
            const double step     = rng.between (0.1, 8.0);

            const auto frame = ClipResampler::read (kernel, left.data(), right.data(), frames,
                                                    position, step);

            check (std::isfinite (frame.left) && std::isfinite (frame.right),
                   "a read at " + std::to_string (position) + " in a buffer of "
                       + std::to_string (frames) + " is finite");

            // Bounded by the kernel's own worst case gain, which is the
            // quantity checked properly above. Here it is only a sanity bound
            // on a short buffer, and it says so: most of the kernel hangs off
            // the end of a buffer of 1 to 120 samples, so this cannot see the
            // real bound and is not the check that does.
            check (std::abs (frame.left) <= 3.0 && std::abs (frame.right) <= 3.0,
                   "a read of full scale material is bounded, got "
                       + std::to_string (frame.left));

            // Entirely outside the buffer is silence, because that is what the
            // file is out there.
            if (position < -(double) kernel.crossings * 8.0 - 4.0)
                check (frame.left == 0.0f && frame.right == 0.0f,
                       "a read entirely before the buffer is silent");
        }
    }

    // -----------------------------------------------------------------------
    // The scale and the tap count as functions of the step, which is what says
    // a clip played fast costs proportionally more arithmetic.
    // -----------------------------------------------------------------------
    {
        check (ClipResampler::scaleForStep (kernel, 0.5) == 1.0,
               "reading slower than one to one needs no extra filtering");
        check (ClipResampler::scaleForStep (kernel, 1.0) == 1.0,
               "reading one to one needs no extra filtering");
        check (std::abs (ClipResampler::scaleForStep (kernel, 2.0) - 0.5) < 1.0e-12,
               "reading twice as fast halves the cutoff");

        check (ClipResampler::halfTapsForStep (kernel, 1.0) == kernel.crossings,
               "a step of one costs the designed tap count");
        check (ClipResampler::halfTapsForStep (kernel, 2.0) == 2 * kernel.crossings,
               "a step of two costs twice the taps");
        check (ClipResampler::halfTapsForStep (kernel, 4.0) == 4 * kernel.crossings,
               "a step of four costs four times the taps");
        check (ClipResampler::halfTapsForStep (kernel, 64.0) == 4 * kernel.crossings,
               "past the tracked step the cost stops growing, because the cutoff stops "
               "coming down");

        // At a step that is not a whole number the tap count has to round up,
        // not down: the last tap has something under it and dropping it leaves
        // the stretched kernel cut off. Every step above is an exact ratio, so
        // rounding the wrong way gives the same answer for all of them and
        // none of them can see this.
        check (ClipResampler::halfTapsForStep (kernel, 48000.0 / 44100.0) == 53,
               "a 48 kHz file in a 44.1 kHz session costs 53 taps a side, rounded up from "
               "52.23, got " + std::to_string (ClipResampler::halfTapsForStep (kernel, 48000.0 / 44100.0)));

        // Degenerate steps the engine should never produce, checked because an
        // audio thread that divides by one of them takes the studio down.
        std::vector<float> flat (400, 1.0f);

        for (double step : { 0.0, -1.0, 1.0e12 })
        {
            const auto frame = ClipResampler::read (kernel, flat.data(), flat.data(), 400,
                                                    200.0, step);
            check (std::isfinite (frame.left), "a step of " + std::to_string (step)
                       + " does not produce an infinity");
        }

        const auto nothing = ClipResampler::read (kernel, nullptr, nullptr, 0, 0.0, 1.0);
        check (nothing.left == 0.0f && nothing.right == 0.0f,
               "a read with no buffer is silence rather than a crash");
    }

    if (failures == 0)
        std::printf ("ClipResamplerTest: all checks passed\n");
    else
        std::printf ("ClipResamplerTest: %d check(s) failed\n", failures);

    return failures == 0 ? 0 : 1;
}
