#pragma once
#include <cmath>
#include <cstddef>
#include <vector>

//==============================================================================
/** Running a nonlinearity at a multiple of the sample rate, and bringing the
    result back down.

    Anything that bends a waveform makes harmonics that were not there before,
    and a digital system has nowhere to put the ones above half the sample
    rate: they fold back down onto frequencies that are not multiples of the
    note. The fix is to do the bending at a higher rate, where there is room,
    and to filter before coming back down.

    Used by the wavefolder, and by the distortions when they arrive. Kept
    separate from both because the filter design is the part that decides how
    well any of them work, and because getting it wrong is invisible: a
    filter with a poor stopband leaks the aliasing straight back in, and the
    oversampling then costs four times the arithmetic for nothing. That
    happened while measuring the folder: an early probe used a Blackman
    window, whose stopband is 58 dB down, and every measurement sat at -55 dB
    however much oversampling was thrown at it. The filter was the floor, not
    the folder.

    Built as a chain of doubling stages rather than one filter at the full
    factor. A stage going from one rate to twice it only has to separate the
    band it keeps from the band it discards across a gap half the width of
    the whole spectrum, where a single filter at four times has to do it
    across a gap four times narrower, and the number of taps goes up roughly
    in proportion. Two cheap stages beat one expensive one.

    The window is Kaiser, which is the standard way to trade stopband depth
    against transition width with one number. `OversamplerTest` measures the
    stopband that comes out rather than taking the design on trust.

    No JUCE.
*/
namespace Fx
{

class Oversampler
{
public:
    /** How much headroom above the audible band the filters leave.

        The transition from passband to stopband has to happen somewhere, and
        every hertz of it is either lost from the top of the audible band or
        spent on taps. 20 kHz is above what anybody hears and leaves the whole
        span from there to 24 kHz to turn over in.
    */
    static constexpr double passbandHz = 20000.0;

    /** How far down the stopband has to be. A hundred decibels is below
        anything the nonlinearity in front of this will produce, so the filter
        is never what sets the floor. */
    static constexpr double stopbandDb = 100.0;

    /** Taps are not fixed: each stage is given what Kaiser's own estimate
        says it needs for the transition it actually has to make, which
        differs enormously between stages. The outermost stage has to turn
        over between 20 and 28 kHz and needs about eighty taps. The one above
        it has from 20 to 76 kHz to do it in and needs about twenty. Giving
        them all the same number would mean paying the outermost stage's
        price four times over for nothing. */
    static constexpr int fewestTaps = 16;
    static constexpr int mostTaps = 256;

    /** `factor` is rounded down to a power of two, because the whole point of
        the design is that it is a chain of doublings. One means no
        oversampling at all, and is allowed so a caller can turn it off
        without a second code path. */
    void prepare (double sampleRate, int factor, int maximumBlockSize)
    {
        baseRate = sampleRate > 0.0 ? sampleRate : 44100.0;

        int stages = 0;

        for (int f = factor > 1 ? factor : 1; f > 1; f /= 2)
            ++stages;

        up.clear();
        down.clear();
        up.resize ((std::size_t) stages);
        down.resize ((std::size_t) stages);

        double rate = baseRate;

        tapsUsed.assign ((std::size_t) stages, 0);

        for (int i = 0; i < stages; ++i)
        {
            // Each stage's filter runs at the rate it produces, and has to
            // pass the audible band and stop everything that would fold back
            // into it when the rate is halved again.
            //
            // Where that stopband starts is the part worth deriving rather
            // than assuming. Coming down from twice R to R, content at a
            // frequency f between R/2 and R folds to R minus f. That is only
            // a problem if it lands inside the audible band, which needs
            // R minus f below the passband edge, which is f above R minus the
            // passband edge. So the stopband starts at R minus 20 kHz, not at
            // R over 2.
            //
            // The difference is large and it is the whole cost of the filter.
            // At a 48 kHz base rate the outermost stage has to turn over
            // between 20 and 28 kHz rather than between 20 and 24, which is
            // twice the room, and the stage above it between 20 and 76 kHz,
            // which is seven times. Aiming every stage at Nyquist instead
            // measured a stopband of -34 dB with the taps that the right
            // specification reaches -100 dB with.
            const double produced = rate * 2.0;
            const double stop = rate - passbandHz;

            const auto taps = design (passbandHz, stop, produced);

            tapsUsed[(std::size_t) i] = (int) taps.size();

            up[(std::size_t) i].setTaps (taps, true);
            down[(std::size_t) i].setTaps (taps, false);

            rate = produced;
        }

        oversampledRate = rate;
        const std::size_t needed = (std::size_t) (maximumBlockSize > 0 ? maximumBlockSize : 1)
                                 * (std::size_t) ratio();
        scratch.assign (needed, 0.0);
        work.assign (needed, 0.0);
    }

    int ratio() const noexcept
    {
        int r = 1;

        for (std::size_t i = 0; i < up.size(); ++i)
            r *= 2;

        return r;
    }

    double getOversampledRate() const noexcept { return oversampledRate; }

    void reset() noexcept
    {
        for (auto& s : up)
            s.reset();

        for (auto& s : down)
            s.reset();
    }

    /** Spreads `count` samples out to `count * ratio()` and hands back the
        buffer holding them. The buffer belongs to this object and stays valid
        until the next call. */
    double* upsample (const double* input, int count) noexcept
    {
        const std::size_t needed = (std::size_t) count * (std::size_t) ratio();

        if (scratch.size() < needed)
            return nullptr;             // prepare was called with a smaller block

        for (int i = 0; i < count; ++i)
            scratch[(std::size_t) i] = input[i];

        int have = count;

        for (std::size_t stage = 0; stage < up.size(); ++stage)
        {
            // Into a second buffer and then swapped, rather than expanded in
            // place. In place would mean walking backwards, because the
            // destination of sample i is 2i and would overwrite samples not
            // yet read, and walking backwards feeds a filter its input in
            // reverse: its history then holds the future rather than the
            // past. That is not a subtle error, it is a different filter, and
            // it measured as a 9 dB tilt across the passband and a stopband
            // 60 dB worse than designed.
            for (int i = 0; i < have; ++i)
            {
                work[(std::size_t) (2 * i)] = up[stage].process (scratch[(std::size_t) i]);

                // The zero is what makes this interpolation: the filter is
                // what fills it in.
                work[(std::size_t) (2 * i + 1)] = up[stage].process (0.0);
            }

            scratch.swap (work);
            have *= 2;
        }

        return scratch.data();
    }

    /** Brings the buffer `upsample` handed out back down, writing `count`
        samples to `output`. */
    void downsample (double* output, int count) noexcept
    {
        int have = count * ratio();

        for (std::size_t stage = down.size(); stage-- > 0;)
        {
            const int half = have / 2;

            for (int i = 0; i < half; ++i)
            {
                // Both samples go through the filter, and only the first of
                // each pair is kept. Filtering before throwing one away is
                // the whole point: dropping one of every two samples without
                // filtering is exactly what folds the top half of the
                // spectrum back down.
                const double kept = down[stage].process (scratch[(std::size_t) (2 * i)]);
                down[stage].process (scratch[(std::size_t) (2 * i + 1)]);

                scratch[(std::size_t) i] = kept;
            }

            have = half;
        }

        for (int i = 0; i < count; ++i)
            output[i] = scratch[(std::size_t) i];
    }

    /** The whole delay the chain adds, in samples of the base rate.

        Needed because a linear phase filter delays what goes through it, and
        an effect that is late by a few samples has to say so or the studio's
        compensation cannot line it up.
    */
    double latencySamples() const noexcept
    {
        double total = 0.0;
        double rateMultiple = 1.0;

        for (std::size_t i = 0; i < up.size(); ++i)
        {
            rateMultiple *= 2.0;

            // Each filter's delay is half its length, at the rate it runs.
            const double atThatRate = (double) (tapsUsed[i] - 1) * 0.5;

            // Once on the way up and once on the way down.
            total += 2.0 * atThatRate / rateMultiple;
        }

        return total;
    }

private:
    /** A Kaiser windowed sinc.

        Beta is chosen from the stopband wanted, by Kaiser's own rule, rather
        than picked: for an attenuation A above 50 dB, beta is
        0.1102 times A minus 8.7.
    */
    static std::vector<double> design (double passband, double stopband, double rate)
    {
        if (rate <= 0.0 || ! (stopband > passband))
            return {};

        // Kaiser's own estimate of how many taps a given attenuation across a
        // given transition needs, rather than a number somebody liked.
        const double transition = 2.0 * pi * (stopband - passband) / rate;

        int taps = (int) std::ceil ((stopbandDb - 8.0) / (2.285 * transition));

        if (taps < fewestTaps) taps = fewestTaps;
        if (taps > mostTaps)   taps = mostTaps;

        if (taps % 2 == 0)
            ++taps;             // odd, so the delay is a whole number of samples

        std::vector<double> h ((std::size_t) taps, 0.0);

        // Cut in the middle of the gap the two edges leave.
        double cutoff = 0.5 * (passband + stopband) / (rate * 0.5);

        if (! (cutoff > 0.0))
            cutoff = 0.5;

        if (cutoff > 0.99)
            cutoff = 0.99;

        // Kaiser's rule for the window parameter that gives an attenuation.
        const double beta = 0.1102 * (stopbandDb - 8.7);
        const double i0beta = besselI0 (beta);

        double sum = 0.0;

        for (int i = 0; i < taps; ++i)
        {
            const double t = (double) i - (double) (taps - 1) * 0.5;

            const double sinc = t == 0.0 ? cutoff
                                         : std::sin (pi * cutoff * t) / (pi * t);

            const double r = 2.0 * (double) i / (double) (taps - 1) - 1.0;
            const double inside = 1.0 - r * r;

            const double window = besselI0 (beta * std::sqrt (inside > 0.0 ? inside : 0.0))
                                / i0beta;

            h[(std::size_t) i] = sinc * window;
            sum += h[(std::size_t) i];
        }

        if (sum != 0.0)
            for (auto& v : h)
                v /= sum;

        return h;
    }

    /** The modified Bessel function of the first kind, order zero, by its own
        series. It converges quickly for the arguments a Kaiser window uses,
        and writing it here keeps this header free of anything but cmath. */
    static double besselI0 (double x) noexcept
    {
        double sum = 1.0;
        double term = 1.0;

        for (int k = 1; k < 64; ++k)
        {
            term *= (x * 0.5) / (double) k;
            const double contribution = term * term;

            sum += contribution;

            if (contribution < 1e-18 * sum)
                break;
        }

        return sum;
    }

    struct Fir
    {
        /** `interpolating` normalises the two halves of the filter separately.

            An interpolator sees a zero between every input sample, so at any
            moment only every other tap has anything under it, and the two
            sets of taps take turns. Each set is what decides the gain for the
            samples it produces, so each has to add to one on its own. Scaling
            the whole filter so that all of its taps add to one leaves the two
            sets slightly unequal, and a steady input then comes out with a
            small ripple on it at half the sample rate rather than steady.

            It measured as five parts in a million on a constant, which is
            nothing to hear and is still wrong: it means a folder fed a steady
            voltage does not give back the fold of that voltage, and that is
            the one case where the answer is known exactly.
        */
        void setTaps (const std::vector<double>& h, bool interpolating)
        {
            taps.resize (h.size());

            for (std::size_t i = 0; i < h.size(); ++i)
                taps[i] = h[i];

            if (interpolating)
            {
                double even = 0.0, odd = 0.0;

                for (std::size_t i = 0; i < taps.size(); ++i)
                    (i % 2 == 0 ? even : odd) += taps[i];

                for (std::size_t i = 0; i < taps.size(); ++i)
                {
                    const double branch = i % 2 == 0 ? even : odd;

                    if (branch != 0.0)
                        taps[i] /= branch;
                }
            }

            history.assign (h.size(), 0.0);
            position = 0;
        }

        void reset() noexcept
        {
            for (auto& v : history)
                v = 0.0;

            position = 0;
        }

        double process (double input) noexcept
        {
            if (taps.empty())
                return input;

            history[(std::size_t) position] = input;

            double sum = 0.0;
            std::size_t index = (std::size_t) position;

            for (std::size_t i = 0; i < taps.size(); ++i)
            {
                sum += taps[i] * history[index];

                index = index == 0 ? history.size() - 1 : index - 1;
            }

            position = position + 1 < (int) history.size() ? position + 1 : 0;

            return sum;
        }

        std::vector<double> taps, history;
        int position = 0;
    };

    static constexpr double pi = 3.14159265358979323846;

    std::vector<Fir> up, down;
    std::vector<int> tapsUsed;
    std::vector<double> scratch, work;

    double baseRate = 44100.0;
    double oversampledRate = 44100.0;
};

} // namespace Fx
