#pragma once
#include <cmath>

//==============================================================================
/** The filter every effect in the studio is built on: a two pole state
    variable filter using the topology preserving transform.

    `Filter.h` is this same core, specialised for one voice of the synth. This
    one is the general case: every output a two pole section can produce, the
    bell and shelf forms an equaliser needs, and double precision, because an
    equaliser band at 30 hertz with a narrow Q has poles close enough to the
    unit circle that single precision coefficients move the centre frequency
    audibly.

    Why this topology rather than a direct form biquad.

    An effect's filter is modulated. A filter sweep, an envelope follower on a
    wah, an LFO on a chorus's tone control: all of them move the cutoff while
    audio is passing. A direct form biquad stores past inputs and outputs, and
    those mean something different the moment the coefficients change, so fast
    sweeps click and ring. The transform keeps the analogue prototype's
    integrators as integrators, with trapezoidal integration in place of the
    analogue ones, so the state stays meaningful while the coefficients move
    and resonance holds right up to Nyquist instead of collapsing as it does
    in digital ladder approximations.

    The cutoff is prewarped with a tangent, which is what makes the digital
    cutoff land on the analogue one rather than drifting flat near Nyquist.

        g  = tan(pi * fc / fs)
        k  = 1 / Q
        a1 = 1 / (1 + g * (g + k)),   a2 = g * a1,   a3 = g * a2

    and per sample

        v3 = input - ic2eq
        v1 = a1 * ic1eq + a2 * v3
        v2 = ic2eq + a2 * ic1eq + a3 * v3
        ic1eq = 2 * v1 - ic1eq
        ic2eq = 2 * v2 - ic2eq

    Every response is then a weighted sum of the input, the bandpass v1 and
    the lowpass v2, which is the part that makes this worth generalising: one
    filter structure, nine responses, and switching between them is three
    numbers rather than a different object.

        output = m0 * input + m1 * v1 + m2 * v2

    Source: Andrew Simper, Cytomic, "Solving the continuous SVF equations
    using trapezoidal integration and equivalent currents", which sets out the
    trapezoidal companion model, the nodal solution and this coefficient form.
    https://cytomic.com/files/dsp/SvfLinearTrapOptimised2.pdf

    The mixing weights below are not taken on trust. `SvfTest` computes this
    filter's exact transfer function from its state space form and asserts
    what each response has to do: the gain a bell reaches at its centre, that
    a shelf's two ends are its two gains, that a lowpass is one at DC and
    nothing at Nyquist, that an allpass is flat everywhere. A weight typed
    wrong fails that, where it would otherwise be a response slightly unlike
    the one asked for, which is the kind of wrong nobody hears until they
    compare against another studio.

    No JUCE, so the tests can reach it.
*/
namespace Fx
{

class Svf
{
public:
    enum class Type
    {
        lowpass = 0,
        highpass,
        bandpass,       /**< unity gain at the centre */
        bandpassQ,      /**< gain of Q at the centre, the raw integrator tap */
        notch,
        peak,           /**< highpass minus lowpass */
        allpass,
        bell,           /**< a boost or cut around the centre, flat at both ends */
        lowShelf,
        highShelf
    };

    void setSampleRate (double newRate) noexcept
    {
        sampleRate = newRate > 0.0 ? newRate : 44100.0;
        update();
    }

    double getSampleRate() const noexcept { return sampleRate; }

    /** Everything a band needs, set together.

        Together rather than one at a time because the coefficients depend on
        all three: setting them separately recomputes the whole set once per
        call and invites a half updated filter if one of the calls is ever
        forgotten.

        `gainDb` is read only by `bell`, `lowShelf` and `highShelf`.
    */
    void set (Type newType, double hz, double q, double gainDb = 0.0) noexcept
    {
        type = newType;

        // The tangent runs off to infinity at Nyquist, so the cutoff has to
        // stop short of it. 0.49 rather than 0.5 keeps g finite and leaves
        // the top of a shelf's range usable, since a shelf multiplies g.
        const double highest = sampleRate * 0.49;
        frequency = hz < 1.0 ? 1.0 : (hz > highest ? highest : hz);

        // Below about 0.025 the two pole denominator stops being a resonant
        // section at all, and above 100 a band is narrower than any ear or
        // any room can make use of.
        resonance = q < 0.025 ? 0.025 : (q > 100.0 ? 100.0 : q);

        // Past this the squaring below overflows what a float buffer can
        // carry, and nothing musical lives out there anyway.
        gain = gainDb < -120.0 ? -120.0 : (gainDb > 120.0 ? 120.0 : gainDb);

        update();
    }

    void reset() noexcept
    {
        ic1eq = 0.0;
        ic2eq = 0.0;
    }

    double process (double input) noexcept
    {
        const double v3 = input - ic2eq;
        const double v1 = a1 * ic1eq + a2 * v3;
        const double v2 = ic2eq + a2 * ic1eq + a3 * v3;

        // The trapezoidal integrator's state update: twice the output minus
        // the old state. This is what keeps the filter equivalent to the
        // analogue circuit rather than to a difference equation that happens
        // to have a similar response.
        ic1eq = 2.0 * v1 - ic1eq;
        ic2eq = 2.0 * v2 - ic2eq;

        return m0 * input + m1 * v1 + m2 * v2;
    }

    /** The coefficients, for the test and for anything that needs to reason
        about this filter rather than run it. */
    struct Coefficients
    {
        double a1, a2, a3;
        double m0, m1, m2;
    };

    Coefficients coefficients() const noexcept { return { a1, a2, a3, m0, m1, m2 }; }

private:
    void update() noexcept
    {
        const double A = std::pow (10.0, gain / 40.0);   // amplitude, half the dB
        const double w = pi * frequency / sampleRate;

        double g = std::tan (w);
        double k = 1.0 / resonance;

        switch (type)
        {
            case Type::lowpass:   m0 = 0.0; m1 = 0.0;    m2 = 1.0;  break;
            case Type::highpass:  m0 = 1.0; m1 = -k;     m2 = -1.0; break;

            // v1 is the bandpass with a peak of Q, which is the integrator
            // tap as it comes. Multiplying by k brings the peak to one, and
            // both are wanted: one for a filter sweep, the other for the
            // bandpass of an equaliser.
            case Type::bandpassQ: m0 = 0.0; m1 = 1.0;    m2 = 0.0;  break;
            case Type::bandpass:  m0 = 0.0; m1 = k;      m2 = 0.0;  break;

            case Type::notch:     m0 = 1.0; m1 = -k;     m2 = 0.0;  break;
            case Type::peak:      m0 = 1.0; m1 = -k;     m2 = -2.0; break;
            case Type::allpass:   m0 = 1.0; m1 = -2.0*k; m2 = 0.0;  break;

            case Type::bell:
                // The bell's skirt is set by Q and its height by A, and the
                // two have to be separated or a boost would also narrow the
                // band. Dividing k by A does that: the denominator widens by
                // exactly as much as the numerator's boost, so the width at
                // the half gain point stays where Q put it.
                k  = 1.0 / (resonance * A);
                m0 = 1.0;
                m1 = k * (A * A - 1.0);
                m2 = 0.0;
                break;

            case Type::lowShelf:
                // Dividing g by the square root of A holds the shelf's half
                // gain point at the frequency asked for, rather than letting
                // it slide with the amount of boost.
                g  = g / std::sqrt (A);
                m0 = 1.0;
                m1 = k * (A - 1.0);
                m2 = A * A - 1.0;
                break;

            case Type::highShelf:
                g  = g * std::sqrt (A);
                m0 = A * A;
                m1 = k * (1.0 - A) * A;
                m2 = 1.0 - A * A;
                break;
        }

        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    static constexpr double pi = 3.14159265358979323846;

    double sampleRate = 44100.0;
    double frequency  = 1000.0;
    double resonance  = 0.70710678118654752;
    double gain       = 0.0;
    Type   type       = Type::lowpass;

    double a1 = 0.0, a2 = 0.0, a3 = 0.0;
    double m0 = 0.0, m1 = 0.0, m2 = 1.0;
    double ic1eq = 0.0, ic2eq = 0.0;
};

} // namespace Fx
