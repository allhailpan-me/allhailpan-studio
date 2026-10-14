#pragma once
#include <cmath>
#include <cstddef>
#include <vector>

//==============================================================================
/** A delay line that can be read between samples.

    Everything that delays a signal by an amount that is not a whole number of
    samples needs this, and almost everything interesting does: a chorus is a
    delay of a few milliseconds being moved about, a flanger is the same thing
    shorter, a granular effect reads a window out of the past at a different
    speed, and an ordinary delay set in milliseconds lands between samples at
    every tempo nobody chose on purpose.

    **Read with a cubic Lagrange interpolator, not with an allpass.** Both
    families are set out in Laakso, Välimäki, Karjalainen and Laine,
    "Splitting the Unit Delay: Tools for Fractional Delay Filter Design",
    IEEE Signal Processing Magazine 13(1), January 1996, pages 30 to 60. The
    allpass family is flatter in magnitude for the same order, which makes it
    the better answer for a delay that stays put, and it is the wrong answer
    here: an allpass interpolator carries state, so changing its coefficient
    leaves a transient behind. A chorus changes the delay every single
    sample, which means the transient never stops, and what should be a
    gentle detune is a continuous warble on top of one.

    A Lagrange interpolator has no state of its own. The coefficients depend
    only on where between two samples the read lands, so moving the read is
    free of any history, and a delay swept from one end of its range to the
    other is as clean at the end as at the start. The price is paid in
    magnitude: it is not flat, and it loses the top of the band. Measured in
    `DelayLineTest` across the range a read can land in, against the two
    obvious alternatives:

        at 0.5 samples of fraction      linear      cubic
        1 kHz                          -0.02 dB    -0.00 dB
        5 kHz                          -0.47 dB    -0.04 dB
        10 kHz                         -2.01 dB    -0.53 dB
        15 kHz                         -5.11 dB    -2.53 dB

    Half a sample is the worst case; at a whole number of samples every
    interpolator is exact and the loss is nothing at all.

    **The shortest delay a cubic read can do is one sample.** Four points are
    needed, one before the read and two after, so the newest of them is one
    sample older than the write. A delay shorter than that is clamped rather
    than reaching for a sample that has not been written, which would read
    whatever the oldest part of the buffer still held: a sound from a second
    ago, at full level, once per buffer. A flanger wants delays of well under
    a millisecond and at 48 kHz one sample is a fiftieth of that, so nothing
    musical is given up.

    No JUCE.
*/
namespace Fx
{

class DelayLine
{
public:
    /** Allocates, so it belongs wherever the sample rate is set and never on
        the audio thread.

        The buffer is a power of two long, so that wrapping is a mask rather
        than a branch or a modulo. That matters here more than it usually
        does: a granular effect reads a dozen times per sample from different
        places in the past, and a conditional in each of those reads is a
        dozen branches the processor cannot predict.
    */
    void prepare (double sampleRate, double maximumSeconds)
    {
        rate = sampleRate > 0.0 ? sampleRate : 44100.0;

        const double wanted = maximumSeconds > 0.001 ? maximumSeconds : 0.001;

        // Four samples of room beyond what anybody can ask for, because a
        // cubic read at the very end of the line still needs the two points
        // after it.
        std::size_t size = 16;
        const std::size_t needed = (std::size_t) std::ceil (wanted * rate) + 4;

        while (size < needed)
            size *= 2;

        buffer.assign (size, 0.0);
        mask = size - 1;
        writePosition = 0;

        longest = (double) (size - 4);
    }

    void reset() noexcept
    {
        for (auto& v : buffer)
            v = 0.0;

        writePosition = 0;
    }

    /** The longest delay this line can give, in samples, and in seconds. */
    double maximumSamples() const noexcept { return longest; }
    double maximumSeconds() const noexcept { return longest / rate; }

    /** The shortest a cubic read can be: see the class comment. */
    static constexpr double shortestSamples = 1.0;

    //==========================================================================
    void write (double value) noexcept
    {
        if (buffer.empty())
            return;

        writePosition = (writePosition + 1) & mask;
        buffer[writePosition] = value;
    }

    /** Reads this many samples back from the newest one written.

        A delay of one is the sample before the one just written. Anything
        shorter or longer than the line can do is clamped to what it can,
        rather than wrapping round to a sound from the far end of the buffer.
    */
    double read (double delayInSamples) const noexcept
    {
        if (buffer.empty())
            return 0.0;

        const double clamped = delayInSamples < shortestSamples
                                 ? shortestSamples
                                 : (delayInSamples > longest ? longest : delayInSamples);

        const double whole = std::floor (clamped);
        const double fraction = clamped - whole;

        const std::size_t at = (std::size_t) whole;

        // The four points, newest first. The read lands between the second
        // and the third of them, which is what a four point interpolator
        // wants: one point behind the gap and two ahead of it.
        const double a = buffer[(writePosition - at + 1) & mask];
        const double b = buffer[(writePosition - at) & mask];
        const double c = buffer[(writePosition - at - 1) & mask];
        const double d = buffer[(writePosition - at - 2) & mask];

        return cubic (a, b, c, d, fraction);
    }

    double readSeconds (double delay) const noexcept
    {
        return read (delay * rate);
    }

    /** A linear read, which is here for the measurement in the test and for
        nothing else. Kept so that the claim the class comment makes about
        the cubic being worth its extra three multiplies can be rechecked
        rather than believed. */
    double readLinear (double delayInSamples) const noexcept
    {
        if (buffer.empty())
            return 0.0;

        const double clamped = delayInSamples < 0.0
                                 ? 0.0
                                 : (delayInSamples > longest ? longest : delayInSamples);

        const double whole = std::floor (clamped);
        const double fraction = clamped - whole;

        const std::size_t at = (std::size_t) whole;

        const double b = buffer[(writePosition - at) & mask];
        const double c = buffer[(writePosition - at - 1) & mask];

        return b + (c - b) * fraction;
    }

    //==========================================================================
    /** The four point Lagrange interpolator, written out so a test can
        compare against the polynomial it is supposed to be.

        `t` runs from nothing to one between `b` and `c`, with `a` the sample
        before `b` and `d` the one after `c`. This is the Lagrange cubic and
        not a Catmull-Rom spline: the two are different interpolators that
        are easy to confuse, and the Lagrange form is the one whose
        coefficients come out of the fractional delay literature above.
    */
    static double cubic (double a, double b, double c, double d, double t) noexcept
    {
        // The products of the distances to the other three points, which is
        // all a Lagrange basis is. Written this way rather than as expanded
        // powers of t because it is the definition and because the
        // definition is what the test checks against.
        const double tPlusOne = t + 1.0;
        const double tMinusOne = t - 1.0;
        const double tMinusTwo = t - 2.0;

        return a * (t * tMinusOne * tMinusTwo) / -6.0
             + b * (tPlusOne * tMinusOne * tMinusTwo) / 2.0
             + c * (tPlusOne * t * tMinusTwo) / -2.0
             + d * (tPlusOne * t * tMinusOne) / 6.0;
    }

private:
    std::vector<double> buffer;

    std::size_t mask = 0;
    std::size_t writePosition = 0;

    double rate = 44100.0;
    double longest = 0.0;
};

} // namespace Fx
