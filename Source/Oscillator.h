#pragma once
#include <cmath>

//==============================================================================
/** Band limited oscillator shapes, by PolyBLEP.

    A naive saw or square is a discontinuity sampled directly, and a
    discontinuity has energy at every frequency. Everything above Nyquist
    folds back down as partials that are not harmonically related to the note,
    so they do not move with it: play a chromatic run and the aliases walk the
    other way. That inharmonic shimmer in the top two octaves is the single
    thing that makes a soft synth sound cheap, and no filter afterwards
    removes it, because by then it is inside the audible band.

    PolyBLEP corrects it by replacing the two samples either side of each
    discontinuity with a polynomial, which is a cheap approximation of the
    step response of a band limited step. Two samples is the smallest useful
    version and costs almost nothing per sample.

    The polynomial is the standard one:

        t < dt:      t /= dt;        t + t - t*t - 1
        t > 1 - dt:  t = (t-1)/dt;   t*t + t + t + 1

    where dt is the phase increment per sample. The correction is subtracted
    from a saw at its single wrap, and applied to a pulse at both edges with
    opposite signs. Sources are in Tests/OscillatorTest.cpp, which checks the
    result rather than taking it on trust.

    No JUCE, so the tests can reach it.
*/
class Oscillator
{
public:
    enum class Shape { sine = 0, saw, square, triangle };

    void setSampleRate (double newRate) noexcept
    {
        sampleRate = newRate > 0.0 ? newRate : 44100.0;
        updateIncrement();
    }

    void setFrequency (double hz) noexcept
    {
        // Clamped below Nyquist. Above it there is no waveform to speak of,
        // and letting the increment reach or pass one would make the PolyBLEP
        // windows overlap and the correction meaningless.
        const double limit = sampleRate * 0.5;
        frequency = hz < 0.0 ? 0.0 : (hz > limit ? limit : hz);
        updateIncrement();
    }

    void setShape (Shape s) noexcept { shape = s; }

    /** Duty cycle of the pulse, as a fraction of the cycle. Kept away from
        zero and one, where the wave would vanish and both edges would land on
        the same sample. */
    void setPulseWidth (double w) noexcept
    {
        pulseWidth = w < 0.05 ? 0.05 : (w > 0.95 ? 0.95 : w);
    }

    void reset (double startPhase = 0.0) noexcept
    {
        phase = startPhase - std::floor (startPhase);
    }

    double getFrequency() const noexcept { return frequency; }

    float nextSample() noexcept
    {
        const double t = phase;
        double value = 0.0;

        switch (shape)
        {
            case Shape::sine:
                value = std::sin (twoPi * t);
                break;

            case Shape::saw:
                // Rises from -1 to 1 and drops back, so the one discontinuity
                // is at the wrap, and the correction comes straight off.
                value = 2.0 * t - 1.0;
                value -= polyBlep (t);
                break;

            case Shape::square:
            {
                value = t < pulseWidth ? 1.0 : -1.0;

                // Rising edge at zero, falling edge at the duty point. The
                // second is found by sliding the phase so that the edge sits
                // at zero for the same polynomial.
                value += polyBlep (t);
                value -= polyBlep (wrap (t - pulseWidth));

                // A pulse that is not half open carries a DC offset of
                // 2w - 1. Harmless on its own and not harmless once it has
                // been through a resonant filter and a mixer with other
                // sources, so it comes off here.
                value -= 2.0 * pulseWidth - 1.0;
                break;
            }

            case Shape::triangle:
                // Deliberately naive, and worth being plain about: this is
                // not band limited. A triangle's partials fall off as one
                // over n squared where a saw's fall off as one over n, so
                // what aliases is roughly forty decibels down by comparison
                // and in practice sits under the rest of a patch. Doing it
                // properly means integrating the corrected pulse, which
                // needs a leaky integrator and brings its own drift. If this
                // ever proves audible, that is the fix, and the aliasing
                // figures in the test are what should decide it.
                value = 4.0 * std::fabs (t - 0.5) - 1.0;
                break;
        }

        phase = wrap (phase + increment);
        return (float) value;
    }

private:
    static constexpr double twoPi = 6.283185307179586476925286766559;

    static double wrap (double t) noexcept
    {
        t -= std::floor (t);
        return t;
    }

    void updateIncrement() noexcept
    {
        increment = frequency / sampleRate;
    }

    /** The correction, zero everywhere except within one sample of an edge. */
    double polyBlep (double t) const noexcept
    {
        const double dt = increment;

        if (dt <= 0.0)
            return 0.0;

        if (t < dt)
        {
            t /= dt;
            return t + t - t * t - 1.0;
        }

        if (t > 1.0 - dt)
        {
            t = (t - 1.0) / dt;
            return t * t + t + t + 1.0;
        }

        return 0.0;
    }

    double sampleRate = 44100.0;
    double frequency  = 440.0;
    double increment  = 440.0 / 44100.0;
    double phase      = 0.0;
    double pulseWidth = 0.5;
    Shape  shape      = Shape::saw;
};
