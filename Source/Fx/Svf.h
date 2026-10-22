#pragma once
#include "BellMatch.h"

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
    filter structure, every response a two pole section has, and switching
    between them is three numbers rather than a different object. It is also
    why the equaliser's matched bell in `BellMatch.h` needs no filter of its
    own: a published design in `s` is five numbers for this one.

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

    Five things in here are deliberately unfailable, and they are written down
    so nobody goes looking for the test that should have caught them. Each
    one was broken on purpose and the suite could not see it, and in each
    case that is the right answer rather than a gap:

    - **The degenerate guard at the top of `magnitudeAt`.** It is a backstop
      nothing reaches. With the corner floored at one hertz and then held
      under 0.49 of a finite rate, `g` is strictly positive and finite, so
      `a1` and `a2` are too, at every setting of all four arguments. That is
      an argument, so `SvfTest` turns it into a reading: it asserts all three
      `a` coefficients are strictly positive over eleven and a half thousand
      settings, the nonsense ones included. The branch was reachable before
      the constructor existed, by a filter nobody had configured.
    - **Recovering `g` from `a2 / a1` rather than `a3 / a2`.** Both are `g`:
      `a2` is `g` times `a1` and `a3` is `g` times `a2`, so either ratio
      gives it back, and the two agree to rounding.
    - **The curve's own ceiling taken at Nyquist rather than at 0.4999 of
      the rate.** Every response tends to `m0` as the prewarped frequency
      runs away, and both ceilings land on that asymptote to within absolute
      rounding, so the answer does not move by anything a tolerance can
      read. It stays because the square of the tangent is what the
      denominator carries, and that overflows once the corner's own tangent
      falls below about 1e-138, which needs a sample rate of the same order.
    - **The first clamp on the frequency `magnitudeAt` is asked about.** The
      `fmin` below it already holds the top end, and a magnitude is even in
      the prewarped frequency, so folding a negative one to DC and
      reflecting it give the same number. Dead either way.
    - **The upper bound on Q.** It is a musical bound and says so: a
      positive `g` and a positive `k` are stable at any Q, so a Q of a
      billion is narrower than any ear or any room can use and perfectly
      well behaved. The lower bound is not like this and is load bearing: a
      Q of nothing makes `k` infinite, which takes `a1` to zero.

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
        highShelf,

        /** The same bell, designed to keep its skirt at the top of the band
            rather than being crushed to nothing at Nyquist. `BellMatch.h`
            says what that means, why a plain bell is five decibels wrong at
            12 kHz, and where the correction runs out. Above the frequency it
            runs out at, this is the plain bell, and `isMatched` says which
            one is running. */
        bellMatched
    };

    /** The coefficients are built here rather than left at zero.

        Left at zero the filter gave two different wrong answers and neither
        was the one its own members describe: `process` returned silence for
        ever, because every `a` was zero, while `magnitudeAt` took its
        degenerate branch and reported unity at every frequency, where the
        members below say a one kilohertz lowpass at 44.1 kHz. A filter that
        nobody has configured yet is now that lowpass, in both answers.

        The useful consequence is that `a1` and `a2` are non-zero at every
        reachable setting, which is what makes the degenerate branch in
        `magnitudeAt` a backstop rather than a state the studio can be in.
    */
    Svf() noexcept { update(); }

    void setSampleRate (double newRate) noexcept
    {
        // Finite as well as positive. Infinity passes a bare `> 0.0`, and a
        // rate of infinity makes the prewarping tangent exactly zero, which
        // is the one value that puts `magnitudeAt` down its degenerate
        // branch and makes `g` unrecoverable from the coefficients. No
        // driver reports it; the guard is here so the invariant the rest of
        // this file relies on, that `g` is strictly positive and finite at
        // every setting, has no exception.
        sampleRate = (newRate > 0.0 && std::isfinite (newRate)) ? newRate : 44100.0;
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

        // Kept as it was asked for, and clamped against Nyquist in `update`
        // instead. The ceiling is a fraction of the sample rate and the rate
        // outlives this call, so a corner clamped here and read back later
        // is a corner measured against a rate that has gone: set near the
        // top of a 192 kHz band and then told the device is now at 48, this
        // held a cutoff past the new Nyquist, where the tangent is negative.
        // A negative `g` puts the poles outside the unit circle and the
        // section doubles its own output until it is not a number any more,
        // in about forty milliseconds, with nothing to say why.
        frequency = hz;

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

    /** Whether a `bellMatched` band really got the matched design, or fell
        back to the conventional one because the correction does not reach
        where it was asked to work. Always false for every other type.

        Worth exposing rather than hiding: it is the difference between a
        twelve kilohertz bell being a tenth of a decibel out and five
        decibels out, and an equaliser that draws its own curve should draw
        the one it is running. */
    bool isMatched() const noexcept { return matched; }

    /** The magnitude of the response at a frequency, exactly, from the
        filter as it is set right now.

        Here rather than in whatever wants to draw a curve, because the
        answer depends on the same six numbers the filter runs on and a
        second copy of that algebra somewhere else is a second copy that can
        disagree. An equaliser's curve is the product of its bands' answers.

        The whole of it follows from the structure. The two integrator taps
        are a bandpass and a lowpass over the same two pole denominator, and
        the output is `m0` of the input plus `m1` of the first plus `m2` of
        the second, so

            H(s) = [ m0 (s^2 + k s + 1) + m1 s + m2 ] / (s^2 + k s + 1)

        where `s` is the frequency normalised to the cutoff. The filter is
        digital, so the frequency axis is the prewarped one the bilinear
        transform gives: `s` is `j tan(pi f / fs) / g`, with `g` the same
        tangent taken at the cutoff. Which is why this reads `g` and `k` back
        out of the coefficients rather than keeping its own copies: those are
        what the running filter is actually using, including every clamp that
        was applied on the way in.
    */
    double magnitudeAt (double hz) const noexcept
    {
        if (! (a2 != 0.0) || ! (a1 != 0.0))
            return 1.0;

        // a2 is g times a1 and a3 is g times a2, so either ratio gives g,
        // and a1 is one over (1 + g(g + k)), which gives k.
        const double g = a3 / a2;
        const double k = (1.0 / a1 - 1.0 - g * g) / (g != 0.0 ? g : 1.0);

        const double highest = sampleRate * 0.5;
        const double at = hz < 0.0 ? 0.0 : (hz > highest ? highest : hz);

        // The tangent runs to infinity at Nyquist, so the top of the range
        // is approached rather than reached.
        const double t = std::tan (pi * std::fmin (at, sampleRate * 0.4999) / sampleRate);

        // s = j w, with w the prewarped frequency over the prewarped cutoff.
        const double w = t / (g != 0.0 ? g : 1.0);

        // s^2 is minus w squared, so the real and imaginary parts separate.
        const double denominatorReal = 1.0 - w * w;
        const double denominatorImaginary = k * w;

        const double numeratorReal = m0 * denominatorReal + m2;
        const double numeratorImaginary = m0 * denominatorImaginary + m1 * w;

        const double top = std::sqrt (numeratorReal * numeratorReal
                                        + numeratorImaginary * numeratorImaginary);
        const double bottom = std::sqrt (denominatorReal * denominatorReal
                                           + denominatorImaginary * denominatorImaginary);

        return bottom > 0.0 ? top / bottom : 1.0;
    }

private:
    void update() noexcept
    {
        const double A = std::pow (10.0, gain / 40.0);   // amplitude, half the dB

        // The tangent runs off to infinity at Nyquist, so the cutoff has to
        // stop short of it. 0.49 rather than 0.5 keeps g finite and leaves
        // the top of a shelf's range usable, since a shelf multiplies g.
        //
        // Here rather than in `set` so that it is measured against whatever
        // the sample rate is now. The floor goes on first and the ceiling
        // last, rather than both in one expression: written the other way
        // round, a sample rate below about two hertz leaves the one hertz
        // floor above its own ceiling, so the clamp hands back a frequency
        // past Nyquist, which is the one thing it exists to prevent.
        const double highest = sampleRate * 0.49;

        double corner = frequency < 1.0 ? 1.0 : frequency;

        if (corner > highest)
            corner = highest;

        // Everything below depends on this: with the corner floored at one
        // hertz and held under 0.49 of a finite rate, `g` is strictly
        // positive and finite, and a trapezoidally integrated state variable
        // filter with a positive `g` and a positive `k` is unconditionally
        // stable. The clamp is not tidiness, it is the stability argument.
        const double w = pi * corner / sampleRate;

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

            case Type::bellMatched:
            {
                const auto design = matchedBell (corner, resonance, gain, sampleRate);

                matched = design.matched;

                if (matched)
                {
                    g  = design.g;
                    k  = design.k;
                    m0 = design.m0;
                    m1 = design.m1;
                    m2 = design.m2;
                    break;
                }

                // Out of reach, so the conventional bell, unchanged, rather
                // than something approximate with nothing to say so. The
                // same three lines as above deliberately: two bells that
                // differ by a typo would be worse than one that is known to
                // be wrong above 13 kHz.
                k  = 1.0 / (resonance * A);
                m0 = 1.0;
                m1 = k * (A * A - 1.0);
                m2 = 0.0;
                break;
            }
        }

        if (type != Type::bellMatched)
            matched = false;

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

    bool matched = false;
};

} // namespace Fx
