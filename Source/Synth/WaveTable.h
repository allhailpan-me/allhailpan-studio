#pragma once
#include "RealFFT.h"

#include <cmath>
#include <cstddef>
#include <vector>

//==============================================================================
/** Wavetables: a bank of waveforms you sweep through, band limited so that
    sweeping them cannot alias.

    This is what takes the instrument past four fixed shapes. A wavetable is a
    row of waveforms, and the oscillator reads somewhere between two of them,
    so moving along the row is a continuous change of timbre that can be
    played, drawn in an envelope, or driven from an LFO. Everything from a
    glassy bell to a screaming digital edge is one position control.

    The whole problem is aliasing. A waveform with harmonics above half the
    sample rate does not simply lose them: they fold back down and land on
    frequencies that are not multiples of the note. Play a chromatic run and
    those partials walk the other way, which is the shimmer that gives a cheap
    soft synth away, and nothing downstream can take it out because by then it
    is inside the audible band.

    PolyBLEP solves this for a saw or a pulse, by knowing where the
    discontinuity is. A wavetable has no discontinuity to find: it is an
    arbitrary waveform. So the band limiting has to be built into the table,
    which means storing the same waveform several times over, each with fewer
    harmonics, and reading the one that is safe at the note being played. That
    is the mipmap below.

    Source for the approach: Nigel Redmon's wavetable oscillator series at
    earlevel.com, which sets out the one subtable per octave scheme, 2048
    samples per table and a double precision phase accumulator.
    https://www.earlevel.com/main/2012/05/09/a-wavetable-oscillator-part-3/

    Four things here are deliberately not what that article does, and each is
    worth saying why.

    **Four levels per octave rather than one.** The article notes that
    switching tables makes an audible tick, worst in the high range, and
    accepts it. The tick is there because one table per octave means the
    neighbouring table has half the harmonics, so crossing the boundary
    removes half the harmonic content in one sample. At four levels per octave
    a neighbour differs by a factor of 2^(1/4), so the step is about sixteen
    percent of the top harmonics, which are the quietest ones in almost any
    waveform.

    **Each level is band limited for the top of its own range, not the
    bottom.** This is what makes the crossfade below safe. If a level were
    limited for the lowest note that uses it, then the highest note using it
    would be past the limit and aliasing. Limiting for the top means the level
    chosen for a note always has fewer harmonics than that note can carry, and
    so does the level above it, so the two can be mixed in any proportion
    without any part of the mix being unsafe. The cost is that at the bottom
    of a level's range the top harmonic sits at about 84 percent of Nyquist
    rather than right at it, which is not something anybody can hear.

    **Table length shrinks with the harmonic count**, but only down to a
    point. A level holding 64 harmonics carries no information that 2048
    samples could hold and 128 could not, so storing it in 2048 is fifteen
    sixteenths waste. Levels are therefore the shortest power of two that
    still suits their harmonic count, which is what makes a thirty seven level
    mipmap affordable at all.

    **Sixteen times longer than the sampling theorem asks, and read with a
    cubic.** This is the part that was measured rather than chosen, because
    the first version of this file got it wrong in a way that is worth
    recording. A table exactly twice its harmonic count is enough to describe
    the waveform exactly, and it is nowhere near enough to read it. Reading
    lands between stored samples, and the interpolation error is itself a
    distortion that sits in the audible band and is not a harmonic of
    anything. Built at twice the harmonic count and read linearly, which is
    what the obvious reading of the sampling theorem gives, this oscillator
    measured about 28 dB below its own fundamental: as bad as the naive
    sawtooth the whole file exists to avoid, and it passed every check about
    harmonic counts because the harmonic counts were right.

    The error against an exact additive reference, worst case over a range of
    read rates, measured for this waveform:

        oversampling   linear    cubic     6 point
        2x             -37.8 dB  -42.2 dB  -45.1 dB
        4x             -49.3 dB  -62.5 dB  -75.4 dB
        8x             -60.8 dB  -82.7 dB  -109.6 dB
        16x            -73.1 dB  -102.3 dB -144.9 dB

    Sixteen times with a cubic is the corner worth taking: it holds every
    level at 89 dB down or better across the whole mipmap, where 6 point
    interpolation costs twice the arithmetic per sample for quality below the
    noise floor of any converter, and where going wider instead of higher
    order costs four times the memory for the same result.
    Tests/WaveTableProbe.cpp is that measurement, kept so the choice can be
    rechecked rather than taken on faith.

    The phase accumulator is double precision. At single precision the phase
    of a low note drifts audibly over a long held drone, which is exactly the
    material this instrument is for.

    No JUCE, so the tests can reach it, and so the one thing that has to be
    right here can be asserted rather than hoped for: `WaveTableTest` plays
    every table across the keyboard, takes the spectrum of what comes out, and
    requires that every significant partial sits on a multiple of the note.
    That is the direct statement of "it does not alias", and it is checked
    rather than argued.
*/
namespace Synth
{

/** The harmonic content of one waveform in a table.

    Given as harmonics rather than as samples because band limiting is a
    statement about harmonics: a level is built by keeping the first N of
    these and discarding the rest, which is exact, where filtering a sampled
    waveform would be approximate and would smear its phase.

    `amplitude[i]` is harmonic i + 1, so `amplitude[0]` is the fundamental.
    `phase` may be left empty, which means every harmonic starts at a cosine.
    Phase is inaudible on its own for a steady tone, but it decides the peak
    height of the waveform, and so how hard it hits a filter or a distortion,
    which is audible.
*/
struct Spectrum
{
    std::vector<float> amplitude;
    std::vector<float> phase;        /**< radians, empty for all zero */
};

//==============================================================================
class WaveTable
{
public:
    /** How much longer a level is than the sampling theorem asks. Measured,
        not chosen: see the note above. */
    static constexpr int oversampling = 16;

    /** No level is shorter than this however few harmonics it holds. The
        levels near the top of the keyboard hold a handful of harmonics and
        would otherwise be thirty two samples long, where the interpolation
        error stops depending on the harmonic count and starts depending on
        having almost no samples to work with. Half a kilobyte each is nothing
        against what it buys. */
    static constexpr int shortestLevel = 512;

    /** The richest level's length, and the harmonic count that follows from
        it at this oversampling.

        431 harmonics is full brightness down to 55 hertz at a 48 kHz sample
        rate, and a 30 hertz note still reaches 13 kHz, which is past where
        its own harmonics carry anything. Going richer would double the memory
        of every table to add harmonics above the top of hearing on notes
        below the bottom of a bass guitar. */
    static constexpr int longestLevel = 8192;
    static constexpr int mostHarmonics = longestLevel / oversampling;

    /** Levels per octave. One is what makes a table switch tick; four puts
        the step down to the top sixteen percent of the harmonics. */
    static constexpr int levelsPerOctave = 4;

    /** Down to a single harmonic, which is a sine, which is safe at any
        frequency a sample rate can represent. */
    static constexpr int numLevels = levelsPerOctave * 9 + 1;

    //==========================================================================
    /** How many harmonics level `level` keeps.

        The `level + 1` is the whole safety argument: a level is limited for
        the top of its range rather than the bottom, so that the level chosen
        for a note, and the level above it, are both safe for that note and
        can be mixed in any proportion.
    */
    static int harmonicsAtLevel (int level) noexcept
    {
        if (level < 0)
            level = 0;

        const double kept = (double) mostHarmonics
                          * std::pow (2.0, -(double) (level + 1) / (double) levelsPerOctave);

        const int rounded = (int) kept;
        return rounded < 1 ? 1 : rounded;
    }

    /** The shortest power of two that suits that level's harmonic count.

        `oversampling` times the harmonic count, not twice it. Twice is what
        describes the waveform; sixteen times is what lets it be read between
        samples without the interpolation becoming audible. The note above has
        the measurement that settled the number.
    */
    static int lengthAtLevel (int level) noexcept
    {
        const int needed = oversampling * harmonicsAtLevel (level);

        int length = shortestLevel;

        while (length < needed && length < longestLevel)
            length *= 2;

        return length;
    }

    /** Which level a note of this frequency should read, as a real number so
        that the fraction can crossfade.

        Zero at the bottom of the range and rising by `levelsPerOctave` for
        every octave up. Below the point where the richest level is already
        safe it stays at zero, because there is nothing richer to offer.
    */
    static double levelFor (double frequency, double sampleRate) noexcept
    {
        if (! (frequency > 0.0) || ! (sampleRate > 0.0))
            return 0.0;

        const double nyquist = sampleRate * 0.5;

        // How far above the richest level's safe frequency this note is.
        const double ratio = (double) mostHarmonics * frequency / nyquist;

        if (ratio <= 1.0)
            return 0.0;

        const double level = (double) levelsPerOctave * std::log2 (ratio);

        return level > (double) (numLevels - 1) ? (double) (numLevels - 1) : level;
    }

    //==========================================================================
    /** Builds the whole mipmap from one spectrum per frame.

        Allocates, takes a transform per level per frame, and is not to be
        called from the audio thread. Building the ten tables this instrument
        ships with is a few hundred transforms of at most 2048 points, which
        is milliseconds at startup, and is why the instrument needs no asset
        files and nothing to download.
    */
    void build (const std::vector<Spectrum>& frames)
    {
        frameCount = (int) frames.size();
        levels.clear();

        if (frameCount <= 0)
            return;

        levels.resize ((std::size_t) numLevels);

        for (int level = 0; level < numLevels; ++level)
        {
            const int length = lengthAtLevel (level);
            const int keep = harmonicsAtLevel (level);

            RealFFT fft (length);

            auto& store = levels[(std::size_t) level];
            store.length = length;

            // Three guard samples per frame: the last sample of the cycle
            // placed before the start, and the first two placed after the
            // end. A cubic needs the sample before the one it is between and
            // the two after it, so with the guards in place the read is four
            // contiguous loads with no wrapping, no modulo and no branch. The
            // audio thread does this a few hundred million times a minute
            // with unison running; the cost belongs here instead.
            store.samples.assign ((std::size_t) (frameCount * (length + 3)), 0.0f);

            std::vector<std::complex<double>> bins ((std::size_t) (length / 2 + 1));
            std::vector<float> cycle ((std::size_t) length, 0.0f);

            for (int frame = 0; frame < frameCount; ++frame)
            {
                const auto& spectrum = frames[(std::size_t) frame];

                for (auto& bin : bins)
                    bin = { 0.0, 0.0 };

                const int available = (int) spectrum.amplitude.size();
                const int highest = keep < available ? keep : available;

                for (int h = 1; h <= highest && h < (int) bins.size(); ++h)
                {
                    const double amplitude = (double) spectrum.amplitude[(std::size_t) (h - 1)];

                    const double phase = (std::size_t) (h - 1) < spectrum.phase.size()
                                       ? (double) spectrum.phase[(std::size_t) (h - 1)]
                                       : 0.0;

                    // An amplitude of A at harmonic h means a bin of A*n/2:
                    // the inverse transform scales by 1/n and the conjugate
                    // pair contributes twice. WaveTableTest asserts this by
                    // asking for a known spectrum and measuring what comes
                    // back, rather than trusting the derivation.
                    const double scale = amplitude * (double) length * 0.5;

                    bins[(std::size_t) h] = { scale * std::cos (phase),
                                              scale * std::sin (phase) };
                }

                fft.inverse (bins.data(), cycle.data());

                float* destination = store.samples.data()
                                   + (std::size_t) frame * (std::size_t) (length + 3);

                // The cycle sits at offset one, with its own tail in front of
                // it and its own head behind it, so that a four point read
                // anywhere in the cycle stays inside the array.
                destination[0] = cycle[(std::size_t) (length - 1)];

                for (int i = 0; i < length; ++i)
                    destination[i + 1] = cycle[(std::size_t) i];

                destination[length + 1] = cycle[0];
                destination[length + 2] = cycle[(std::size_t) (length > 1 ? 1 : 0)];
            }
        }

        normalise();
    }

    bool isBuilt() const noexcept { return frameCount > 0 && ! levels.empty(); }
    int  getNumFrames() const noexcept { return frameCount; }

    /** What the whole mipmap costs, so a test can hold the design to a
        budget rather than discovering it on somebody's machine. */
    std::size_t memoryBytes() const noexcept
    {
        std::size_t total = 0;

        for (const auto& level : levels)
            total += level.samples.size() * sizeof (float);

        return total;
    }

    //==========================================================================
    /** One sample.

        `phase` is a fraction of a cycle in [0, 1). `position` is where along
        the row of frames to read, from 0 to frames - 1. `level` is what
        `levelFor` returned for the note being played.

        Interpolated three ways: along the waveform, between the two frames
        either side of the position, and between the two levels either side of
        the note. The third is the one that is easy to leave out and is the
        difference between a table change being inaudible and being a tick.
    */
    float read (double phase, double position, double level) const noexcept
    {
        if (! isBuilt())
            return 0.0f;

        if (level < 0.0)
            level = 0.0;

        const double highest = (double) (numLevels - 1);

        if (level > highest)
            level = highest;

        const int lower = (int) level;
        const int upper = lower + 1 < numLevels ? lower + 1 : lower;
        const double blend = level - (double) lower;

        const float a = readLevel (lower, phase, position);

        if (blend <= 0.0 || upper == lower)
            return a;

        const float b = readLevel (upper, phase, position);

        return (float) (a + ((double) b - (double) a) * blend);
    }

private:
    struct Level
    {
        int length = 0;
        std::vector<float> samples;   /**< frames end to end, each length + 3 */
    };

    /** Catmull-Rom, which is the cubic through the two samples either side
        with the slopes taken from their neighbours.

        Chosen over a plain cubic Lagrange because it passes through the
        stored samples exactly, so a table read at a whole sample returns that
        sample and nothing else, which keeps the single harmonic case exact
        and makes the test above able to compare against a cosine.
    */
    static double cubic (double a, double b, double c, double d, double t) noexcept
    {
        const double c1 = 0.5 * (c - a);
        const double c2 = a - 2.5 * b + 2.0 * c - 0.5 * d;
        const double c3 = 0.5 * (d - a) + 1.5 * (b - c);

        return ((c3 * t + c2) * t + c1) * t + b;
    }

    float readLevel (int level, double phase, double position) const noexcept
    {
        const auto& store = levels[(std::size_t) level];
        const int length = store.length;

        // Phase is wrapped by the caller in the normal case, but a modulation
        // source can push it anywhere, and a table read off the end of the
        // array is a crash rather than a wrong note.
        phase -= std::floor (phase);

        if (! (phase >= 0.0 && phase < 1.0))
            phase = 0.0;

        const double exact = phase * (double) length;
        int index = (int) exact;

        if (index >= length)        // only reachable if phase rounded up to one
            index = length - 1;

        const double fraction = exact - (double) index;

        if (position < 0.0)
            position = 0.0;

        const double lastFrame = (double) (frameCount - 1);

        if (position > lastFrame)
            position = lastFrame;

        const int frameA = (int) position;
        const int frameB = frameA + 1 <= frameCount - 1 ? frameA + 1 : frameA;
        const double frameBlend = position - (double) frameA;

        const int stride = length + 3;

        // The cycle starts at offset one, so the four points a cubic needs
        // around `index` are the four entries from `index` onward.
        const float* rowA = store.samples.data()
                          + (std::size_t) frameA * (std::size_t) stride
                          + (std::size_t) index;

        if (frameBlend <= 0.0 || frameB == frameA)
            return (float) cubic (rowA[0], rowA[1], rowA[2], rowA[3], fraction);

        const float* rowB = store.samples.data()
                          + (std::size_t) frameB * (std::size_t) stride
                          + (std::size_t) index;

        // Blended before the cubic rather than after it. The cubic is a
        // weighted sum of its four points, so blending first gives exactly
        // the same answer as blending two cubics, at a third of the work.
        const double p0 = rowA[0] + (rowB[0] - rowA[0]) * frameBlend;
        const double p1 = rowA[1] + (rowB[1] - rowA[1]) * frameBlend;
        const double p2 = rowA[2] + (rowB[2] - rowA[2]) * frameBlend;
        const double p3 = rowA[3] + (rowB[3] - rowA[3]) * frameBlend;

        return (float) cubic (p0, p1, p2, p3, fraction);
    }

    /** Brings the table to a peak of one.

        One factor for the whole table, not one per level and not one per
        frame. Per level would change the loudness as a note moves between
        levels, which is the tick this design exists to avoid. Per frame would
        change the loudness as the position control is swept, which would make
        a wavetable sweep a volume ride as well as a timbre one, and would
        stop an LFO on position from being usable on anything with a quiet
        frame in it.
    */
    void normalise() noexcept
    {
        float peak = 0.0f;

        for (const auto& level : levels)
            for (float sample : level.samples)
            {
                const float magnitude = sample < 0.0f ? -sample : sample;

                if (magnitude > peak)
                    peak = magnitude;
            }

        if (! (peak > 0.0f))
            return;

        const float scale = 1.0f / peak;

        for (auto& level : levels)
            for (float& sample : level.samples)
                sample *= scale;
    }

    std::vector<Level> levels;
    int frameCount = 0;
};

//==============================================================================
/** An oscillator reading a wavetable.

    Holds the phase, the note and the sample rate, and picks the level. The
    table itself is shared and const: every voice of every note reads the same
    mipmap, which is the reason building it is affordable.
*/
class WaveTableOscillator
{
public:
    void setTable (const WaveTable* newTable) noexcept { table = newTable; }

    void setSampleRate (double newRate) noexcept
    {
        sampleRate = newRate > 0.0 ? newRate : 44100.0;
        updateIncrement();
    }

    void setFrequency (double hz) noexcept
    {
        // There is no waveform above Nyquist, and an increment of one or more
        // would read the table backwards or stand still.
        const double limit = sampleRate * 0.5;
        frequency = hz < 0.0 ? 0.0 : (hz > limit ? limit : hz);
        updateIncrement();
    }

    /** Where along the row of frames to read, from 0 to 1 across the whole
        table, so a patch and a modulation source need not know how many
        frames a particular table happens to have. */
    void setPosition (double newPosition) noexcept
    {
        position = newPosition < 0.0 ? 0.0 : (newPosition > 1.0 ? 1.0 : newPosition);
    }

    double getFrequency() const noexcept { return frequency; }

    void reset (double startPhase = 0.0) noexcept
    {
        phase = startPhase - std::floor (startPhase);
    }

    /** `phaseOffset` is a fraction of a cycle added before reading and not
        accumulated, which is what makes this a phase modulation input rather
        than a tuning one: the oscillator stays in tune however hard it is
        driven, where adding to the increment instead would send the pitch
        wandering off with the modulator. */
    float nextSample (double phaseOffset = 0.0) noexcept
    {
        if (table == nullptr || ! table->isBuilt())
            return 0.0f;

        const double frames = (double) (table->getNumFrames() - 1);
        const float value = table->read (phase + phaseOffset, position * frames, level);

        phase += increment;

        if (phase >= 1.0)
            phase -= std::floor (phase);

        return value;
    }

private:
    void updateIncrement() noexcept
    {
        increment = frequency / sampleRate;
        level = WaveTable::levelFor (frequency, sampleRate);
    }

    const WaveTable* table = nullptr;

    double sampleRate = 44100.0;
    double frequency  = 0.0;
    double increment  = 0.0;
    double phase      = 0.0;
    double position   = 0.0;
    double level      = 0.0;
};

} // namespace Synth
