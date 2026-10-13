#pragma once
#include <cmath>

//==============================================================================
/** A rise and a fall, which will either happen once or go round forever.

    The 0-Coast calls this Slope and a Buchla calls it a function generator,
    and the useful thing about both is that they refuse to be one kind of
    module. Fired once it is an envelope. Left to cycle it is an LFO. Cycled
    fast enough it is an oscillator, and since its rise and fall are set
    separately it is an oscillator whose waveform you can sweep from a ramp
    through a triangle to the other ramp while it runs.

    That is why it is here rather than being another entry in the modulation
    matrix's list of LFO shapes. An envelope and an LFO being the same object
    is not a convenience, it is the thing: the slow sweep in a west coast
    patch is an envelope that never stops, and the vibrato is the same module
    turned up, and the growl is the same module turned up again until it is
    in the audible range.

    The recording this was built towards has one of these cycling at about
    0.35 hertz, taking a wavefolder from nearly closed to wide open and back
    every 2.8 seconds. There was no way to make that: `Envelope.h` is an
    ADSR, which holds while a note is held and so cannot cycle, and an LFO
    with a fixed shape cannot have a rise of one length and a fall of
    another.

    **The shape control is a curve, not a selection.** Hardware function
    generators bend between logarithmic, linear and exponential continuously,
    because the interesting settings are the ones in between. At the middle
    this is exactly linear, which matters: a shape control whose centre is
    nearly straight but not straight is the kind of thing nobody notices and
    everybody has to work around.

    No JUCE.
*/
namespace Synth
{

class Slope
{
public:
    void setSampleRate (double newRate) noexcept
    {
        sampleRate = newRate > 0.0 ? newRate : 44100.0;
        updateIncrements();
    }

    /** Seconds to rise, and seconds to fall.

        The floor is a quarter of a millisecond rather than zero: at zero the
        segment would take no time at all, the cycle would have no period, and
        cycling it would be a division by nothing. A quarter of a millisecond
        is already faster than any envelope needs and is four thousand cycles
        a second if both are set to it, which is the top of where this is an
        oscillator rather than a modulator.
    */
    void setTimes (double riseSeconds, double fallSeconds) noexcept
    {
        rise = clampTime (riseSeconds);
        fall = clampTime (fallSeconds);
        updateIncrements();
    }

    /** Bends both segments, from minus one for logarithmic through zero for
        straight to plus one for exponential.

        Logarithmic rises fast and then eases in, which is what a struck
        thing's envelope does on the way up. Exponential starts slowly and
        then runs away, which is what a capacitor charging through a resistor
        does on the way down, and is why a decay set this way sounds natural
        where a straight line sounds like a fader being moved.
    */
    void setShape (double amount) noexcept
    {
        shape = amount < -1.0 ? -1.0 : (amount > 1.0 ? 1.0 : amount);
    }

    /** Whether it goes round again at the end of the fall. */
    void setCycling (bool shouldCycle) noexcept { cycling = shouldCycle; }
    bool isCycling() const noexcept { return cycling; }

    /** Starts a rise from wherever it currently is.

        From wherever rather than from zero, so that retriggering during a
        fall carries on from there rather than stepping down to nothing first,
        which is a click.
    */
    void trigger() noexcept
    {
        // Where the current output sits on the way up, so the rise picks up
        // at the same height rather than at the same time.
        position = positionForOutput (output);
        falling = false;
        running = true;
    }

    void reset() noexcept
    {
        position = 0.0;
        output = 0.0;
        falling = false;
        running = cycling;
    }

    /** True while there is something happening. A cycling slope is always
        running; a triggered one stops at the bottom. */
    bool isRunning() const noexcept { return running; }

    /** The last value produced, from zero to one. */
    double getOutput() const noexcept { return output; }

    /** How many times a second a cycling slope goes round. Worth having
        because it is the question anybody asks of it once it is cycling, and
        because the answer is not either of the two numbers that were set. */
    double cyclesPerSecond() const noexcept { return 1.0 / (rise + fall); }

    //==========================================================================
    double nextSample() noexcept
    {
        if (! running)
        {
            output = 0.0;
            return output;
        }

        // `position` runs from zero to one across whichever segment is
        // current, so both segments are the same arithmetic with different
        // increments and only the direction differs.
        position += falling ? fallIncrement : riseIncrement;

        while (position >= 1.0)
        {
            position -= 1.0;

            if (! falling)
            {
                falling = true;
            }
            else
            {
                falling = false;

                if (! cycling)
                {
                    running = false;
                    position = 0.0;
                    output = 0.0;
                    return output;
                }
            }
        }

        const double travelled = bend (position);

        output = falling ? 1.0 - travelled : travelled;

        return output;
    }

private:
    /** Bends a straight zero to one into a curve, with the middle of the
        control leaving it exactly straight.

        Written as a power so that the two ends are fixed: whatever the shape,
        the segment still starts at zero and ends at one, so changing the
        shape while it runs changes the path and not the destination. A bend
        that moved the endpoints would make the shape control a level control
        as well.
    */
    double bend (double x) const noexcept
    {
        if (x <= 0.0) return 0.0;
        if (x >= 1.0) return 1.0;

        if (shape == 0.0)
            return x;

        // Three at the ends is enough to be clearly a curve and gentle enough
        // that the fast end of it does not become a step.
        const double power = std::pow (3.0, shape);

        return std::pow (x, power);
    }

    /** The inverse of `bend`, for picking up a retrigger at the current
        height rather than at the current time. */
    double positionForOutput (double value) const noexcept
    {
        if (value <= 0.0) return 0.0;
        if (value >= 1.0) return 1.0;

        if (shape == 0.0)
            return value;

        return std::pow (value, 1.0 / std::pow (3.0, shape));
    }

    static double clampTime (double seconds) noexcept
    {
        if (seconds < 0.00025) return 0.00025;
        if (seconds > 600.0)   return 600.0;

        return seconds;
    }

    void updateIncrements() noexcept
    {
        riseIncrement = 1.0 / (rise * sampleRate);
        fallIncrement = 1.0 / (fall * sampleRate);
    }

    double sampleRate = 44100.0;
    double rise = 0.01;
    double fall = 0.30;
    double shape = 0.0;

    double riseIncrement = 0.0;
    double fallIncrement = 0.0;

    double position = 0.0;
    double output = 0.0;

    bool falling = false;
    bool cycling = false;
    bool running = false;
};

} // namespace Synth
