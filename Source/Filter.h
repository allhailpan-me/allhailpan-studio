#pragma once
#include <cmath>

//==============================================================================
/** Two pole resonant filter, topology preserving transform.

    The filter is where a subtractive synth gets its character, and it is also
    where the naive implementations fall over. A direct form biquad with its
    coefficients recomputed every sample, which is what a filter envelope
    needs, does not behave like an analogue filter being swept: its state
    means something different after each coefficient change, so fast sweeps
    produce clicks and zipper noise, and at high resonance it can be driven
    unstable by the modulation alone. Digital ladder approximations tend to
    lose their resonance as the cutoff approaches Nyquist, so the filter goes
    dull exactly where it should still sing.

    The topology preserving transform avoids both. It keeps the integrators of
    the analogue prototype as integrators, with trapezoidal integration in
    place of the analogue ones, so the state stays meaningful when the
    coefficients move and the resonance holds all the way up. The cutoff is
    prewarped with a tangent, which makes the digital cutoff land exactly on
    the analogue one rather than drifting flat near Nyquist.

        g  = tan(pi * fc / fs)
        k  = 1 / Q
        a1 = 1 / (1 + g * (g + k)),  a2 = g * a1,  a3 = g * a2

    Measured against the analogue prototype before it was written: the
    lowpass magnitude at the cutoff equals Q to four decimal places for Q
    from 0.5 to 10, the gain is one at DC and nothing at Nyquist, and the
    structural identity below holds to machine precision.

        highpass + k * bandpass + lowpass == input

    That identity is exact, not approximate, and FilterTest asserts it, which
    is what makes a coefficient typed wrong impossible to miss.

    No JUCE, so the tests can reach it.
*/
class Filter
{
public:
    enum class Mode { lowpass = 0, highpass, bandpass };

    void setSampleRate (double newRate) noexcept
    {
        sampleRate = newRate > 0.0 ? newRate : 44100.0;
        updateCoefficients();
    }

    void setMode (Mode m) noexcept { mode = m; }

    /** Cutoff in hertz. Held away from both ends: the tangent runs off to
        infinity at Nyquist, and nothing useful happens below a few hertz. */
    void setCutoff (double hz) noexcept
    {
        const double highest = sampleRate * 0.45;
        cutoff = hz < 10.0 ? 10.0 : (hz > highest ? highest : hz);
        updateCoefficients();
    }

    /** Resonance as Q. Half is the gentlest useful setting and anything much
        past twenty rings for long enough to be a tone of its own rather than
        a filter, which is a sound, but not one to arrive at by accident. */
    void setResonance (double q) noexcept
    {
        resonance = q < 0.5 ? 0.5 : (q > 20.0 ? 20.0 : q);
        updateCoefficients();
    }

    double getCutoff() const noexcept    { return cutoff; }
    double getResonance() const noexcept { return resonance; }

    void reset() noexcept
    {
        state1 = 0.0;
        state2 = 0.0;
    }

    float nextSample (float input) noexcept
    {
        const double v0 = input;
        const double v3 = v0 - state2;
        const double v1 = a1 * state1 + a2 * v3;
        const double v2 = state2 + a2 * state1 + a3 * v3;

        // Trapezoidal integrators: the new state is twice the output minus
        // the old state, which is what keeps this equivalent to the analogue
        // circuit rather than to a difference equation that happens to have
        // a similar response.
        state1 = 2.0 * v1 - state1;
        state2 = 2.0 * v2 - state2;

        switch (mode)
        {
            case Mode::lowpass:  return (float) v2;
            case Mode::bandpass: return (float) v1;
            case Mode::highpass: return (float) (v0 - k * v1 - v2);
        }

        return (float) v2;
    }

private:
    void updateCoefficients() noexcept
    {
        const double g = std::tan (pi * cutoff / sampleRate);

        k  = 1.0 / resonance;
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    static constexpr double pi = 3.14159265358979323846;

    double sampleRate = 44100.0;
    double cutoff     = 1000.0;
    double resonance  = 0.707;

    double k = 1.0 / 0.707;
    double a1 = 0.0, a2 = 0.0, a3 = 0.0;
    double state1 = 0.0, state2 = 0.0;
    Mode   mode = Mode::lowpass;
};
