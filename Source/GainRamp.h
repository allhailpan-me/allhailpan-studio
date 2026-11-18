#pragma once

#include <algorithm>
#include <cmath>

/*
    What a gain does while it is moving.

    A mixer gain that is read once a block and applied flat across it does not
    move continuously: it steps, once per block, by however far the control
    travelled in that time. One press of a button is then a single click, and a
    control that keeps moving, a fader being ridden or a volume automation
    curve being played, is a staircase for as long as it moves. The two are
    different artifacts and the second is the one that needed a name:

    A per block constant gain is the intended gain multiplied by a zero order
    hold of itself, sampled at the block rate fb = fs / N. For a control moving
    at a constant rate the error is a sawtooth of period N/fs and peak to peak
    amplitude dg, the gain change per block, whose k'th harmonic has amplitude
    dg/(pi k). Multiplying a signal component at f0 by that error puts a pair
    of sidebands at f0 +- k fb at dg/(2 pi k) of it. So the artifact is
    inharmonic, it sits at multiples of the block rate away from every
    component of the signal, and it moves when the buffer size changes: at 48
    kHz a 1024 sample buffer spaces it 46.9 Hz away, which is a buzz, and a 64
    sample buffer 750 Hz away, which is a whistle. That is the zipper the word
    describes, and stepSidebandDb below is that arithmetic.

    This is sampling theory rather than an audio specification, and it is
    written here as a derivation rather than cited, because the research found
    no published audibility threshold for the artifact anywhere: not in the
    DAFx parameter interpolation literature, which is about filter
    coefficients, and not in any vendor's documentation. The figures below are
    therefore this project's own choices, and this comment is where that is
    admitted rather than implied otherwise.

    The law: a slew limiter on the linear gain, clamped against the target, its
    state held across blocks.

    Each of those four words was decided against the two references that
    publish their arithmetic as code, Ardour at c453ffb4 and JUCE at 9.0.3.

    LINEAR, not decibels. A fader has to reach silence and a mute has to reach
    silence, and a gain smoothed in the decibel domain cannot: JUCE says so in
    juce_SmoothedValue.h:236, "Note that when you are using multiplicative
    smoothing you cannot ever reach a target value of zero!", and asserts on it
    rather than only documenting it. Its own dsp::Gain and dsp::Panner use the
    Linear type despite offering Multiplicative, Ardour's Amp smooths the gain
    coefficient (amp.cc:195-213), and the Web Audio API throws on an
    exponential ramp to zero. The decibel domain is for a parameter whose scale
    is multiplicative and which never reaches zero: a cutoff, a frequency, a
    delay time.

    A SLEW LIMITER, not a one pole. Ardour's is a one pole at a 25 Hz corner,
    a = 156.825f / sample_rate, a time constant of 6.3765 ms at every sample
    rate. A one pole never arrives, so Ardour carries an epsilon snap at a
    linear gain of 1e-5 in four separate places to make it terminate
    (amp.cc:136, 219, 285 and delivery.cc:354). A slew limiter arrives because
    it is clamped, which is worth more here than the shape: a control left
    alone then sits exactly on its own gain, so a steady mix is one exact
    multiply and is reproducible, and a mute is actually silent rather than 100
    dB down. It is also the law the solo and input monitoring ramps in this
    engine already follow, so there is one law rather than two shapes meeting
    in the same multiply.

    CLAMPED AGAINST THE TARGET, which is what makes arrival exact, and is also
    what makes a dead band unnecessary. Ardour needs one because its one pole
    would otherwise creep forever; a change here smaller than one block's
    travel completes inside that block and lands on the target exactly.

    STATE HELD ACROSS BLOCKS, which is the one that matters most and is the
    reason this is a slew rate rather than a ramp across the buffer. Ramping
    from the last block's gain to this one's across the whole buffer needs no
    state and always arrives by the end of the block, and it makes the
    smoothing time a function of the buffer size: 1.33 ms at 64 samples and
    42.7 ms at 2048, at 48 kHz. Playback and export are two code paths over the
    same graph here, so that would make a bounce differ from what was heard and
    differ again at another buffer size, which is the fault CLAUDE.md calls
    among the worst a studio can have. Neither reference does it: Ardour holds
    Amp::_current_gain and JUCE holds SmoothedValue::currentValue, both as
    members, and both let a move take as many blocks as it takes.

    The other side of holding state across blocks is that the state has to be
    put where it is going at any discontinuity, or the discontinuity becomes a
    fade. Ardour says this twice: disk_reader.cc:348-354 turns its declick off
    and sets the gain straight to the target when exporting offline, and
    amp.cc:409-428 reseeds _current_gain from the first sample of the
    automation curve whenever the transport is not where it expected to be,
    which includes the first block of a bounce. That is what latch is for.

    No JUCE in here, so all of it is tested: the gain a sample gets is the one
    thing in a mixer that cannot announce that it is wrong. A ramp that is half
    a millisecond too short does not error, does not look wrong on a meter, and
    sounds like the mix.
*/
namespace GainRamp
{
    /** How long a gain takes to cross unity, and so the time any move within
        a fader's normal range takes. A slew rate rather than a fixed ramp
        length, which is the same choice the solo and monitoring ramps already
        made: a small move completes proportionally sooner, and a move from the
        fader's +10 dB ceiling down to silence takes 3.16 times as long because
        it is 3.16 times as far.

        Five milliseconds is this project's own figure and was already its
        figure for solo and for input monitoring: short enough to feel
        immediate, long enough not to click. Nobody publishes one. What the
        references do is worth knowing for the range it spans: Ardour's one
        pole is a 6.38 ms time constant, JUCE's own panner and its plugin
        example use 50 ms, and the only cross studio figures that exist are one
        third party's rendered measurements of the fastest automation move each
        studio can make, from under 0.5 ms in Pro Tools to about 320 ms in
        Studio One (Admiral Bumblebee, "DAW v DAW: Automation part 2"), which
        is corroboration and not specification. Logic measures at about 5 ms
        there, which is where this sits.
    */
    inline constexpr double levelSeconds = 0.005;

    /** The same, for a pan or balance control, and shorter on purpose.

        A pan position means a pair of gains, and smoothing the pair is not the
        same as smoothing the position. The straight line from (1, 0) to (0, 1)
        passes through (0.5, 0.5), whose power is exactly half, so a hard left
        to hard right move taken as two gain ramps dips 3.01 dB in the middle
        however good the law at either end is. Evaluating the law per sample
        instead would hold the power exactly, and is what Ardour does on its
        automated pan path (panner_1in2out.cc:257-300), but not on a manual
        one: there it smooths the two gains and bounds the damage by keeping
        the span to at most 64 samples, which is 1.45 ms at 44.1 kHz and 1.33
        at 48 (panner_1in2out.cc:161-180). JUCE smooths the gains too and takes
        50 ms over it (juce_Panner.cpp:78-79), which carries that dip for
        fifty times as long, and is the worse of the two.

        This follows Ardour, with the span written as a time rather than as a
        sample count so that it does not shorten as the sample rate rises the
        way 64 samples does. The cost is measured rather than asserted: the
        test sweeps the full width and holds the energy deficit through the
        move to a bound, which is what makes the figure defensible.
    */
    inline constexpr double panSeconds = 0.0015;

    /** How far a ramp may travel in one sample, from the time it should take
        to cross unity.

        Guarded at both ends because both have reached here before: a sample
        rate of zero before a device has opened, and a time of zero from a
        preference that has not been read yet. Either would be a division by
        nothing, and the second would make every ramp instant, which is the
        fault this header exists to fix wearing the shape of a fix.
    */
    inline double stepPerSample (double sampleRate, double seconds) noexcept
    {
        return 1.0 / (std::max (1.0, sampleRate) * std::max (1.0e-6, seconds));
    }

    /** Where a ramp was at the start of a block and where it is at the end.

        Two numbers rather than one because that is what the mixer applies: a
        gain ramped across the block from the first to the second, which is a
        straight line and so is exactly the slew the ramp is following. When
        the two are equal nothing is moving and the block takes one constant
        multiply, which is what keeps a steady mix reproducible.
    */
    struct Span
    {
        double from = 0.0, to = 0.0;

        bool steady() const noexcept { return from == to; }
    };

    /** The gain the n'th sample of a block gets under that span.

        This is juce::AudioBuffer::applyGainRamp's arithmetic written out, so
        that what the test measures is what the mixer applies. Note what it
        means at the ends, because JUCE's own documentation gets it wrong: the
        first sample is multiplied by `from` and the last by one increment
        short of `to`, not by `to`. That is correct rather than an off by one.
        It is what makes two consecutive blocks continuous instead of applying
        the same gain to the last sample of one and the first of the next, and
        a test that asserts the last sample carries `to` is asserting a bug.
    */
    inline double gainAt (Span s, int numSamples, int n) noexcept
    {
        if (numSamples <= 0)
            return s.from;

        return s.from + (s.to - s.from) * ((double) n / (double) numSamples);
    }

    /** One moving gain.

        One of these per gain that is applied, not per control that is read.
        Ardour holds _current_gain in the Amp processor and nothing at all in
        the GainControl, for a reason worth copying: the same control is read
        by more than one consumer here too, the fader by the mixing loop and
        the send levels by the send loop, and a smoother shared between
        consumers would make each one's ramp depend on how often the others
        ran.

        Held in double rather than in float, which costs nothing next to a
        block of audio and buys two things. A long ramp accumulates its step
        once per block, so the rounding of that addition is what decides
        whether the same gesture comes out the same at every buffer size, which
        is the property the export depends on. And arrival is tested as exact
        equality, which a float accumulating toward a float target reaches only
        because of the clamp, where a double reaches it with room to spare.
    */
    struct Ramp
    {
        double value = 0.0;

        /** Moves toward `target` by at most `step` a sample and reports the
            block. Never overshoots and never reverses: the clamp is the whole
            of the arrival rule, and monotone arrival is a property the test
            asserts rather than an accident. Ardour's own pan ramp is worth
            knowing about here as the thing not to do: it advances its
            interpolator by a delta taken from the error in a *lagged* copy of
            itself (panner_1in2out.cc:166-176), so it overshoots by about 12
            per cent of the step and crosses zero on one side, which is a
            momentarily inverted gain, before settling over five or six blocks.
        */
        Span advance (double target, double step, int numSamples) noexcept
        {
            const double from = value;

            if (numSamples > 0)
            {
                const double travel = step * (double) numSamples;

                value = from < target ? std::min (target, from + travel)
                                      : std::max (target, from - travel);
            }

            return { from, value };
        }

        /** Puts the ramp where it is going, for a moment that is not a gain
            change at all: the first block of an offline render, a device
            opening or changing its rate, or a path that has just been pointed
            somewhere new and has nothing to fade from. Without it a bounce
            taken straight after a fader move starts with a fade on the front
            of the file, which is the kind of fault that sounds like nothing.
        */
        void latch (double target) noexcept { value = target; }

        bool settled (double target) const noexcept { return value == target; }
    };

    /** The level of the k'th sideband that a per block constant gain puts
        either side of every component of the signal, in dB relative to it,
        from the derivation at the top of this header. `gainChangePerBlock` is
        how far the gain moves in one block and `k` counts from one.

        Here rather than in the test because it is the arithmetic that says why
        any of this exists, and because the test uses it the right way round:
        it measures the sidebands of a simulated stepped gain from a transform
        and checks the measurement against this, which is a cross check of a
        measurement against a derivation rather than a formula agreeing with
        itself. Only once the measurement is known to be sound is it turned on
        the ramped version.
    */
    inline double stepSidebandDb (double gainChangePerBlock, int k) noexcept
    {
        const double pi = 3.14159265358979323846;
        const double a  = std::abs (gainChangePerBlock) / (2.0 * pi * (double) std::max (1, k));

        return a > 0.0 ? 20.0 * std::log10 (a) : -1000.0;
    }
}
