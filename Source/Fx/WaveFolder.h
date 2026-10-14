#pragma once
#include "Oversampler.h"

#include <cmath>

//==============================================================================
/** A wavefolder: the west coast way of making a sound brighter.

    A filter takes harmonics away. A folder puts them in. Anything that goes
    past the limit is sent back the other way instead of being flattened, so
    driving the input harder does not make it louder, it makes it fold more
    times per cycle, and each fold adds harmonics. Turning the drive up is a
    timbre control that happens to leave the level roughly where it was.

    It is the sound of a Buchla, of the Serge wave multipliers, and of the
    Multiply section of a 0-Coast, and it does something no filter can: a
    folded sine is mostly odd harmonics, and they come and go as the drive
    moves rather than simply rolling off. Sweeping a folder sounds like
    nothing else.

    **What this models, and what it does not.** The 0-Coast's own folder
    circuit is not published, so this is not a model of it. What is published
    is the behaviour of the class, in Esqueda, Pöntynen, Välimäki and
    Bilbao's virtual analog models of the Lockhart and Serge wavefolders
    (Applied Sciences 7(12), 2017, https://doi.org/10.3390/app7121328), and in
    particular that these circuits are built as folding stages with gain
    between them, and that the folding characteristic of a transistor pair is
    rounded rather than sharp.

    So the characteristic here is a sinusoidal fold, which is what a
    transistor pair approximates and what the Buchla style circuits are
    closest to. It is not claimed to be a particular module: writing
    "0-Coast mode" on something reverse engineered by ear would be the kind
    of borrowed authority CLAUDE.md warns about.

    **A triangle fold was built alongside it and then taken out.** The ideal
    limit of a folder, where the characteristic turns in a corner rather than
    a curve, is the obvious second flavour and it is brighter and more
    aggressive. It is also unusable. Measured as the highest note whose stray
    partials stay below -70 dB:

        drive          1     2     3     4     6     8
        sinusoidal  4494  3199  2550  2033  1447  1153 Hz
        triangle    7920   371   371   236     0     0 Hz

    At any drive that actually folds, the triangle is dirty almost everywhere
    on the keyboard. A corner is a break in the slope rather than in the
    value, so it throws harmonics that fall away only as one over the square
    of their number, and they run far past where four times oversampling can
    hold them. Eight and sixteen times were tried and give the same answer.

    It is no loss, because a corner is not a thing a wavefolder has. The
    circuits in the paper above fold through transistor curves, which are
    rounded; the sharp triangle is a mathematical idealisation that no
    hardware produces. Shipping it would have meant offering a mode that
    sounds broken in order to model something that does not exist.

    **Antialiasing, which is most of the work here.** Folding generates
    enormous harmonic content, and a digital system has nowhere to put what
    lands above half the sample rate: it folds back down onto frequencies
    that are not multiples of the note. Measured on a sine at 1760 hertz and
    four times drive, a naive sharp fold puts stray partials 20 dB below the
    fundamental, which is not a subtle artefact, it is a second sound.

    Two things are done about it and they are complementary.

    The nonlinearity is evaluated by first order antiderivative
    antialiasing, from Parker, Zavalishin and D'Angelo, "Reducing the
    Aliasing of Nonlinear Waveshaping Using Continuous-Time Convolution",
    DAFx-16. Instead of taking the fold of this sample, it takes the average
    of the fold across the step from the last sample to this one, which is
    the difference of the fold's antiderivative divided by the step:

        y[n] = (F0(x[n]) - F0(x[n-1])) / (x[n] - x[n-1])            (their eq. 9)

    with the obvious problem when two samples are nearly equal, where the
    paper's own fallback is used:

        y[n] = f((x[n] + x[n-1]) / 2)                               (their eq. 10)

    Both characteristics here were chosen partly because their antiderivatives
    are exact closed forms, so this costs almost nothing. The method delays
    the signal by about half a sample.

    Second order rather than first, because first was tried and the
    measurement said it was not enough: at four times oversampling it left
    stray partials around 40 dB down on the harder settings, and eight and
    sixteen times gave the same answer to within a fifth of a decibel. The
    oversampling was not the limit, the order of the antialiasing was.

    And the whole thing runs at four times the sample rate, which is where
    the measurement stops improving.

    **One honest limit.** None of this rescues a high note driven hard. The
    folded waveform's harmonics eventually run past where any practical rate
    can hold them, and that is a property of folding rather than of this
    implementation: the analogue circuit has no such problem because it is
    not sampled at all. Folding is a bass and midrange instrument, which is
    where it is used, and `foldsCleanlyBelow` reports the edge, fitted to the
    measurement, so a patch or an interface can keep off it.

    No JUCE below the oversampler, which has none either, so all of this is
    tested.
*/
namespace Fx
{

class WaveFolder
{
public:
    //==========================================================================
    /** The folding characteristic itself, with no antialiasing and no state.
        Exposed because it is the definition: a test can compare everything
        else against it. */
    static double foldOnce (double x) noexcept
    {
        return std::sin (pi * x * 0.5);
    }

    /** The antiderivative of `foldOnce`, zero at zero. */
    static double foldIntegral (double x) noexcept
    {
        return (2.0 / pi) * (1.0 - std::cos (pi * x * 0.5));
    }

    /** The antiderivative of x times `foldOnce`, which the second order
        antialiasing needs, again exact and again zero at zero.

        The integral of x sin(ax) is (sin(ax) minus ax cos(ax)) over a
        squared, with a of pi over two.
    */
    static double foldSecondIntegral (double x) noexcept
    {
        const double a = pi * 0.5;

        return (std::sin (a * x) - a * x * std::cos (a * x)) / (a * a);
    }

    //==========================================================================
    void prepare (double sampleRate, int maximumBlockSize)
    {
        rate = sampleRate > 0.0 ? sampleRate : 44100.0;
        oversampler.prepare (rate, oversamplingFactor, maximumBlockSize);
        reset();
    }

    /** For a caller that does its own oversampling and will call
        `processOneAtCurrentRate`, with `oversampledRate` being the rate it
        will call at.

        Worth having as its own entry point rather than leaving a caller to
        use `prepare` and then not use the oversampler it built. An instrument
        voice runs the whole of its nonlinear section at a multiple of the
        sample rate, so the folder inside it must not oversample again, and it
        must still know the rate it is actually running at: `foldsCleanlyBelow`
        is a figure about a sample rate, and a folder told 48 kHz while
        running at 192 would report a quarter of the range it really has.
    */
    void prepareAtRate (double oversampledRate) noexcept
    {
        rate = oversampledRate > 0.0 ? oversampledRate : 44100.0;
        history = {};
    }

    void reset() noexcept
    {
        oversampler.reset();
        history = {};
    }

    /** How hard the signal is driven into the folder. One is the edge of
        folding at all and the useful range runs to about sixteen, past which
        there are more folds per cycle than any sample rate can carry. */
    void setDrive (double amount) noexcept
    {
        drive = amount < 0.0 ? 0.0 : (amount > 64.0 ? 64.0 : amount);
    }

    /** An offset added before folding.

        A folder fed a symmetric waveform gives back a symmetric one, and a
        symmetric waveform has only odd harmonics. Pushing it off centre
        breaks that and brings the even harmonics in, which is the difference
        between a hollow sound and a full one, and is the control that is
        missing from most software folders.
    */
    void setSymmetry (double offset) noexcept
    {
        symmetry = offset < -1.0 ? -1.0 : (offset > 1.0 ? 1.0 : offset);
    }

    /** The highest note that folds without audible aliasing at the current
        drive, in hertz.

        Not a limiter: nothing is clamped to it. It is here so a patch, a
        test or an interface can know where the edge is rather than finding
        it by ear. The rule comes from the measurement above: a fold lands
        its harmonics out to roughly the drive times the fundamental times
        the number of corners a cycle has, and four times oversampling holds
        that as far as about a fifth of the oversampled Nyquist.
    */
    double foldsCleanlyBelow() const noexcept
    {
        // Fitted to the measurement rather than reasoned about. The highest
        // note whose stray partials stay below -70 dB, at a 48 kHz sample
        // rate, measured at drives from one to sixteen:
        //
        //     drive    1     2     3     4     6     8    12    16
        //     hertz  4494  3199  2550  2033  1447  1153   821   655
        //
        // which is close to a constant over the drive plus a bit, and the
        // constant here is pulled down from the fit so the figure is a little
        // pessimistic at every drive rather than a little optimistic at some.
        return (rate / 48000.0) * 10500.0 / (1.6 + drive);
    }

    double latencySamples() const noexcept
    {
        // The oversampler's filters, plus the sample the second order
        // antialiasing costs, which runs at the oversampled rate.
        return oversampler.latencySamples() + 1.0 / (double) oversamplingFactor;
    }

    //==========================================================================
    /** Folds a block in place. */
    void process (double* samples, int count) noexcept
    {
        if (count <= 0)
            return;

        double* high = oversampler.upsample (samples, count);

        if (high == nullptr)
            return;                 // prepared for a smaller block than this

        const int total = count * oversampler.ratio();

        for (int i = 0; i < total; ++i)
            high[i] = foldSample (high[i]);

        oversampler.downsample (samples, count);
    }

    /** One sample at a time, for a caller that already runs oversampled, such
        as a voice whose whole chain is at a higher rate. No oversampling and
        no filtering is done here: the antialiasing of the nonlinearity itself
        is, because that part is free. */
    double processOneAtCurrentRate (double input) noexcept
    {
        return foldSample (input);
    }

private:
    /** The paper's second order form, their equation 14, which averages the
        fold over a triangular window two samples wide rather than a
        rectangular one sample wide.

            y[n] = [ x[n] (F0(x[n]) - F0(x[n-1])) - (F1(x[n]) - F1(x[n-1])) ]
                   / (x[n] - x[n-1])^2
                 + [ x[n-2] (F0(x[n-2]) - F0(x[n-1])) - (F1(x[n-2]) - F1(x[n-1])) ]
                   / (x[n-2] - x[n-1])^2

        with their equations 15 and 16 as the fallbacks where two consecutive
        inputs are nearly equal and the quotient is two close numbers over a
        very small one.

        First order was tried first and is not enough here. Measured across
        notes from 169 Hz to 3 kHz and drives from two to eight, first order
        at four times oversampling left stray partials at -40 dB, and eight
        and sixteen times gave the same answer to within a fifth of a
        decibel: the oversampling was not the limit, the order of the
        antialiasing was. Folding generates far more harmonic content than an
        ordinary distortion and leans on this much harder.

        It costs about one sample of delay rather than half, at the
        oversampled rate, which is a sixteenth of a sample of the real one.
    */
    double foldSample (double input) noexcept
    {
        const double x = input * drive + symmetry;

        const double f0 = foldIntegral (x);
        const double f1 = foldSecondIntegral (x);

        const double toPrevious = x - history.x1;
        const double fromBefore = history.x2 - history.x1;

        double sum = 0.0;

        if (std::fabs (toPrevious) < illConditioned)
            sum += 0.5 * foldOnce ((x + 2.0 * history.x1) / 3.0);
        else
            sum += (x * (f0 - history.f0a) - (f1 - history.f1a))
                 / (toPrevious * toPrevious);

        if (std::fabs (fromBefore) < illConditioned)
            sum += 0.5 * foldOnce ((history.x2 + 2.0 * history.x1) / 3.0);
        else
            sum += (history.x2 * (history.f0b - history.f0a) - (history.f1b - history.f1a))
                 / (fromBefore * fromBefore);

        history.x2 = history.x1;
        history.f0b = history.f0a;
        history.f1b = history.f1a;

        history.x1 = x;
        history.f0a = f0;
        history.f1a = f1;

        return sum;
    }

    /** Below this the difference quotients are two nearly equal numbers over
        a nearly zero one, which loses most of their significant figures. The
        paper gives the fallbacks and no threshold. This one is where the
        quotient still has about half its digits in double precision, and the
        fallback it hands over to is accurate to within the same amount, so
        the join is smooth either way. */
    static constexpr double illConditioned = 1.0e-5;

    struct History
    {
        double x1 = 0.0, x2 = 0.0;
        double f0a = 0.0, f1a = 0.0;    /**< the integrals at x1 */
        double f0b = 0.0, f1b = 0.0;    /**< and at x2 */

    };

    static constexpr double pi = 3.14159265358979323846;

    /** Measured, not chosen. Eight and sixteen were tried and give the same
        answer as four to within a tenth of a decibel. */
    static constexpr int oversamplingFactor = 4;

    Oversampler oversampler;

    double drive = 1.0;
    double symmetry = 0.0;
    double rate = 44100.0;

    History history;
};

} // namespace Fx
