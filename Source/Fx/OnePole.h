#pragma once
#include <cmath>

//==============================================================================
/** One pole, trapezoidally integrated: the half of a filter that `Svf.h`
    cannot express.

    It exists for three jobs, and all three are jobs a two pole section is the
    wrong shape for.

    **An odd order filter needs it.** A Butterworth cascade of order N is
    `floor(N/2)` conjugate pole pairs plus, when N is odd, one real pole
    (Analog Devices, *Basic Linear Design*, chapter 8, section 8.6: "Odd order
    filters will have a single-pole section"). So 6, 18, 30 and so on decibels
    per octave are unreachable without this, and a 6 dB per octave high pass
    is the single most used filter in a mix. The `Svf` can only fake a first
    order response by cancelling a pole against a zero, which is fragile in
    exactly the place it would be used.

    **A phaser needs a first order allpass.** Not a two pole one: the classic
    units are a cascade of first order sections, and the notch count follows
    from that (`Phaser.h` has the arithmetic and the sources). The allpass
    here is Smith's `AP1(s) = (s - wb) / (s + wb)`, which is the high pass
    minus the low pass, so its phase is pi at DC, pi/2 at the corner and
    nothing at Nyquist. Written that way round deliberately: the other sign
    is also an allpass and it puts the notches somewhere else.

    **A 6 dB per octave shelf is a one pole shelf.** A shelf of any slope
    other than twelve decibels per octave is not a single two pole section,
    and the gentlest one is this.

    The structure is Zavalishin's topology preserving one pole, which is the
    same trapezoidal integrator `Svf.h` is built from and keeps the same
    property: the state stays meaningful while the coefficients move, so a
    swept corner does not click.

        G = tan(pi * fc / fs)
        c = G / (1 + G)
        v = (x - s) * c
        y = v + s
        s = y + v

    with `y` the low pass and `x - y` the high pass. `OnePoleTest` measures
    all five responses out of the running section, in phase as well as in
    magnitude, and holds them against the closed form each one is specified
    by: the two passes agree to about 3e-14 of their own size and the pair
    adds back up to the input to the last bit, which is the strict statement
    that it is complementary.

    **Each shelf's corner is prewarped by its own gain, not by the square
    root of it.** A two pole shelf divides `g` by `sqrt(A)` where `A` is the
    half decibel amplitude; a one pole shelf divides by `A` itself. The half
    gain point of `(G + jw)/(1 + jw)` is at `w = sqrt(G)`, with `G` the full
    linear gain, so placing it at the frequency asked for needs `g / sqrt(G)`,
    and `sqrt(G)` is `A`. Get that wrong and the corner slides with the amount
    of boost, which is the quiet kind of wrong this codebase keeps meeting:
    the shelf is still a shelf, it is just not where the number says.

    No JUCE.
*/
namespace Fx
{

class OnePole
{
public:
    enum class Type
    {
        lowpass = 0,
        highpass,
        allpass,        /**< Smith's AP1: pi of phase at DC, none at Nyquist */
        lowShelf,
        highShelf
    };

    /** The coefficients are built up front so that a section nobody has
        configured is the filter its own members describe, a one kilohertz low
        pass at 44.1 kHz, rather than two different wrong answers: `c` left at
        zero returns silence from `process` while `magnitudeAt` takes its
        degenerate branch and reports unity. */
    OnePole() noexcept { update(); }

    void setSampleRate (double newRate) noexcept
    {
        sampleRate = newRate > 0.0 ? newRate : 44100.0;
        update();
    }

    double getSampleRate() const noexcept { return sampleRate; }

    /** `gainDb` is read only by the two shelves. */
    void set (Type newType, double hz, double gainDb = 0.0) noexcept
    {
        type = newType;

        // Kept as it was asked for and clamped in `update()` instead, against
        // whatever the sample rate is by then. See the ceiling there.
        frequency = hz;

        // Past this the arithmetic below leaves what a float buffer can carry,
        // and nothing musical lives out there. The gain does not depend on the
        // rate, so unlike the corner it can be clamped once and kept.
        gain = gainDb < -120.0 ? -120.0 : (gainDb > 120.0 ? 120.0 : gainDb);

        update();
    }

    void reset() noexcept { state = 0.0; }

    double process (double input) noexcept
    {
        const double v = (input - state) * c;
        const double low = v + state;

        state = low + v;

        return m0 * input + m1 * low;
    }

    /** The exact magnitude of the response as the filter is set right now,
        on the prewarped axis, for the same reason `Svf` has one: a curve
        drawn from a second copy of this algebra is a second copy that can
        disagree. */
    double magnitudeAt (double hz) const noexcept
    {
        // A backstop that nothing reaches, kept because it costs a compare
        // and the alternative is a division by zero on a curve. The corner is
        // floored at one hertz and then held under 0.49 of the rate, so the
        // tangent is strictly positive and finite and `c` is strictly between
        // nought and one at every setting of all three arguments. Removing
        // this changes no measurement, and `OnePoleTest` says why rather than
        // leaving it to be rediscovered: it asserts the corner is three
        // decibels down whatever nonsense it was given, and three decibels
        // down is not the flat one this branch would return.
        //
        // It was reachable until the coefficients were built in the
        // constructor. An unconfigured section had `c` at zero, so this
        // returned unity while `process` returned silence.
        if (! (c > 0.0) || ! (c < 1.0))
            return 1.0;

        // c = G/(1+G), so G comes back out as c/(1-c).
        const double G = c / (1.0 - c);

        // A negative frequency is not a request either of these two can
        // answer: a magnitude is even in it and a phase is odd, so folding to
        // DC and reflecting are both defensible and neither is the thing a
        // curve is for. So nothing asserts which of them happens here, and
        // dropping the fold is a no-op that `OnePoleTest` deliberately does
        // not fail. What it does assert is that the answer stays finite and
        // the magnitude stays positive.
        const double highest = sampleRate * 0.4999;
        const double at = hz < 0.0 ? 0.0 : (hz > highest ? highest : hz);

        const double w = std::tan (pi * at / sampleRate) / G;

        // low = 1/(1+jw), so the output is m0 + m1/(1+jw), which over the
        // common denominator is (m0 + m1 + j m0 w) / (1 + jw).
        const double numeratorReal = m0 + m1;
        const double numeratorImaginary = m0 * w;

        const double top = std::sqrt (numeratorReal * numeratorReal
                                        + numeratorImaginary * numeratorImaginary);
        const double bottom = std::sqrt (1.0 + w * w);

        return top / bottom;
    }

    /** The phase of the response, in radians, which the allpass exists for
        and which nothing can be checked about without. */
    double phaseAt (double hz) const noexcept
    {
        if (! (c > 0.0) || ! (c < 1.0))
            return 0.0;

        const double G = c / (1.0 - c);

        const double highest = sampleRate * 0.4999;
        const double at = hz < 0.0 ? 0.0 : (hz > highest ? highest : hz);

        const double w = std::tan (pi * at / sampleRate) / G;

        return std::atan2 (m0 * w, m0 + m1) - std::atan2 (w, 1.0);
    }

private:
    void update() noexcept
    {
        // The full linear gain, not the half decibel amplitude: a one pole
        // shelf's corner is set by the square root of this, where a two pole
        // shelf's is set by the fourth root. See the header comment.
        const double linear = std::pow (10.0, gain / 20.0);

        // The same ceiling `Svf` uses and for the same reason: the tangent
        // runs off to infinity at Nyquist.
        //
        // It is applied here, on the way to the coefficient, rather than once
        // in `set`, because what counts as too high depends on the rate and
        // the rate outlives the call that set the corner. Clamped in `set`
        // alone, a section given a corner near the top of a 192 kHz band and
        // then handed 48 kHz keeps a corner past the new Nyquist, where the
        // tangent is negative: `c` comes out negative with it, the pole lands
        // outside the unit circle and the section runs away to infinity in a
        // few seconds. Nothing reports that. The mixer simply fills with
        // noise, and the driver change that caused it was several minutes
        // earlier.
        //
        // The ceiling is applied after the floor rather than beside it,
        // because below about two hertz of sample rate the one hertz floor is
        // itself above the ceiling.
        const double wanted = frequency < 1.0 ? 1.0 : frequency;
        const double highest = sampleRate * 0.49;

        double G = std::tan (pi * std::fmin (wanted, highest) / sampleRate);

        switch (type)
        {
            case Type::lowpass:   m0 = 0.0; m1 = 1.0;  break;
            case Type::highpass:  m0 = 1.0; m1 = -1.0; break;

            // The high pass minus the low pass, which is (s - 1)/(s + 1) on
            // the normalised axis. Twice the low pass taken off the input is
            // the same thing and is one multiply.
            case Type::allpass:   m0 = 1.0; m1 = -2.0; break;

            case Type::lowShelf:
                G  = G / std::sqrt (linear);
                m0 = 1.0;
                m1 = linear - 1.0;
                break;

            case Type::highShelf:
                G  = G * std::sqrt (linear);
                m0 = linear;
                m1 = 1.0 - linear;
                break;
        }

        c = G / (1.0 + G);
    }

    static constexpr double pi = 3.14159265358979323846;

    double sampleRate = 44100.0;
    double frequency  = 1000.0;
    double gain       = 0.0;
    Type   type       = Type::lowpass;

    double c  = 0.0;
    double m0 = 0.0, m1 = 1.0;
    double state = 0.0;
};

} // namespace Fx
