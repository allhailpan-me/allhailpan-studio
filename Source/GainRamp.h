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

    A SLEW LIMITER, not a one pole, and this is the one choice here that is a
    trade rather than a conclusion. Ardour's is a one pole at a 25 Hz corner,
    a = 156.825f / sample_rate, a time constant of 6.3765 ms at every sample
    rate. A one pole never arrives, so Ardour carries an epsilon snap at a
    linear gain of 1e-5 in four separate places to make it terminate
    (amp.cc:136, 219, 285 and delivery.cc:354). A slew limiter arrives because
    it is clamped, which is worth a great deal here: a control left alone then
    sits exactly on its own gain, so a steady mix is one exact multiply and is
    reproducible, and a mute is actually silent rather than 100 dB down. It is
    also the law the solo and input monitoring ramps in this engine already
    follow, so there is one law rather than two shapes meeting in the same
    multiply.

    What it costs is measured rather than waved away, and the measurement is
    kept: Tests/GainRampProbe.cpp runs the same fader drag through both laws.
    A slew limiter's smoothing time is proportional to the size of the move,
    so the small jumps an interface makes while somebody drags a fader, about
    a twentieth of the travel at sixty a second, are smoothed over thirteen
    samples. That removes the discontinuity completely, which is the audible
    part, and takes 11.71 dB off the error that lands above 2 kHz. A one pole
    smooths every move over the same time whatever its size and takes 42.59 dB
    off instead, which is a great deal more.

    It is not here for two reasons, and the honest statement of the trade is
    that the first is a real cost and the second is only machinery. At Ardour's
    own time constant a one pole reaches -40 dB of a mute in 29.3 ms and exact
    silence in 73.3 ms, where the slew limiter is exactly silent in 5.02 ms,
    and five milliseconds is the figure this studio deliberately chose for a
    mute and a solo. And it arrives only because of an epsilon snap, which
    Ardour needs in four separate places.

    So this is the cheaper law, knowingly, and the better one is costed rather
    than dismissed: the probe is in the tree and the figures are in
    Research/BACKLOG.md, so a later night can take it with the measurement in
    hand. An earlier version of this comment estimated the one pole at "about
    30 dB" and an audit measured 42.59. Worth leaving on the record, because a
    number that is present, plausible and wrong is what this project keeps
    meeting, and here it was in the argument rather than in the signal, in the
    direction that flattered the decision already made.

    One thing it does NOT fix, which is worth knowing before trusting it: a
    control that only changes once a block still arrives as a staircase, and a
    slew that crosses the whole fader in five milliseconds crosses a single
    block's worth of an automation curve in a few samples and then waits. So an
    automated fade comes out continuous but still shelved, and the sidebands
    the derivation above describes are unchanged. Removing those needs the
    target to move within the block, which means reading the automation curve
    per sample the way Ardour does (amp.cc:106-116). That is the next step and
    is recorded, not done here.

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

        Guarded at both ends because both have reached code like this before: a
        sample rate of zero before a device has opened, and a span of zero from
        a preference that has not been read yet. Unguarded, either is a
        division by nothing, and what comes out is an infinity that becomes a
        NaN the first time it meets a gain of zero, which does not go quiet: it
        silences everything downstream of that insert and gives no clue where
        it came from. The guards do not pretend to rescue a nonsense rate, and
        the test says so rather than implying otherwise: at no sample rate
        there is no ramp to be had and the gain arrives at once.
    */
    inline double stepPerSample (double sampleRate, double seconds) noexcept
    {
        return 1.0 / (std::max (1.0, sampleRate) * std::max (1.0e-6, seconds));
    }

    /** One moving gain.

        One of these per gain that is applied, not per control that is read.
        Ardour holds _current_gain in the Amp processor and nothing at all in
        the GainControl, for a reason worth copying: the same control is read
        by more than one consumer here too, the fader by the mixing loop and
        the send levels by the send loop, and a smoother shared between
        consumers would make each one's ramp depend on how often the others
        ran.

        Advanced one sample at a time, which is what makes the gesture
        independent of the buffer size: the recursion below does not know what
        a block is, so the same move gives the same samples whether the driver
        hands over 32 of them or 2048, and a bounce is what was heard.

        Written as a block of two numbers and one ramp across it instead, which
        is what the first version of this did, it is not. Two things break.
        A ramp that arrives part way through a block has its remaining travel
        spread across the whole of it, so a move that should take five
        milliseconds takes as long as the buffer: 42.7 ms at 2048 samples. And
        a mixer multiplies several of these together, so the gain a sample gets
        is a product of ramps, and the straight line between the ends of a
        block is not that product: solo and the fader both crossing their whole
        travel inside one block put the chord a quarter of full scale away from
        the curve in the middle of it. Both of those are the buffer size
        reaching the sound through the back door, which is the one thing this
        header exists to prevent. The measured cost of doing it per sample
        instead is a handful of operations a sample on the inserts that are
        actually moving.

        Held in double rather than in float, which costs nothing next to a
        block of audio: arrival is tested as exact equality, and the clamp
        reaches it with room to spare rather than only just.
    */
    struct Ramp
    {
        double value = 0.0;

        /** Moves one sample toward `target` and gives back the gain that
            sample gets.

            Advanced before it is read, which is a choice and is written down
            because the two references differ and mixing them inside one engine
            would be a half sample disagreement between a gain and a pan that
            nobody would ever find: Ardour's Amp multiplies and then updates
            (amp.cc:210-213) and its panner updates and then multiplies
            (panner_1in2out.cc:172-174). This is the second, because the
            monitoring path here was already written that way.

            Never overshoots and never reverses: the clamp is the whole of the
            arrival rule, and monotone arrival is a property the test asserts
            rather than an accident. Ardour's own pan ramp is worth knowing
            about as the thing not to do: it advances its interpolator by a
            delta taken from the error in a lagged copy of itself, so it
            overshoots by about 12 per cent of the step and crosses zero on one
            side, which is a momentarily inverted gain, before settling over
            five or six blocks.
        */
        double next (double target, double step) noexcept
        {
            value = value < target ? std::min (target, value + step)
                                   : std::max (target, value - step);

            return value;
        }

        /** Puts the ramp where it is going, for a moment that is not a gain
            change at all: the first block of an offline render, a device
            opening or changing its rate, or a path that has just been pointed
            somewhere new and has nothing to fade from. Without it a bounce
            taken straight after a fader move starts with a fade on the front
            of the file, which is the kind of fault that sounds like nothing.
        */
        void latch (double target) noexcept { value = target; }

        /** Exact equality, deliberately. It is what lets a block that is not
            moving be one constant multiply, so a steady mix comes out the same
            every time, and it is reachable because the step above is clamped
            rather than asymptotic. Ardour's one pole cannot do this, which is
            why it carries an epsilon snap in four places. */
        bool settled (double target) const noexcept { return value == target; }
    };

    /** The two gains an insert's output is multiplied by, for one sample. */
    struct Pair
    {
        double left = 0.0, right = 0.0;
    };

    /** Where the four gains on a mixer strip are going.

        Gains rather than positions, because a position only means anything
        through a law and three of these four have no position at all.
    */
    struct Targets
    {
        double solo  = 1.0;
        double fader = 1.0;
        double left  = 1.0;
        double right = 1.0;
    };

    /** The four moving gains on one insert, and the pair they come to.

        This is here rather than in the mixer, and the whole of it is here
        rather than half, because of what the two halves get wrong when they
        are apart. Both faults were found by mutation testing the engine and
        neither was reachable by any test while the arithmetic lived in
        `AudioEngine.cpp`, which cannot be compiled without JUCE:

        Advancing the ramps once per channel instead of once per sample pair
        halves every span and gives the left channel the first half of every
        move while the right gets the second, which is a pan on every gain
        change. CLAUDE.md warns about exactly this shape and Ardour reseeds its
        own interpolator at the top of each channel to avoid it
        (amp.cc:111). Asking for both gains in one call makes it unspellable.

        Advancing the two pan gains at different rates lets a pan move swing
        through an image the law never specifies, and makes the 3.01 dB dip
        last the longer of the two spans rather than the shorter, which is the
        whole justification for the pan span being short. One step argument for
        both pan ramps makes that unspellable too.

        The multiply order is written down because it is load bearing: solo and
        the fader make one level, and the two pan gains are applied to that
        same level, so the two channels can only differ by their pan. Computing
        each channel's gain as its own product of three would let them differ
        by a rounding step in the level as well.
    */
    struct Strip
    {
        // Solo starts at unity because nothing is soloed until something is,
        // and because zero here is silence: a strip default constructed at
        // zero would open the studio fading up.
        Ramp solo { 1.0 };
        Ramp fader, left, right;

        Pair next (const Targets& t, double levelStep, double panStep) noexcept
        {
            const double level = solo.next (t.solo, levelStep) * fader.next (t.fader, levelStep);

            return { level * left .next (t.left,  panStep),
                     level * right.next (t.right, panStep) };
        }

        bool settled (const Targets& t) const noexcept
        {
            return solo.settled (t.solo) && fader.settled (t.fader)
                && left.settled (t.left) && right.settled (t.right);
        }

        /** The pair a settled strip applies, which the mixer uses as one
            constant multiply per channel for the whole block. Same multiply
            order as `next`, so a block that stops moving mid gesture does not
            change the gain by a rounding step as it settles. */
        Pair gains() const noexcept
        {
            const double level = solo.value * fader.value;

            return { level * left.value, level * right.value };
        }

        void latch (const Targets& t) noexcept
        {
            solo .latch (t.solo);
            fader.latch (t.fader);
            left .latch (t.left);
            right.latch (t.right);
        }
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
