#pragma once
#include <cmath>
#include <cstdint>

//==============================================================================
/** A low frequency oscillator, which is only low frequency by habit.

    Four of these drive whatever a patch points them at, and the matrix
    declares six octaves of rate modulation for each, which takes one from a
    slow sweep to an audio rate tone. So this is built as an oscillator that
    happens to be slow rather than as a thing that only works below twenty
    hertz, and the difference shows up in two places: the edges are
    antialiased, and the shapes are exact rather than approximated.

    **Why this is not `Synth/Slope.h`.** A slope is a rise and a fall with
    independent times, which is an envelope that can be left to cycle. It is
    unipolar, it has no shape beyond its bend, and its two times are what set
    its rate. An LFO is a waveform about a centre with one rate and one
    shape. Both exist because patches want both, and neither can be made to
    do the other's job without becoming a worse version of both.

    **The edges are antialiased, by PolyBLEP.** A sawtooth or a square read
    naively from a phase accumulator steps between samples, and a step
    carries energy at every frequency, so above a few hundred hertz the
    aliases of those steps land back in the audible band at frequencies that
    are not multiples of anything. The correction is the polynomial band
    limited step: where the phase has crossed a discontinuity within the last
    sample, a two sample polynomial replaces the step with the ramp a band
    limited one would have had.

    From Välimäki and Huovilainen's work on antialiasing classic waveforms,
    surveyed in Välimäki, Pekonen and Nam, "Perceptually informed synthesis
    of bandlimited classical waveforms using integrated polynomial
    interpolation", JASA 131(1), 2012
    (https://mac.kaist.ac.kr/pubs/ValimakiPeknenNam-jasa2012.pdf). The
    residual, with `t` the phase in zero to one and `dt` the phase step per
    sample:

        t < dt:       x = t / dt,        residual = 2x - x^2 - 1
        t > 1 - dt:   x = (t - 1) / dt,  residual = x^2 + 2x + 1
        otherwise:    0

    which runs from plus one just before the jump to minus one just after it,
    so subtracting it from a sawtooth that falls by two at the wrap leaves
    the output passing smoothly through the middle instead of stepping.

    It is one correction per discontinuity and costs almost nothing, and it
    does not make a square as clean as a wavetable: it removes the first
    order of the aliasing and leaves the rest. Measured in `LfoTest`, which
    is the only reason to believe any of the above.

    **Sample and hold and smooth random are shapes here, not a separate
    module.** They are what a patch reaches for when it wants a new value
    every cycle rather than a waveform, and they share everything else an
    LFO has: a rate, a retrigger, a fade. The random is drawn from the
    object's own sequence, so two LFOs set identically do not move together,
    and a voice's own copies are seeded from the voice rather than shared.

    No JUCE.
*/
namespace Synth
{

class Lfo
{
public:
    enum class Shape
    {
        sine = 0,
        triangle,
        sawUp,
        sawDown,
        square,
        pulse,          /**< a square with a width control */
        sampleAndHold,  /**< a new random value each cycle, held */
        smoothRandom    /**< the same, with a smooth path between values */
    };

    //==========================================================================
    void setSampleRate (double newRate) noexcept
    {
        sampleRate = newRate > 0.0 ? newRate : 44100.0;
        recalculate();
    }

    void setShape (Shape newShape) noexcept { shape = newShape; }

    /** The rate in hertz. Clamped to something a phase accumulator can
        actually carry: a cycle every four minutes at the bottom, and just
        under Nyquist at the top, where the increment approaches a half and
        the waveform stops being one. */
    void setRateHz (double hz) noexcept
    {
        const double highest = sampleRate * 0.45;

        rateHz = hz < 0.004 ? 0.004 : (hz > highest ? highest : hz);
        recalculate();
    }

    double getRateHz() const noexcept { return rateHz; }

    void setPulseWidth (double width) noexcept
    {
        pulseWidth = width < 0.02 ? 0.02 : (width > 0.98 ? 0.98 : width);
    }

    /** Where in its cycle the LFO starts when it is retriggered, from zero
        to one. A patch that wants two LFOs at the same rate to be a quarter
        cycle apart sets this rather than hoping. */
    void setPhaseOffset (double offset) noexcept
    {
        phaseOffset = offset - std::floor (offset);
    }

    /** Whether a note restarts it.

        Off is not a lesser version of on. An LFO left running free is the
        same movement across every note of a chord, which is what a patch
        wants when the movement belongs to the sound rather than to the note;
        retriggered, every note gets its own, which is what a patch wants
        when it belongs to the note. Hardware and software both offer the
        choice for that reason.
    */
    void setRetrigger (bool shouldRetrigger) noexcept { retrigger = shouldRetrigger; }

    /** How long the LFO takes to arrive, in seconds.

        A vibrato that is there from the attack sounds like a broken tape; a
        player leans into it after the note has spoken. The fade is the
        difference, and it is on the LFO rather than left to a routing
        through an envelope because wanting it is the common case.
    */
    void setFadeSeconds (double seconds) noexcept
    {
        fadeSeconds = seconds < 0.0 ? 0.0 : (seconds > 30.0 ? 30.0 : seconds);
        recalculate();
    }

    /** Seeds the random sequence. Two LFOs that would otherwise draw the
        same numbers, and every voice of a chord, are told apart by this. */
    void setSeed (std::uint64_t seed) noexcept
    {
        randomState = seed * 6364136223846793005ull + 1442695040888963407ull;
    }

    //==========================================================================
    void reset() noexcept
    {
        phase = phaseOffset;
        faded = 0.0;
        output = 0.0;
        held = nextRandom();
        previousHeld = held;
    }

    void noteOn() noexcept
    {
        if (retrigger)
        {
            phase = phaseOffset;
            previousHeld = held;
            held = nextRandom();
        }

        // The fade restarts whatever the retrigger setting is. A free
        // running LFO is about where the movement sits in time, not about
        // whether a new note should hear it arrive.
        faded = 0.0;
    }

    double getOutput() const noexcept { return output; }

    //==========================================================================
    /** The next sample, from minus one to one, with the fade applied. */
    double nextSample() noexcept
    {
        const double value = shapeAt (phase);

        advance();

        if (fadeIncrement >= 1.0)
            faded = 1.0;
        else if (faded < 1.0)
            faded = faded + fadeIncrement > 1.0 ? 1.0 : faded + fadeIncrement;

        output = value * faded;

        return output;
    }

private:
    //==========================================================================
    /** The polynomial band limited step's residual, which is plus one just
        before a discontinuity and minus one just after it, and nothing more
        than a sample either side. Subtracting it from a falling edge, or
        adding it to a rising one, replaces the step with the two sample ramp
        a band limited edge has. */
    double blep (double t) const noexcept
    {
        if (t < increment)
        {
            const double x = t / increment;
            return 2.0 * x - x * x - 1.0;
        }

        if (t > 1.0 - increment)
        {
            const double x = (t - 1.0) / increment;
            return x * x + 2.0 * x + 1.0;
        }

        return 0.0;
    }

    double shapeAt (double t) const noexcept
    {
        constexpr double pi = 3.14159265358979323846;

        switch (shape)
        {
            case Shape::sine:
                // No correction needed and none applied: a sine has no
                // discontinuity to band limit, in its value or its slope.
                return std::sin (2.0 * pi * t);

            case Shape::triangle:
                // Nor does a triangle in its value. Its slope breaks at the
                // corners, which throws harmonics falling away as one over
                // their square, far below what a step throws, and correcting
                // that needs the integrated form rather than this one.
                return 4.0 * (t < 0.5 ? t : 1.0 - t) - 1.0;

            case Shape::sawUp:
                return (2.0 * t - 1.0) - blep (t);

            case Shape::sawDown:
                // The same waveform the other way up, correction included,
                // rather than a separate case that could disagree with it.
                return -((2.0 * t - 1.0) - blep (t));

            case Shape::square:
                return pulseAt (t, 0.5);

            case Shape::pulse:
                return pulseAt (t, pulseWidth);

            case Shape::sampleAndHold:
                return held;

            case Shape::smoothRandom:
            {
                // A raised cosine between the last value and this one, so
                // the path has no corner at either end. Linear would leave
                // one at every step, which on a filter cutoff is audible as
                // a tick at the LFO's own rate.
                const double eased = 0.5 - 0.5 * std::cos (pi * t);

                return previousHeld + (held - previousHeld) * eased;
            }
        }

        return 0.0;
    }

    /** A pulse of the given width, with both of its edges corrected: the one
        at the start of the cycle rises, the one at the width falls. */
    double pulseAt (double t, double width) const noexcept
    {
        double value = t < width ? 1.0 : -1.0;

        value += blep (t);

        double shifted = t - width;

        if (shifted < 0.0)
            shifted += 1.0;

        value -= blep (shifted);

        return value;
    }

    void advance() noexcept
    {
        phase += increment;

        if (phase >= 1.0)
        {
            phase -= std::floor (phase);

            // A new random value once per cycle, which is what makes the
            // rate control mean the same thing for the two random shapes as
            // it does for the rest.
            previousHeld = held;
            held = nextRandom();
        }
    }

    double nextRandom() noexcept
    {
        randomState = randomState * 6364136223846793005ull + 1442695040888963407ull;

        return (double) ((randomState >> 33) & 0x7fffff) / (double) 0x7fffff * 2.0 - 1.0;
    }

    void recalculate() noexcept
    {
        increment = rateHz / sampleRate;

        // A whole cycle in a sample or less is not a waveform, and the
        // correction below assumes the discontinuity is inside one step.
        if (increment > 0.45)
            increment = 0.45;

        fadeIncrement = fadeSeconds > 0.0 ? 1.0 / (fadeSeconds * sampleRate) : 2.0;
    }

    Shape shape = Shape::sine;

    double sampleRate = 44100.0;
    double rateHz = 1.0;
    double increment = 1.0 / 44100.0;

    double pulseWidth = 0.5;
    double phaseOffset = 0.0;
    bool   retrigger = true;

    double fadeSeconds = 0.0;
    double fadeIncrement = 2.0;
    double faded = 1.0;

    double phase = 0.0;
    double output = 0.0;

    double held = 0.0, previousHeld = 0.0;

    std::uint64_t randomState = 0x853c49e6748fea9bull;
};

} // namespace Synth
