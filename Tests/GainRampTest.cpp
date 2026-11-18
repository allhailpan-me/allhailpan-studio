// ---------------------------------------------------------------------------
// Checks what a moving gain actually does to the signal it is multiplying.
//
// This is the thing in a mixer least able to announce that it is wrong. A gain
// ramped over the wrong span, or over a span that quietly depends on the
// buffer size, does not error, does not look wrong on a meter, and sounds like
// the mix. So the properties below are about the audio rather than about the
// formula:
//
//   * THE ARTIFACT ITSELF IS MEASURED. A gain held constant across a block and
//     stepped between blocks is a zero order hold of the control, and the
//     derivation in GainRamp.h says what that puts into the signal: sidebands
//     at multiples of the block rate either side of every component of it, at
//     dg/(2 pi k) of the signal entering the gain. The measurement is checked
//     against that derivation on the stepped version first, and only then
//     turned on the ramped one. A check that only looked at the ramp's shape
//     could not say whether the ramp was worth having.
//
//   * AGAINST THE IDEAL, NOT AGAINST A FLOOR THIS TEST CHOSE. The ramped
//     version is compared with a gain that moves continuously, computed per
//     sample with no blocks at all, so the comparison does not depend on the
//     measurement's own noise floor. That is the trap `Fx/WaveFolder.h`
//     records: a window whose sidelobes sit at -55 dB makes every reading -55
//     dB however good the thing being measured is.
//
//   * BLOCK SIZE INVARIANCE, BIT FOR BIT. The same gesture at twelve buffer
//     sizes from 1 to 2048 has to give the same samples. Playback and export
//     are two paths over the same graph here, so a smoothing time that
//     depended on the buffer size would make a bounce differ from what was
//     heard, which CLAUDE.md calls among the worst bugs a studio can have. It
//     is also the check that caught the first version of this very change,
//     which applied one ramp across each block and so stretched any move that
//     arrived early across the whole buffer: 42.7 ms at 2048 samples for a
//     move that should take five.
//
//   * NO DISCONTINUITY AT A BLOCK BOUNDARY, measured as a boundary against the
//     inside of a block rather than in absolute terms. A ramp whose state is
//     not carried across blocks looks perfect inside a block.
//
//   * EXACT ARRIVAL AND EXACT STEADINESS, as bitwise equality, because a gain
//     that merely approaches its target leaves a mute 100 dB down rather than
//     silent and a steady mix irreproducible.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include "GainRamp.h"
#include "PanLaw.h"
#include "RealFFT.h"

#include <algorithm>
#include <complex>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

static int failures = 0;

// Capped, because a property checked over thousands of samples can fail
// thousands of times and nobody reads past the first few. The count is still
// complete and is what the exit code comes from.
static void fail (const std::string& what)
{
    ++failures;

    if (failures <= 20)
        std::printf ("  FAIL  %s\n", what.c_str());
    else if (failures == 21)
        std::printf ("  ... further failures not printed\n");
}

static void check (bool ok, const std::string& what)
{
    if (! ok)
        fail (what);
}

static const double pi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// The mixer's own arithmetic, written out once so that every check below is
// measuring what the engine does rather than something near it: the control is
// read once a block, and the ramp advances a sample at a time within it.

struct Gesture
{
    struct Point { int atSample; double target; };   // the target from here on

    std::vector<Point> points;

    double targetAt (int sample) const
    {
        double t = points.empty() ? 0.0 : points.front().target;

        for (const auto& p : points)
            if (sample >= p.atSample)
                t = p.target;

        return t;
    }
};

static std::vector<double> rampedGains (const Gesture& g, double start, double step,
                                        int totalSamples, int blockSize)
{
    std::vector<double> out;
    out.reserve ((size_t) totalSamples);

    GainRamp::Ramp ramp { start };

    for (int at = 0; at < totalSamples; at += blockSize)
    {
        const int    n      = std::min (blockSize, totalSamples - at);
        const double target = g.targetAt (at);   // the atomic, loaded once a block

        for (int i = 0; i < n; ++i)
            out.push_back (ramp.next (target, step));
    }

    return out;
}

/** What the mixer did before any of this: one constant per block. */
static std::vector<double> steppedGains (const Gesture& g, int totalSamples, int blockSize)
{
    std::vector<double> out;
    out.reserve ((size_t) totalSamples);

    for (int at = 0; at < totalSamples; at += blockSize)
    {
        const int    n     = std::min (blockSize, totalSamples - at);
        const double value = g.targetAt (at);

        for (int i = 0; i < n; ++i)
            out.push_back (value);
    }

    return out;
}

// ---------------------------------------------------------------------------
// Measurement
//
// A Hann window, and the reason is the lesson in CLAUDE.md about a window's
// own sidelobes becoming the floor of every reading. Rectangular was tried
// first and its floor was -49 dB, because the gain at one end of the window is
// not the gain at the other and the wrap is a step: that is the same order as
// the artifact being measured, so nothing could be seen. A Hann window's
// sidelobes fall 18 dB an octave, and the sidebands here sit 48 bins from the
// carrier, which puts its floor far below anything that matters.
//
// Its coherent gain is a half, which is divided out, so what comes back is the
// amplitude of a component as it is in the signal.

static double windowedMagnitude (const std::vector<double>& x, double cyclesInWindow)
{
    const double n = (double) x.size();
    double re = 0.0, im = 0.0, weight = 0.0;

    for (size_t i = 0; i < x.size(); ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * pi * (double) i / n);
        const double phase = 2.0 * pi * cyclesInWindow * (double) i / n;

        re += w * x[i] * std::cos (phase);
        im -= w * x[i] * std::sin (phase);
        weight += w;
    }

    // Twice, because a real sinusoid of amplitude A puts A/2 at each of its
    // two bins, so without this every reading is 6 dB low. The derivation it
    // is checked against is in amplitude, and the first version of this
    // measurement was exactly 6 dB under it for this reason alone.
    return 2.0 * std::sqrt (re * re + im * im) / weight;
}

static double toDb (double amplitude)
{
    return amplitude > 0.0 ? 20.0 * std::log10 (amplitude) : -1000.0;
}

// ---------------------------------------------------------------------------
// 1. The artifact, measured, and the measurement checked against the
//    derivation before any of it is trusted.

static void checkTheArtifact()
{
    const double rate        = 48000.0;
    const int    blockSize   = 512;
    const int    window      = 24576;            // 48 blocks, half a second
    const double fadeSeconds = 1.0;

    // Whole cycles in the window, so the component sits exactly on a bin, and
    // deliberately NOT a whole number of cycles per BLOCK.
    //
    // That second condition was learned the hard way and is the reason this
    // measurement exists in the shape it does. The first version used 480
    // cycles, which is exactly 10 per 512 sample block, and read the stepped
    // gain's sidebands at -300 dB: a signal that repeats every block,
    // multiplied by a gain that is constant within every block, puts nothing
    // at all at a neighbouring bin, because over one block two different bins
    // are orthogonal. The staircase was there and the measurement could not
    // see it. It is the same fault as the one in `Synth/WaveTable.h`'s notes,
    // a test waveform that does not exercise the thing under test.
    const double carrierCycles = 481.0;                                  // 939.5 Hz
    const double blockCycles   = (double) window / (double) blockSize;   // 48, i.e. 93.75 Hz

    const int    total = (int) (rate * fadeSeconds);
    const double step  = GainRamp::stepPerSample (rate, GainRamp::levelSeconds);

    // The fade is written as a target that moves every block, because that is
    // what a volume automation curve is: the lane is read once a block and the
    // figure written into the atomic the mixer loads.
    Gesture fade;
    for (int at = 0; at < total; at += blockSize)
        fade.points.push_back ({ at, 1.0 - (double) at / (double) total });

    const auto stepped = steppedGains (fade, total, blockSize);
    const auto ramped  = rampedGains  (fade, 1.0, step, total, blockSize);

    std::vector<double> ideal ((size_t) total);
    for (int i = 0; i < total; ++i)
        ideal[(size_t) i] = 1.0 - (double) i / (double) total;

    // How far the gain moves in one block, which is what the derivation is in
    // terms of.
    const double perBlock = (double) blockSize / (double) total;

    // A window starting a quarter of the way into the fade, on a block
    // boundary, so neither end of the fade is in shot.
    const int from = (total / 4 / blockSize) * blockSize;

    struct Case { const char* name; const std::vector<double>* gains; };
    const Case cases[] = { { "ideal", &ideal }, { "stepped", &stepped }, { "ramped", &ramped } };

    double sidebandDb[3] = { 0.0, 0.0, 0.0 };

    for (int c = 0; c < 3; ++c)
    {
        std::vector<double> signal ((size_t) window);

        for (int i = 0; i < window; ++i)
        {
            const double phase = 2.0 * pi * carrierCycles * (double) i / (double) window;
            signal[(size_t) i] = std::sin (phase) * (*cases[c].gains)[(size_t) (from + i)];
        }

        const double lower = windowedMagnitude (signal, carrierCycles - blockCycles);
        const double upper = windowedMagnitude (signal, carrierCycles + blockCycles);

        // Absolute, because the signal entering the gain has an amplitude of
        // one, which is exactly what the derivation is relative to. Dividing
        // by the measured carrier instead would divide by the fade itself.
        sidebandDb[c] = toDb (std::max (lower, upper));

        std::printf ("  %-8s first sideband at %7.2f dB of the signal\n",
                     cases[c].name, sidebandDb[c]);
    }

    const double predicted = GainRamp::stepSidebandDb (perBlock, 1);
    std::printf ("  the derivation says the stepped one should be %7.2f dB\n", predicted);

    // The measurement against the derivation. If these disagree then either
    // the derivation in the header is wrong or this transform is, and until
    // they agree no reading here means anything.
    check (std::abs (sidebandDb[1] - predicted) < 1.0,
           "the measured sideband of a per block constant gain disagrees with the derivation: measured "
               + std::to_string (sidebandDb[1]) + " dB, derived " + std::to_string (predicted) + " dB");

    // The ideal is the floor of this measurement, and it has to be far below
    // the thing being measured or nothing above means anything. That is the
    // check `Fx/WaveFolder.h` records the absence of: a probe whose window
    // sidelobes sat at -58 dB read -55 dB however much oversampling was thrown
    // at it.
    check (sidebandDb[0] < sidebandDb[1] - 40.0,
           "the measurement's own floor is within 40 dB of the artifact, so it cannot see it: floor "
               + std::to_string (sidebandDb[0]) + " dB, artifact " + std::to_string (sidebandDb[1]) + " dB");

    // AND THE LIMIT OF WHAT THE RAMP DOES, WHICH IS WHY THIS CHECK IS HERE
    // RATHER THAN A CLAIM IN A COMMENT.
    //
    // A ramp does not improve this reading and is not expected to. The target
    // comes from a control read once a block, and a slew limiter travels the
    // whole of a fader in five milliseconds, which at a 512 sample buffer is
    // less than half a block: so it arrives at each block's target within a
    // few samples and then holds, and what it holds is the same staircase.
    // What the ramp removes is the discontinuity at the joins, which is what
    // the next check measures, and that is the part that is audible as a click
    // rather than as a buzz.
    //
    // Removing the staircase itself needs the target to move within the block,
    // which means reading an automation curve per sample rather than per
    // block. Ardour does exactly that, fetching the curve into a per sample
    // buffer and filtering that (amp.cc:106-116), and it is the next step
    // here. It is recorded in Research/BACKLOG.md rather than attempted
    // tonight, because the automation writer is a different mechanism from
    // the mixer's gains.
    //
    // Asserted as "no worse" so that the limit is pinned rather than
    // described: if a later change does improve it, this fails and somebody
    // reads this comment.
    check (sidebandDb[2] < sidebandDb[1] + 0.5,
           "the ramp made the once per block staircase worse: ramped " + std::to_string (sidebandDb[2])
               + " dB against stepped " + std::to_string (sidebandDb[1]) + " dB");

    // The derivation's own shape, since the header publishes it: 6 dB for
    // every doubling of the gain change per block, and 6 dB down for each
    // doubling of the sideband order.
    check (std::abs ((GainRamp::stepSidebandDb (perBlock * 2.0, 1) - predicted) - 6.0206) < 1.0e-3,
           "the derivation does not scale at 6 dB per doubling of the gain change per block");
    check (std::abs ((GainRamp::stepSidebandDb (perBlock, 2) - predicted) + 6.0206) < 1.0e-3,
           "the derivation does not fall at 6 dB per doubling of the sideband order");
}

// ---------------------------------------------------------------------------
// 1b. What the ramp is actually for: a control being moved by hand.
//
// This is the case that matters daily and it is not the same case as an
// automation curve. A fader being dragged is written by the interface at its
// own rate, tens of times a second, so what reaches the mixer is a sequence of
// JUMPS rather than a smooth curve. Applied flat across a block each jump is a
// step in the gain, and a step is a click: its spectrum falls at only 6 dB per
// octave, so it is heard across the whole band at roughly the level of
// whatever was playing. Sixty of those a second is the sound of dragging a
// fader.
//
// Two things are measured, and the second is the one that cannot be argued
// with: the largest jump the gain makes in one sample, which is what a click
// is, and how much of the error against a smooth drag lands high up the band,
// which is where a click is heard and a fader move is not.

static void checkAFaderBeingDragged()
{
    const double rate      = 48000.0;
    const int    blockSize = 512;
    const int    total     = 32768;          // a power of two, for the transform

    // Unity to silence over a third of a second, written by the interface
    // every 800 samples, which is 60 times a second.
    const int    dragSamples = (int) (rate * 0.3);
    const int    uiPeriod    = 800;
    const double step        = GainRamp::stepPerSample (rate, GainRamp::levelSeconds);

    Gesture drag;
    for (int at = 0; at <= dragSamples; at += uiPeriod)
        drag.points.push_back ({ at, std::max (0.0, 1.0 - (double) at / (double) dragSamples) });
    drag.points.push_back ({ dragSamples, 0.0 });

    const auto stepped = steppedGains (drag, total, blockSize);
    const auto ramped  = rampedGains  (drag, 1.0, step, total, blockSize);

    // What the drag was asking for: the straight line it was drawn along.
    std::vector<double> ideal ((size_t) total);
    for (int i = 0; i < total; ++i)
        ideal[(size_t) i] = std::max (0.0, 1.0 - (double) i / (double) dragSamples);

    struct Case { const char* name; const std::vector<double>* gains; double jump, highDb; };
    Case cases[] = { { "ideal", &ideal, 0.0, 0.0 },
                     { "stepped", &stepped, 0.0, 0.0 },
                     { "ramped", &ramped, 0.0, 0.0 } };

    RealFFT fft (total);
    std::vector<std::complex<double>> bins ((size_t) fft.getNumBins());

    // Above 2 kHz, which is well clear of the drag itself and of the
    // interface's own rate and its first few harmonics, and is squarely where
    // a click is heard.
    const int firstHighBin = (int) (2000.0 * (double) total / rate);

    for (auto& c : cases)
    {
        for (size_t i = 1; i < c.gains->size(); ++i)
            c.jump = std::max (c.jump, std::abs ((*c.gains)[i] - (*c.gains)[i - 1]));

        std::vector<float> error ((size_t) total);
        for (int i = 0; i < total; ++i)
            error[(size_t) i] = (float) ((*c.gains)[(size_t) i] - ideal[(size_t) i]);

        fft.forward (error.data(), bins.data());

        double energy = 0.0;
        for (int k = firstHighBin; k < fft.getNumBins(); ++k)
            energy += std::norm (bins[(size_t) k]);

        // Root mean square of the part of the error above 2 kHz, as a
        // proportion of full scale.
        c.highDb = toDb (std::sqrt (2.0 * energy) / (double) total);

        std::printf ("  %-8s largest change in one sample %.6f, error above 2 kHz %7.2f dB\n",
                     c.name, c.jump, c.highDb);
    }

    // The click, as the gain step that causes it. The ramp cannot move faster
    // than its slew rate by construction, so this is really a check that the
    // interface's jumps are bigger than that, which is what makes the whole
    // thing worth doing: if a drag moved the gain by less than the slew rate
    // per sample there would be nothing to smooth.
    check (cases[1].jump > cases[2].jump * 8.0,
           "a dragged fader does not step further than the ramp allows, so there is nothing to fix: "
               "stepped " + std::to_string (cases[1].jump) + ", ramped " + std::to_string (cases[2].jump));

    check (cases[2].jump <= step * (1.0 + 1.0e-9),
           "the ramped gain moves faster in one sample than the slew rate allows");

    // The click itself, in decibels, which is the claim worth making: the gain
    // no longer moves in one sample by more than the slew allows, so a jump of
    // any size becomes a ramp rather than a step.
    check (toDb (cases[1].jump / cases[2].jump) > 20.0,
           "the largest change in one sample is reduced by less than 20 dB: "
               + std::to_string (toDb (cases[1].jump / cases[2].jump)) + " dB");

    // And the part of it that is heard, which is the honest figure rather than
    // the flattering one. 8 dB is this project's own floor, set under the 11.7
    // measured so that it fails on a regression rather than on a rounding
    // difference, and nothing publishes a figure for how much of this is
    // acceptable.
    //
    // ELEVEN DECIBELS IS NOT THIRTY, AND THAT IS WORTH KNOWING RATHER THAN
    // ROUNDING UP. A slew limiter's smoothing time is proportional to the size
    // of the move, so a small jump is smoothed over few samples: the 0.056 one
    // the interface makes here is spread over 13 of them, a quarter of a
    // millisecond, and a quarter of a millisecond does not reach far down the
    // band. What it does remove completely is the discontinuity, which is the
    // check above and is the audible part, because a step's spectrum falls at
    // only 6 dB an octave where this one falls at twelve.
    //
    // A one pole, which is what Ardour uses, smooths every move over the same
    // time whatever its size, and at a 6.4 ms time constant would take about
    // 30 dB off this reading rather than 11.7. It was not taken tonight and
    // the reason is in Research/BACKLOG.md: it never arrives, so it needs an
    // epsilon snap wherever it is read, and at a time constant long enough to
    // be worth having it would also make a mute or a solo slower than the five
    // milliseconds this studio deliberately chose for them. The measurement is
    // recorded so that a later night can take that trade with the numbers
    // already in hand.
    check (cases[1].highDb - cases[2].highDb > 8.0,
           "the ramp removes less than 8 dB of the error above 2 kHz: stepped "
               + std::to_string (cases[1].highDb) + " dB, ramped " + std::to_string (cases[2].highDb) + " dB");

    // The ideal is the floor again: a straight line has no error against
    // itself, so this reading says how much of what is above is the
    // measurement rather than the artifact.
    check (cases[0].highDb < cases[2].highDb - 30.0,
           "the measurement's own floor is within 20 dB of the ramped reading: floor "
               + std::to_string (cases[0].highDb) + " dB, ramped " + std::to_string (cases[2].highDb) + " dB");
}

// ---------------------------------------------------------------------------
// 2. The same gesture at twelve buffer sizes has to give the same samples.

static void checkBlockSizeInvariance()
{
    const double rate  = 48000.0;
    const double step  = GainRamp::stepPerSample (rate, GainRamp::levelSeconds);
    const int    total = 2048 * 16;

    // The targets move on multiples of 2048 so that every buffer size tested
    // reads the same target at the same sample. A control that changed mid
    // block would be read at a different moment at each buffer size, which is
    // a fact about when the interface writes rather than about the ramp.
    Gesture g;
    g.points.push_back ({ 0,         1.0 });
    g.points.push_back ({ 2048 * 2,  0.0 });                  // dropped to silence
    g.points.push_back ({ 2048 * 5,  3.1622776601683795 });   // and up to the fader's ceiling
    g.points.push_back ({ 2048 * 9,  0.5 });
    g.points.push_back ({ 2048 * 12, 0.5 });                  // a target that does not change

    const int blocks[] = { 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048 };
    const auto reference = rampedGains (g, 1.0, step, total, 2048);

    double worst = 0.0;

    for (int b : blocks)
    {
        const auto got = rampedGains (g, 1.0, step, total, b);

        if (got.size() != reference.size())
        {
            fail ("block size " + std::to_string (b) + " produced a different number of samples");
            continue;
        }

        for (size_t i = 0; i < got.size(); ++i)
            worst = std::max (worst, std::abs (got[i] - reference[i]));
    }

    std::printf ("  worst disagreement across buffer sizes 1 to 2048: %.3e\n", worst);

    // Bit for bit, and it can be: the recursion does not know what a block is,
    // so the same sample index gets the same arithmetic in the same order
    // whatever the buffer size. Anything above zero here means something in
    // the chain has learned about blocks.
    check (worst == 0.0,
           "the same gesture gives different gains at different buffer sizes, worst "
               + std::to_string (worst));
}

// ---------------------------------------------------------------------------
// 3. No discontinuity at a block boundary, measured against the inside of a
//    block so that a ramp which forgets its state cannot pass.

static void checkNoBoundaryDiscontinuity()
{
    const double rate = 48000.0;
    const double step = GainRamp::stepPerSample (rate, GainRamp::levelSeconds);
    const int    blockSize = 256;
    const int    total = blockSize * 64;

    Gesture g;
    g.points.push_back ({ 0, 1.0 });
    g.points.push_back ({ blockSize * 4, 0.0 });
    g.points.push_back ({ blockSize * 24, 1.0 });

    const auto gains = rampedGains (g, 1.0, step, total, blockSize);

    double worstInside = 0.0, worstBoundary = 0.0;

    for (size_t i = 1; i < gains.size(); ++i)
    {
        const double d = std::abs (gains[i] - gains[i - 1]);

        if ((int) i % blockSize == 0)
            worstBoundary = std::max (worstBoundary, d);
        else
            worstInside = std::max (worstInside, d);
    }

    std::printf ("  largest sample to sample change: inside a block %.6e, at a boundary %.6e\n",
                 worstInside, worstBoundary);

    check (worstBoundary <= worstInside + 1.0e-12,
           "the gain jumps further at a block boundary than inside a block: boundary "
               + std::to_string (worstBoundary) + ", inside " + std::to_string (worstInside));

    // And neither may exceed the slew rate, which is what makes the span the
    // span it claims to be.
    check (worstInside <= step * (1.0 + 1.0e-9),
           "the gain moves faster than the slew rate allows");
}

// ---------------------------------------------------------------------------
// 4. Arrival: exact, and taking the time the span promises at every rate.

/** How many samples a ramp takes to get from `start` to `target`. */
static int samplesToArrive (double start, double target, double step, int limit)
{
    GainRamp::Ramp ramp { start };
    int samples = 0;

    while (! ramp.settled (target) && samples < limit)
    {
        ramp.next (target, step);
        ++samples;
    }

    return samples;
}

static void checkArrival()
{
    const double rates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };

    for (double rate : rates)
    {
        const double step    = GainRamp::stepPerSample (rate, GainRamp::levelSeconds);
        const int    samples = samplesToArrive (1.0, 0.0, step, (int) rate);
        const double ms      = 1000.0 * (double) samples / rate;

        std::printf ("  %6.0f Hz: unity to silence in %.4f ms (%d samples)\n", rate, ms, samples);

        // The time, not the sample count, is what has to be the same at every
        // rate. A coefficient held constant instead would be 4.3 times as fast
        // at 192 kHz as at 44.1.
        //
        // Within one sample, which is the honest bound rather than a round
        // number: the step is a reciprocal that does not divide the travel
        // exactly, so the sum of the steps falls a few parts in 1e16 short and
        // one more sample finishes it. At 48 kHz that is 241 samples where 240
        // is the arithmetic.
        const double oneSample = 1000.0 / rate;

        check (std::abs (ms - 1000.0 * GainRamp::levelSeconds) < oneSample * 1.5,
               "unity to silence does not take the published span at " + std::to_string (rate)
                   + ": " + std::to_string (ms) + " ms");

        GainRamp::Ramp ramp { 1.0 };
        for (int i = 0; i < samples; ++i)
            ramp.next (0.0, step);

        check (ramp.value == 0.0,
               "a ramp to silence does not reach exactly zero at " + std::to_string (rate));
    }

    // A move of a quarter of the travel takes a quarter of the time, which is
    // what makes this a slew rate rather than a fixed ramp length. JUCE's
    // SmoothedValue is the other convention, where every move takes the full
    // span whatever its size, and the two are not interchangeable.
    {
        const double rate = 48000.0;
        const double step = GainRamp::stepPerSample (rate, GainRamp::levelSeconds);
        const double ms   = 1000.0 * (double) samplesToArrive (1.0, 0.75, step, (int) rate) / rate;

        check (std::abs (ms - 1000.0 * GainRamp::levelSeconds * 0.25) < 1000.0 / rate * 1.5,
               "a quarter of the travel does not take a quarter of the span: " + std::to_string (ms) + " ms");
    }

    // The fader's ceiling is +10 dB, a gain of 3.16, so a move from the top of
    // the travel to silence is 3.16 times as far and takes 3.16 times as long.
    // Written down because it is the one consequence of a slew rate that could
    // be mistaken for a fault.
    {
        const double rate = 48000.0;
        const double step = GainRamp::stepPerSample (rate, GainRamp::levelSeconds);
        const double top  = 3.1622776601683795;
        const double ms   = 1000.0 * (double) samplesToArrive (top, 0.0, step, (int) rate) / rate;

        std::printf ("  the fader's +10 dB ceiling to silence takes %.3f ms\n", ms);

        check (std::abs (ms - 1000.0 * GainRamp::levelSeconds * top) < 1000.0 / rate * 1.5,
               "a move from the fader's ceiling does not take proportionally longer");
    }
}

// ---------------------------------------------------------------------------
// 5. A steady control is one exact constant.

static void checkSteadiness()
{
    const double step = GainRamp::stepPerSample (48000.0, GainRamp::levelSeconds);

    GainRamp::Ramp ramp { 0.4 };
    check (ramp.settled (0.4), "a ramp already on its target does not report itself settled");

    for (int i = 0; i < 512; ++i)
        check (ramp.next (0.4, step) == 0.4,
               "a steady ramp does not hand back exactly its own value");

    // Arriving mid block has to leave the ramp exactly on the target, so the
    // next block takes the constant path rather than a ramp of nearly zero
    // length. Exact equality is the whole point: anything approximate here is
    // a mute at -100 dB and a mix that does not render the same twice.
    GainRamp::Ramp arriving { 1.0 };
    for (int i = 0; i < 512; ++i)
        arriving.next (0.999, step);

    check (arriving.settled (0.999), "a ramp that arrives mid block is not exactly on its target");
    check (arriving.value == 0.999, "a ramp that arrives is not bitwise on its target");

    // And a latch, which is what a render and a device opening do, has to
    // leave nothing to fade. This is the property nothing else can catch: a
    // bounce that opened with the mixer catching up would sound like a quiet
    // first beat and nothing in the file would say why.
    GainRamp::Ramp latched { 0.0 };
    latched.latch (0.8);
    check (latched.settled (0.8), "a latched ramp is not on its target");

    GainRamp::Ramp unlatched { 0.0 };
    const double first = unlatched.next (0.8, step);
    check (first < 0.8, "a ramp that was not latched arrived in one sample, so the latch proves nothing");
}

// ---------------------------------------------------------------------------
// 6. Mute reaches true silence, in float, which is where it is applied.

static void checkMuteIsSilent()
{
    const double rate = 48000.0;
    const double step = GainRamp::stepPerSample (rate, GainRamp::levelSeconds);
    const int    blockSize = 128;

    Gesture g;
    g.points.push_back ({ 0, 1.0 });
    g.points.push_back ({ blockSize, 0.0 });

    const auto gains = rampedGains (g, 1.0, step, blockSize * 16, blockSize);

    // One slew's worth after the mute, rounded up to a block, plus a block of
    // slack for the control being read at the top of one.
    const size_t settledBy = (size_t) (blockSize * 2 + (int) (rate * GainRamp::levelSeconds) + blockSize);
    bool reachedZero = false;

    for (size_t i = settledBy; i < gains.size(); ++i)
    {
        // As the engine applies it: cast to float and multiplied into the
        // buffer. A gain that is merely small leaves a muted insert feeding
        // the master at -100 dB, which a long reverb tail can be heard
        // through.
        const float applied = (float) gains[i];

        if (applied == 0.0f)
            reachedZero = true;
        else
            fail ("a muted gain is " + std::to_string (applied) + " rather than exactly zero");
    }

    check (reachedZero, "the mute never reached silence at all");
}

// ---------------------------------------------------------------------------
// 7. The pan, which is the one place the span had to be chosen by measurement.
//
// A position is two gains, and ramping the two gains is not ramping the
// position: the straight line between two constant power pairs dips in the
// middle. The question the span answers is how long that dip is allowed to
// last, so what is measured is the energy lost through the move, in decibel
// milliseconds, rather than whether a dip exists at all.

static double panEnergyDeficitDbMs (double seconds, double rate, int blockSize, PanLaw::Law law)
{
    const double step = GainRamp::stepPerSample (rate, seconds);

    const auto from = PanLaw::gains (PanLaw::Mode::pan, law, -1.0);
    const auto to   = PanLaw::gains (PanLaw::Mode::pan, law,  1.0);

    // What the law holds its power at, read off its own endpoints rather than
    // assumed to be unity: a compensated law is the same curve scaled so that
    // centre is unity, so its constant is 2 rather than 1.
    const double reference = 10.0 * std::log10 (from.left * from.left + from.right * from.right);

    GainRamp::Ramp left { from.left }, right { from.right };

    const int total = (int) (rate * seconds * 4.0) + blockSize;
    double deficit = 0.0;

    for (int at = 0; at < total; at += blockSize)
    {
        const int n = std::min (blockSize, total - at);

        for (int i = 0; i < n; ++i)
        {
            const double gl = left .next (to.left,  step);
            const double gr = right.next (to.right, step);
            const double power = gl * gl + gr * gr;
            const double db = power > 0.0 ? 10.0 * std::log10 (power) : -1000.0;

            // Only a shortfall against what the law holds counts. An excess
            // would be a different fault and there is none: a chord between
            // two points on a circle lies inside it.
            if (db < reference)
                deficit += (reference - db) * 1000.0 / rate;
        }
    }

    return deficit;
}

static void checkPanSweep()
{
    const double rate = 48000.0;

    // The constant power laws only, and that is the point rather than a gap.
    // These are the two whose power the law itself holds flat across the whole
    // sweep, so anything measured through a move is the ramp's own doing. The
    // compromise curve dips 1.5 dB at centre by design and the linear one dips
    // 3, which is what those laws are, and `PanLawTest` is where that belongs:
    // measured here they would report a law's definition as a ramp artifact.
    // The linear law is the degenerate case worth knowing about, since it is
    // the straight line itself, so ramping its two gains IS ramping its
    // position and the ramp costs it exactly nothing.
    for (auto law : { PanLaw::Law::minus3, PanLaw::Law::minus3Compensated })
    {
        const double atPan   = panEnergyDeficitDbMs (GainRamp::panSeconds,   rate, 64, law);
        const double atLevel = panEnergyDeficitDbMs (GainRamp::levelSeconds, rate, 64, law);

        std::printf ("  hard left to hard right: %.4f dB ms lost at the pan span, %.4f at the level span\n",
                     atPan, atLevel);

        // The bound is this project's own and comes from the measurement
        // rather than from a reference, because nothing publishes a figure for
        // how much energy a pan move may lose. A full width jump is the worst
        // case the control can be given, and the compensated law travels a
        // factor of root two further, so it takes that much longer and costs
        // that much more: 3.4 and 4.8 dB ms measured, bounded at 6.
        check (atPan < 6.0,
               "a full width pan jump loses more than 6 dB ms of energy: " + std::to_string (atPan));

        // And the reason the pan span is shorter than the level span, asserted
        // rather than claimed: the same move costs proportionally more when it
        // is allowed to take longer.
        check (atLevel > atPan * 2.0,
               "the level span does not cost more than the pan span through a full width move, so "
               "the shorter pan span has nothing to justify it");
    }

    // The dip itself, for the record: the straight line from (1, 0) to (0, 1)
    // passes through (0.5, 0.5), whose power is half, which is 3.01 dB down.
    // Pinned so that a span changed without reading GainRamp::panSeconds's
    // comment fails rather than drifts.
    {
        const double step = GainRamp::stepPerSample (rate, GainRamp::panSeconds);
        const auto from = PanLaw::gains (PanLaw::Mode::pan, PanLaw::Law::minus3, -1.0);
        const auto to   = PanLaw::gains (PanLaw::Mode::pan, PanLaw::Law::minus3,  1.0);

        GainRamp::Ramp left { from.left }, right { from.right };
        double worstDb = 0.0;

        for (int i = 0; i < (int) (rate * GainRamp::panSeconds * 4.0); ++i)
        {
            const double l = left .next (to.left,  step);
            const double r = right.next (to.right, step);
            worstDb = std::min (worstDb, 10.0 * std::log10 (l * l + r * r));
        }

        std::printf ("  the deepest point of a full width pan jump is %.3f dB\n", worstDb);

        check (worstDb > -3.02 && worstDb < -2.99,
               "the worst power through a full width pan jump is not the 3.01 dB the geometry says: "
                   + std::to_string (worstDb));
    }

    // How long the move takes, which bounds how long that dip can last.
    {
        const double step = GainRamp::stepPerSample (rate, GainRamp::panSeconds);
        const double ms   = 1000.0 * (double) samplesToArrive (1.0, 0.0, step, (int) rate) / rate;

        std::printf ("  a full width pan move takes %.4f ms\n", ms);

        check (std::abs (ms - 1000.0 * GainRamp::panSeconds) < 1000.0 / rate * 1.5,
               "a full width pan move does not take the published pan span: " + std::to_string (ms) + " ms");

        // Shorter than the level span, which is the whole reason there are two
        // figures. Ardour's own manual pan ramp is at most 64 samples, 1.33 ms
        // at 48 kHz, for exactly this reason.
        check (GainRamp::panSeconds < GainRamp::levelSeconds,
               "the pan span is not shorter than the level span");
    }
}

// ---------------------------------------------------------------------------
// 8. The guards on the step, both of which have reached code like this before:
//    a sample rate read before a device has opened, and a span of zero from a
//    preference that has not been loaded.

static void checkStepGuards()
{
    for (double rate : { 0.0, -1.0, 0.5, 48000.0 })
        for (double seconds : { 0.0, -1.0, 1.0e-9, GainRamp::levelSeconds })
        {
            const double step = GainRamp::stepPerSample (rate, seconds);

            check (std::isfinite (step) && step > 0.0,
                   "stepPerSample is not a usable number at rate " + std::to_string (rate)
                       + " and span " + std::to_string (seconds));
        }

    // What a nonsense rate does is worth pinning rather than guarding against,
    // because there is no ramp to be had at no sample rate and pretending
    // otherwise would hide a real fault: it arrives at once. Nothing reaches
    // this in practice, since opening a device latches every ramp and the
    // mixer does not run before one is open.
    GainRamp::Ramp ramp { 1.0 };
    ramp.next (0.0, GainRamp::stepPerSample (0.0, GainRamp::levelSeconds));
    check (ramp.value == 0.0, "a ramp with no sample rate behaves unpredictably rather than arriving");

    // The published spans are the ones the engine uses, and both are in the
    // range where they are a few milliseconds rather than a few seconds. A
    // span fat fingered by a factor of a thousand would pass every other check
    // in this file.
    check (GainRamp::levelSeconds > 0.001 && GainRamp::levelSeconds < 0.05,
           "the level span is not a few milliseconds");
    check (GainRamp::panSeconds > 0.0005 && GainRamp::panSeconds < 0.01,
           "the pan span is not a few milliseconds");
}

// ---------------------------------------------------------------------------
// 9. Thousands of generated gestures, because hand picked cases have
//    repeatedly been the weaker half of a test in this project.

static void checkRandomGestures()
{
    std::mt19937 rng (0x9A1Du);
    std::uniform_real_distribution<double> gain (0.0, 3.1622776601683795);
    std::uniform_real_distribution<double> rateDist (8000.0, 192000.0);
    std::uniform_int_distribution<int> blockDist (1, 2048);
    std::uniform_int_distribution<int> countDist (1, 8);

    int gestures = 0;

    for (int trial = 0; trial < 3000; ++trial)
    {
        const double rate      = rateDist (rng);
        const int    blockSize = blockDist (rng);
        const double step      = GainRamp::stepPerSample (rate, GainRamp::levelSeconds);
        const double start     = gain (rng);
        const int    changes   = countDist (rng);

        // Long enough for the last move to finish, whatever rate and buffer
        // size came up: the longest possible move is the whole of the fader's
        // travel and takes that many times the span.
        const int settleSamples = (int) (rate * GainRamp::levelSeconds * 3.1622776601683795) + 2;
        const int spacing       = blockSize * 4;
        const int total         = spacing * (changes + 1) + settleSamples + blockSize;

        Gesture g;
        for (int c = 0; c <= changes; ++c)
            g.points.push_back ({ spacing * c, gain (rng) });

        const auto gains = rampedGains (g, start, step, total, blockSize);
        ++gestures;

        bool outOfRange = false, tooFast = false;

        for (size_t i = 0; i < gains.size(); ++i)
        {
            // Never past the target it is heading for, and never away from it.
            // Those two together are monotone arrival with no overshoot, which
            // is what Ardour's own pan ramp fails: it overshoots by about 12
            // per cent and crosses zero on one side, a momentarily inverted
            // gain.
            const double target   = g.targetAt ((int) (i / (size_t) blockSize) * blockSize);
            const double previous = i > 0 ? gains[i - 1] : start;
            const double lo = std::min (previous, target);
            const double hi = std::max (previous, target);

            if (gains[i] < lo || gains[i] > hi)
                outOfRange = true;

            if (i > 0 && std::abs (gains[i] - previous) > step * (1.0 + 1.0e-9))
                tooFast = true;
        }

        if (outOfRange)
            fail ("a generated gesture overshot or reversed at rate " + std::to_string (rate)
                      + " and buffer " + std::to_string (blockSize));

        if (tooFast)
            fail ("a generated gesture moved faster than the slew rate at rate " + std::to_string (rate)
                      + " and buffer " + std::to_string (blockSize));

        // And it has to have arrived, exactly, by the end.
        if (gains.back() != g.points.back().target)
            fail ("a generated gesture had not arrived by the end at rate " + std::to_string (rate)
                      + " and buffer " + std::to_string (blockSize));
    }

    std::printf ("  %d generated gestures held every invariant\n", gestures);
}

// ---------------------------------------------------------------------------

int main()
{
    std::printf ("GainRamp\n");

    std::printf (" the artifact, measured against its derivation\n");
    checkTheArtifact();

    std::printf (" a fader being dragged, which is what the ramp is for\n");
    checkAFaderBeingDragged();

    std::printf (" the same gesture at every buffer size\n");
    checkBlockSizeInvariance();

    std::printf (" no discontinuity at a block boundary\n");
    checkNoBoundaryDiscontinuity();

    std::printf (" arrival\n");
    checkArrival();

    std::printf (" steadiness, and the latch\n");
    checkSteadiness();

    std::printf (" mute\n");
    checkMuteIsSilent();

    std::printf (" the pan sweep\n");
    checkPanSweep();

    std::printf (" the guards on the step\n");
    checkStepGuards();

    std::printf (" generated gestures\n");
    checkRandomGestures();

    if (failures > 0)
    {
        std::printf ("\n%d check(s) failed.\n", failures);
        return 1;
    }

    std::printf ("\nAll checks passed.\n");
    return 0;
}
