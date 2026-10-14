#pragma once
#include "Oversampler.h"

#include <cmath>

//==============================================================================
/** Drive: the thing that makes a signal sound pushed rather than loud.

    A saturator bends the top off a waveform. What makes one musical rather
    than merely loud is that the bend is gradual, so quiet passages pass
    through almost untouched and loud ones thicken, and that turning the drive
    up changes the timbre without changing the level. Both of those are
    decisions this makes explicitly, below.

    It is here because three separate things in this studio need the same
    object: the drive on the instrument's filter, the distortion effects, and
    the saturation on a mix bus. One shared, measured implementation is worth
    more than three that each sound slightly different.

    **Three curves, and why there are three rather than one.**

    `soft` is a hyperbolic tangent. That is the curve a transistor
    differential pair actually has, which is why it is the curve the published
    ladder filter models use for the same job: Huovilainen, "Non-Linear
    Digital Implementation of the Moog Ladder Filter", DAFx-04
    (https://dafx.de/paper-archive/2004/P_061.PDF), takes it from the pair's
    transfer function and applies it at every stage. It has no corner at all,
    at any drive, so it is the one to reach for when the drive should be heard
    as weight rather than as distortion.

    `cubic` is the cubic soft clipper, `1.5x - 0.5x^3` inside the limit and
    flat outside it. It is chosen so that the slope is already zero where it
    meets the limit, which means the curve has a continuous first derivative
    and its harmonics fall away as one over the cube of their number. It is
    brighter than the tangent and much cheaper, and, as the next section
    explains, it is the one that can carry the better antialiasing.

    `hard` is a plain clip. The corner is a break in the slope, so its
    harmonics fall away only as one over the square, and it is the loudest and
    rudest of the three. It is here because that sound is sometimes the one
    that is wanted, not because it models anything.

    **Antialiasing, and the one place the curves genuinely differ.**

    Any nonlinearity makes harmonics, and a sampled system has nowhere to put
    the ones above half the sample rate: they fold back down onto frequencies
    that are not multiples of the note. So the curve is not evaluated at each
    sample. It is averaged across the step from the previous sample to this
    one, which is the difference of its antiderivative divided by the step,
    from Parker, Zavalishin and D'Angelo, "Reducing the Aliasing of Nonlinear
    Waveshaping Using Continuous-Time Convolution", DAFx-16:

        y[n] = (F0(x[n]) - F0(x[n-1])) / (x[n] - x[n-1])             (their eq. 9)

    with their own fallback where two samples are nearly equal and the
    quotient loses its significant figures:

        y[n] = f((x[n] + x[n-1]) / 2)                                (their eq. 10)

    Their second order form averages over a triangular window two samples wide
    instead, which is better again, and needs a second antiderivative: the
    integral of x times the curve.

    **The tangent can only have the first order form, and that is a fact about
    the function rather than a shortcut taken here.** Its first antiderivative
    is `log(cosh x)`, which is exact and cheap. Its second is not elementary:
    it contains a dilogarithm, `Li2`, which has no closed form in terms of the
    usual functions and has to be approximated. Worked through in full at
    https://apulsoft.ch/blog/tanh-antiderivatives/, where the symbolic result
    carries `Li2(-e^-2x)` and the author's own conclusion is that it cannot be
    reduced. The two polynomial curves have exact piecewise antiderivatives to
    any order, so they get the second order form.

    What that costs is measured rather than assumed, in `Tests/SaturatorProbe.cpp`.
    The loudest stray partial below 20 kHz, on a sine at 1109 Hz at a 48 kHz
    sample rate, where -95 dB is the measurement's own floor and not a reading:

        drive               1x      2x      4x      8x
        soft, first order
             8            -60     -95     -95     -95
            16            -43     -85     -95     -95
            32            -37     -62     -95     -95
        cubic, second
             8            -54     -92     -95     -95
            16            -44     -92     -95     -95
            32            -42     -66     -95     -95
        hard, second
             8            -63     -83     -95     -95
            16            -46     -75     -95     -95
            32            -42     -70     -95     -95

    So four times, where every curve at every drive disappears under the floor
    of the measurement, and where the latency is 43.6 samples, which is 0.9 ms
    at this rate. Two times is close and is not the same thing: the plain clip
    and the hard driven tangent are both still audible there.

    **The band the measurement counts matters as much as the measurement.**
    Read across the whole spectrum instead of up to 20 kHz, these same figures
    sit around -50 to -67 dB and stop improving after two times, which reads
    as a wall that more oversampling cannot climb. It is not one. The
    oversampler's filters are specified to stop at the oversampled rate minus
    20 kHz rather than at Nyquist, precisely so that what folds lands above
    20 kHz where nothing can hear it, and that deliberate fold is what the
    wider measurement was finding. An hour went into the wrong conclusion for
    want of saying which band the number was about.

    **The log(cosh) is not computed as written.** `cosh` overflows for inputs
    past about 710 and the logarithm then nearly cancels it, so even well
    before that most of the significant figures are gone. The identity

        log(cosh x) = |x| + log(1 + e^-2|x|) - log 2

    is exact, is evaluated with `log1p`, and holds its accuracy everywhere.
    A driven signal reaches those inputs easily: full scale at a drive of
    sixty-four is already past where the naive form has lost digits.

    **Turning the drive up must not turn the level up.** The output is divided
    by the curve's own value at the drive, so a full scale input comes back at
    full scale whatever the drive is: at a low drive that is the same as
    dividing by the small signal gain, and at a high drive it is what keeps a
    nearly square output from being nearly twice as loud as the input that
    made it. Without it, drive is a volume control with a tone side effect,
    which is not a control anybody can use.

    **Bias is how the even harmonics get in.** A symmetric curve fed a
    symmetric waveform gives back a symmetric one, and a symmetric waveform
    has only odd harmonics, which is the hollow sound every software
    distortion has. Pushing the signal off centre before the curve breaks that
    symmetry.

    The bias belongs to the curve here rather than to the signal, which is a
    distinction with two consequences worth having. The shaper is

        g(u) = f(u + bias) - f(bias)

    so it still passes through the origin: nothing in gives nothing out, at
    every bias, and no constant offset travels on into whatever follows. A DC
    offset into a filter wastes its headroom and into a gate it is a thump at
    every note.

    And because the bias is part of the function rather than part of the
    input, moving it does not look like a step in the signal. Written the
    other way, with the offset added to the input, the antialiasing sees the
    signal jump by the whole change of bias and averages across that jump: a
    bias sweep becomes a sequence of small clicks, and the first sample after
    any reset is wrong by however far the bias sits from zero. Measured at a
    bias of 0.8 that first sample came out at 0.30 with silence going in.
    The antiderivatives of `g` are exact, given the curve's own:

        G0(u) = F0(u+b) - F0(b) - f(b) u
        G1(u) = F1(u+b) - F1(b) - b (F0(u+b) - F0(b)) - f(b) u^2 / 2

    **A strong bias does make the output quieter**, and that is left alone
    rather than compensated. The makeup gain is taken at the drive and not at
    the drive plus the bias, so that the two controls do not interact: a bias
    that moved the makeup would be a second level control wearing the name of
    a tone control. What it means is that pushing the signal a long way up the
    curve leaves it with less room to swing, which is what happens in the
    circuit too.

    No JUCE below the oversampler, which has none either, so all of this is
    tested.
*/
namespace Fx
{

class Saturator
{
public:
    enum class Curve
    {
        soft = 0,   /**< a hyperbolic tangent: no corner anywhere */
        cubic,      /**< a cubic soft clip: a continuous slope at the limit */
        hard        /**< a plain clip: a corner, and the brightest of the three */
    };

    //==========================================================================
    /** The curve itself, with no antialiasing, no drive and no state. Exposed
        because it is the definition: the test compares everything else here
        against it, and it is the only description of what this does that
        cannot drift from what it does. */
    static double shape (Curve curve, double x) noexcept
    {
        switch (curve)
        {
            case Curve::soft:
                return std::tanh (x);

            case Curve::cubic:
                return std::fabs (x) >= 1.0 ? sign (x)
                                            : x * (1.5 - 0.5 * x * x);

            case Curve::hard:
                return x < -1.0 ? -1.0 : (x > 1.0 ? 1.0 : x);
        }

        return x;
    }

    /** The antiderivative of `shape`, zero at zero.

        Even in every case, because every curve here is odd.
    */
    static double integral (Curve curve, double x) noexcept
    {
        const double a = std::fabs (x);

        switch (curve)
        {
            case Curve::soft:
                // log(cosh x), by the identity in the class comment rather
                // than as written, because cosh overflows and the logarithm
                // then cancels most of what is left.
                return a + std::log1p (std::exp (-2.0 * a)) - logTwo;

            case Curve::cubic:
                return a >= 1.0 ? cubicIntegralAtOne + (a - 1.0)
                                : a * a * (0.75 - 0.125 * a * a);

            case Curve::hard:
                return a >= 1.0 ? 0.5 + (a - 1.0) : 0.5 * a * a;
        }

        return 0.5 * x * x;
    }

    /** The antiderivative of x times `shape`, which the second order
        antialiasing needs. Odd, because x times an odd function is even.

        Not available for `soft`: see the class comment. `hasSecondIntegral`
        says so, and asking for it anyway gives the first order form's answer
        rather than a wrong one.
    */
    static double secondIntegral (Curve curve, double x) noexcept
    {
        const double a = std::fabs (x);

        switch (curve)
        {
            case Curve::soft:
                break;

            case Curve::cubic:
                return sign (x) * (a >= 1.0
                                    ? cubicSecondAtOne + 0.5 * (a * a - 1.0)
                                    : a * a * a * (0.5 - 0.1 * a * a));

            case Curve::hard:
                return sign (x) * (a >= 1.0
                                    ? (1.0 / 3.0) + 0.5 * (a * a - 1.0)
                                    : a * a * a / 3.0);
        }

        return 0.0;
    }

    /** Whether this curve can carry the second order antialiasing, which is
        the same question as whether its second antiderivative is elementary. */
    static bool hasSecondIntegral (Curve curve) noexcept
    {
        return curve != Curve::soft;
    }

    /** The curve's slope at the origin, which is what a small signal is
        multiplied by. The tangent and the clip pass a quiet signal through
        untouched; the cubic does not, and anything comparing curves has to
        know that rather than discover it. */
    static double slopeAtZero (Curve curve) noexcept
    {
        return curve == Curve::cubic ? 1.5 : 1.0;
    }

    //==========================================================================
    /** For standalone use, where this does its own oversampling.

        `factor` is settable only so that `SaturatorProbe` can measure what it
        buys. Everything in the studio leaves it alone and gets the figure the
        measurement settled on.
    */
    void prepare (double sampleRate, int maximumBlockSize,
                  int factor = oversamplingFactor)
    {
        rate = sampleRate > 0.0 ? sampleRate : 44100.0;
        oversampler.prepare (rate, factor, maximumBlockSize);
        oversampled = oversampler.getOversampledRate();
        reset();
    }

    /** For a caller that already runs oversampled and will use
        `processOneAtCurrentRate`, such as an instrument voice whose whole
        nonlinear section is at a higher rate. Nothing is allocated and no
        filtering is done here: the antialiasing of the curve itself still is,
        because that part is free. */
    void prepareAtRate (double rateItWillBeCalledAt) noexcept
    {
        rate = rateItWillBeCalledAt > 0.0 ? rateItWillBeCalledAt : 44100.0;
        oversampled = rate;
        history = {};
    }

    void reset() noexcept
    {
        oversampler.reset();
        history = {};
    }

    void setCurve (Curve newCurve) noexcept
    {
        if (newCurve == curve)
            return;

        curve = newCurve;
        recompute();
        reseedHistory();
    }

    Curve getCurve() const noexcept { return curve; }

    /** How hard the signal is driven in. One is the edge of the curve, and the
        useful range runs to about sixty-four, which on every curve here is a
        waveform that is nearly square. */
    void setDrive (double amount) noexcept
    {
        const double wanted = amount < smallestDrive ? smallestDrive
                                                     : (amount > 64.0 ? 64.0 : amount);

        // Unchanged is the common case and it has to be free. A voice calls
        // this once a sample, because the drive is a modulation destination
        // and might be moving, and on the great majority of patches it is
        // not moving at all. Recomputing anyway cost a hyperbolic tangent
        // and a pair of exponentials per sample per voice for nothing, which
        // measured as a fifth of a processor core across sixteen voices.
        if (wanted == drive)
            return;

        drive = wanted;
        recompute();
    }

    /** An offset added before the curve, which is what brings the even
        harmonics in. Minus one to one, where either end is most of the way up
        the curve and the sound is close to a rectifier. */
    void setBias (double offset) noexcept
    {
        const double wanted = offset < -1.0 ? -1.0 : (offset > 1.0 ? 1.0 : offset);

        // As with the drive, and more so: a bias change also re-reads the
        // history, so an unconditional setter called once a sample pays for
        // four evaluations of the antiderivatives to arrive at the numbers
        // it already had.
        if (wanted == bias)
            return;

        bias = wanted;
        recompute();
        reseedHistory();
    }

    double latencySamples() const noexcept
    {
        // The oversampler's filters, plus what the antialiasing itself costs,
        // which is half a sample for the first order form and one for the
        // second, at the oversampled rate.
        const double order = hasSecondIntegral (curve) ? 1.0 : 0.5;

        return oversampler.latencySamples() + order / (double) oversampler.ratio();
    }

    //==========================================================================
    /** Drives a block in place. */
    void process (double* samples, int count) noexcept
    {
        if (count <= 0)
            return;

        double* high = oversampler.upsample (samples, count);

        if (high == nullptr)
            return;                 // prepared for a smaller block than this

        const int total = count * oversampler.ratio();

        for (int i = 0; i < total; ++i)
            high[i] = driveSample (high[i]);

        oversampler.downsample (samples, count);
    }

    /** One sample at a time, for a caller that oversamples itself. */
    double processOneAtCurrentRate (double input) noexcept
    {
        return driveSample (input);
    }

private:
    //==========================================================================
    double driveSample (double input) noexcept
    {
        const double u = input * drive;

        const double shaped = hasSecondIntegral (curve) ? secondOrder (u)
                                                        : firstOrder (u);

        return shaped * makeup;
    }

    /** The shaper, which is the curve offset by the bias and brought back
        through the origin. See the class comment for why the bias lives here
        rather than on the input. */
    double biasedShape (double u) const noexcept
    {
        return shape (curve, u + bias) - atBias;
    }

    double biasedIntegral (double u) const noexcept
    {
        return integral (curve, u + bias) - integralAtBias - atBias * u;
    }

    double biasedSecondIntegral (double u) const noexcept
    {
        // Nothing reads this for a curve that has no second antiderivative,
        // and computing it anyway is not free: the expression below calls
        // `integral` twice, which for the tangent is an exponential and a
        // logarithm each time, to scale a term that is zero.
        if (! hasSecondIntegral (curve))
            return 0.0;

        return secondIntegral (curve, u + bias) - secondAtBias
                 - bias * (integral (curve, u + bias) - integralAtBias)
                 - 0.5 * atBias * u * u;
    }

    /** Their equation 9, with equation 10 where the quotient is ill
        conditioned. */
    double firstOrder (double x) noexcept
    {
        const double f0 = biasedIntegral (x);
        const double step = x - history.x1;

        const double out = std::fabs (step) < illConditioned
                             ? biasedShape (0.5 * (x + history.x1))
                             : (f0 - history.f0a) / step;

        history.x2 = history.x1;
        history.f0b = history.f0a;
        history.f1b = history.f1a;

        history.x1 = x;
        history.f0a = f0;
        history.f1a = biasedSecondIntegral (x);

        return out;
    }

    /** Their equation 14, with 15 and 16 as the fallbacks. The same form the
        wavefolder uses, and the comment there has the algebra. */
    double secondOrder (double x) noexcept
    {
        const double f0 = biasedIntegral (x);
        const double f1 = biasedSecondIntegral (x);

        const double toPrevious = x - history.x1;
        const double fromBefore = history.x2 - history.x1;

        double sum = 0.0;

        if (std::fabs (toPrevious) < illConditioned)
            sum += 0.5 * biasedShape ((x + 2.0 * history.x1) / 3.0);
        else
            sum += (x * (f0 - history.f0a) - (f1 - history.f1a))
                 / (toPrevious * toPrevious);

        if (std::fabs (fromBefore) < illConditioned)
            sum += 0.5 * biasedShape ((history.x2 + 2.0 * history.x1) / 3.0);
        else
            sum += (history.x2 * (history.f0b - history.f0a)
                      - (history.f1b - history.f1a))
                 / (fromBefore * fromBefore);

        history.x2 = history.x1;
        history.f0b = history.f0a;
        history.f1b = history.f1a;

        history.x1 = x;
        history.f0a = f0;
        history.f1a = f1;

        return sum;
    }

    /** The two numbers that depend on the drive, the bias and the curve, kept
        rather than recomputed per sample.

        `makeup` is one over the curve at the drive, which is what keeps the
        level where it was: see the class comment. It is taken at the drive
        rather than at the drive plus the bias, because the bias should change
        the timbre and not the level, and a bias that moved the makeup would
        make the two controls interact.
    */
    void recompute() noexcept
    {
        atBias = shape (curve, bias);
        integralAtBias = integral (curve, bias);
        secondAtBias = secondIntegral (curve, bias);

        const double atDrive = std::fabs (shape (curve, drive));

        makeup = atDrive > 1.0e-12 ? 1.0 / atDrive : 1.0;
    }

    /** Re-reads the antiderivatives the history is holding, for the shaper as
        it is now.

        Called whenever the shaper itself changes, which is a change of curve
        or of bias. The history carries the integrals at the last two inputs,
        and the next sample's difference quotient subtracts them from the new
        shaper's: left alone they would make that one quotient a difference
        between two different functions, which is a single wrong sample, a
        click, arriving exactly when somebody turns a knob. Recomputing them
        at the inputs that are already stored costs four evaluations and is
        exact.
    */
    void reseedHistory() noexcept
    {
        history.f0a = biasedIntegral (history.x1);
        history.f1a = biasedSecondIntegral (history.x1);
        history.f0b = biasedIntegral (history.x2);
        history.f1b = biasedSecondIntegral (history.x2);
    }

    static double sign (double x) noexcept { return x < 0.0 ? -1.0 : 1.0; }

    /** Below this the difference quotients are two nearly equal numbers over a
        nearly zero one. The paper gives the fallbacks and no threshold; this
        is where the quotient still has about half its digits in double
        precision, and is the same figure the wavefolder uses. */
    static constexpr double illConditioned = 1.0e-5;

    /** A drive of exactly zero would be a curve that outputs the bias and
        nothing else, and a makeup of one over nothing. The floor is low enough
        that the output is the input to within a part in ten thousand, which is
        what somebody turning the drive to its minimum means. */
    static constexpr double smallestDrive = 1.0e-4;

    /** Measured, not chosen: see the table in the class comment. */
    static constexpr int oversamplingFactor = 4;

    static constexpr double logTwo = 0.69314718055994530942;

    /** The cubic's antiderivatives where it meets the limit, so the two
        branches join exactly rather than nearly. 0.75 - 0.125 and 0.5 - 0.1. */
    static constexpr double cubicIntegralAtOne = 0.625;
    static constexpr double cubicSecondAtOne = 0.4;

    struct History
    {
        double x1 = 0.0, x2 = 0.0;
        double f0a = 0.0, f1a = 0.0;    /**< the integrals at x1 */
        double f0b = 0.0, f1b = 0.0;    /**< and at x2 */
    };

    Oversampler oversampler;

    Curve  curve = Curve::soft;
    double drive = 1.0;
    double bias = 0.0;

    double makeup = 1.0 / 0.76159415595576488812;    // one over tanh(1)

    /** The shaper's three constants at the current bias, kept rather than
        recomputed per sample. */
    double atBias = 0.0, integralAtBias = 0.0, secondAtBias = 0.0;

    double rate = 44100.0;
    double oversampled = 44100.0;

    History history;
};

} // namespace Fx
