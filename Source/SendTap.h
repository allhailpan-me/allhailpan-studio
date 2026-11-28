#pragma once

/**
    WHERE A SEND IS TAPPED OFF A STRIP

    An aux send is a copy of an insert, and the question this header answers is
    *which* copy: the one before the fader, the pan and the cut, or the one
    after them. Every studio looked at offers the choice and four of the six
    offer nothing else, because the two taps are for two different jobs. A post
    fader send is a reverb that comes down with the part it belongs to. A pre
    fader send is a level that stays where it was put, whatever the fader
    afterwards does.

    WHAT THE STUDIOS ACTUALLY OFFER, AND WHERE THEY DISAGREE

    Reaper has three, and names them for the stages they are after and before.
    User Guide section 5.12: "There are three types of send: Post-Fader (Post
    Pan), Pre-Fader (Post FX) and Pre FX", and "the volume of a send that is
    Post-Fader (Post Pan) will be affected by changes made to the source
    track's volume fader. A send that is Pre-Fader (Post FX) will not."

    Logic also has three and spends its third on the pan rather than on the
    effects, so its triad is Post Pan, Post Fader, Pre Fader, and it has no pre
    FX send at all: its own manual calls its pre fader tap "pre fader, post EQ",
    and says of the pre fader one that "the level of the signal routed to the
    send remains constant, regardless of any Volume fader movements on the
    source channel strip". Cubase, Pro Tools and Studio One are binary, pre or
    post fader, with no third option anywhere. Ardour has no enumeration at all:
    a send is a processor in a list, and `Route::set_processor_positions`
    (`route.cc:5469`) calls everything before the Amp pre fader, so an Ardour
    pre fader send is pre FX or post FX depending only on where the user
    dropped it.

    So the third tap is not one thing that some studios have and others do not.
    It is two different features of the same count, and the only tap all six
    agree on is the pre fader, post FX one. That is the one here.

    THE PAN IS NOT PART OF THIS QUESTION, AND THE NAMES SAY SO

    Not one of the six reads a post pan buffer for a pre fader send, and none
    mono sums one. In Ardour it is structurally impossible: the strip's panner
    lives inside the Main Outs delivery, which `route.cc:5762` pushes onto the
    end of the processor list after every send, so every Ardour send, pre or
    post fader, taps upstream of the pan. The rest answer the pan question with
    a switch of its own rather than with the tap point: Pro Tools' "FMP (Follow
    Main Pan)", Cubase's Link Panners, Ardour's `link-send-and-route-panner`.

    Which settles what this engine already is rather than only what it gains.
    `AudioEngine::processInsert` applies the fader, the pan and the cut in one
    multiply and `routeSends` copies out of the result, so the send this studio
    has always had is a **post fader, post pan** send in Reaper's exact sense:
    a named, documented, default behaviour rather than an accident. The new tap
    skips that whole multiply, which is Reaper's "Pre-Fader (Post FX)" exactly.

    The asymmetry is therefore deliberate and is not something to apologise for
    in a later tidy up: a post fader send here carries the strip's pan because
    it is copied out of the panned buffer, and a pre fader send does not,
    because it is copied out of the buffer before it. That is precisely the
    distinction Reaper's two names assert. A per send panner is a separate
    feature, and if one is ever added the established default is to follow the
    strip (Ardour's `link-send-and-route-panner` ships true), which means
    applying the same pan gains to the send's own copy rather than reading the
    panned buffer. Those are not the same code path and they differ by a pan
    law, so keep them straight.

    A TAP POINT MUST NEVER REACH THE COMPENSATION GRAPH

    This is the statement most worth not relearning, because getting it wrong
    does not crackle.

    Ardour computes a send's compensation from the send's real position, walking
    the processor list in reverse and accumulating what is still downstream of
    the tap on the dry path (`Route::update_signal_latency`, `route.cc:4820`,
    reconciled in `Send::update_delaylines`, `send.cc:163`). Between a post FX
    pre fader tap and a post fader tap sit the Amp and nothing else, and a gain
    multiply and a pair of pan gains have no latency, so that accumulation is
    identical at both taps.

    The same holds here for the same reason: all eight effect slots run before
    the fader, so the only thing between the two taps is one multiply. A pre
    fader send therefore leaves at exactly the latency a post fader send leaves
    at, and **`LatencyGraph` takes no tap point argument and must not grow
    one**. Switching a send between these two taps may not move a single number
    the compensation produces.

    It is also why there is no pre FX tap here. That one *would* move the
    figure: taken above the slots, a send leaves with the insert's own plugin
    latency still ahead of it on the dry path, so it leaves at `inLat[i]`, and
    `inLat` rather than zero because everything feeding the insert has already
    been lined up to `inLat` before the slots run. Using zero there would make
    a pre FX send early by the whole upstream alignment. Whoever adds that tap
    owes it a figure in `LatencyGraph` and a test for that figure, and until
    then the two taps below are the ones that are free.

    WHAT THE LEVEL IS WORTH, AND WHAT A CUT DOES TO IT

    `levelTarget` is the whole of the arithmetic, and it is one line per tap
    because the engine does the rest by choosing which buffer to copy. A post
    fader send's cut is already in the buffer it is copied from, so its target
    is the send level alone and applying a cut again here would square it. A
    pre fader send is copied from a buffer nothing has been applied to, so
    whatever cut it is supposed to carry has to be folded in here.

    `SoloGraph` is what decides that figure, through Ardour's mute point, and
    the reasoning for which way it defaults is at
    `SoloGraph::Settings::muteAffectsPreFaderSends`.
*/
namespace SendTap
{
    //==========================================================================
    /** Where a send's copy is taken from.

        Named after Reaper's own two names for these taps, including the
        parentheses, because the parenthesis is the half that stops a later
        reader assuming the other thing. "Post fader" does not imply "post pan"
        anywhere except where it says so: Logic ships them as two separate
        positions and Ardour cannot do post pan at all.
    */
    enum class Tap
    {
        postFader,   /**< after the fader, the pan and the cut: Reaper's "Post-Fader (Post Pan)" */
        preFader     /**< after the effect slots, before all three: Reaper's "Pre-Fader (Post FX)" */
    };

    /** Post fader, which is every studio's default and is also what every
        project saved before this existed was doing. Those two facts agreeing
        is what lets an absent attribute and a chosen default be the same
        value, so no existing mix moves. */
    constexpr Tap defaultTap = Tap::postFader;

    constexpr int numTaps = 2;

    //==========================================================================
    /** The name on the control, from Reaper's names, and the short form the
        mixer row has space for.

        Both are here rather than written out at the control for the same
        reason the predicates below are: a label that says one thing over a tap
        that does another is worse than either mistake on its own, so the words
        a user reads and the behaviour they describe come out of one file and
        are checked against each other. `shortName` is required to be the
        opening of `displayName`, which is what stops the two drifting into
        describing different taps.
    */
    inline const char* displayName (Tap tap) noexcept
    {
        return tap == Tap::preFader ? "pre fader (post FX)"
                                    : "post fader (post pan)";
    }

    inline const char* shortName (Tap tap) noexcept
    {
        return tap == Tap::preFader ? "pre" : "post";
    }

    //==========================================================================
    /** The value an `.ahp` carries, and the only place these numbers are
        written down.

        A small integer rather than a string id, which is the convention
        `PanLaw` already uses for the mixer settings saved in the same element,
        and it is safe here for the same reason: the mapping is an explicit
        switch rather than an index into a list, so a tap added in the middle
        of the enum cannot silently renumber the ones a project has already
        saved. That is the failure `Synth/ModMatrix.h` uses string ids to avoid,
        and the protection an explicit switch gives is the same protection.

        **A pre FX tap takes 2. These two numbers do not move.** Renumbering
        them would leave every project anybody had saved sending from the wrong
        point, and it would still load, still play and still sound plausible.
    */
    inline int tapToStored (Tap tap) noexcept
    {
        switch (tap)
        {
            case Tap::postFader:  return 0;
            case Tap::preFader:   return 1;
        }

        return 0;   // unreachable, and the stored value of the default if it were not
    }

    /** A value this header does not recognise reads as the default.

        Which is the quiet reading as well as the compatible one: post fader is
        what the send did before the choice existed, so a file written by a
        later version with a tap this one has never heard of plays the way this
        version has always played rather than from a point it cannot model.
    */
    inline Tap tapFromStored (int stored) noexcept
    {
        return stored == 1 ? Tap::preFader : defaultTap;
    }

    //==========================================================================
    /** Whether the strip's fader and pan are in the copy this tap takes.

        Both answers come from the one fact that the engine applies all three in
        a single multiply, so there is no tap here that carries the fader
        without the pan or the other way round. They are two functions rather
        than one because they are two claims about the behaviour, they are what
        the test names, and the day a per send panner arrives it is
        `carriesPan` that stops being the same question as `carriesFader`.
    */
    inline bool carriesFader (Tap tap) noexcept  { return tap == Tap::postFader; }
    inline bool carriesPan   (Tap tap) noexcept  { return tap == Tap::postFader; }

    /** Which of the two buffers a send at this tap is copied out of: the one
        taken before the multiply, or the insert's own output after it.

        A one line function because of what inverting it would do rather than
        because it earns its own name. It is the quietest possible breakage of
        this whole feature: both buffers hold the right strip, in time, at a
        plausible level, so a swapped answer is a reverb that follows a fader it
        was told not to follow and nothing else, with no error, no click and
        nothing on a meter. Left as a ternary in the mixing loop it would have
        had no oracle at all, since none of that loop can be compiled without
        JUCE. Here it is one of the things the test pins.

        It is the same question as `carriesFader`, read from the other end, and
        it is written as its own function rather than as `! carriesFader` so
        that the day a tap arrives whose buffer is neither of these two, this is
        where that is dealt with. */
    inline bool sourceIsPreFader (Tap tap) noexcept  { return tap == Tap::preFader; }

    //==========================================================================
    /** Whether the pre fader copy has to be taken this block, asked from the
        two places that ask it, which are not asking the same question.

        The copy is one block of two channels taken inside `processInsert`
        before the fader multiply, and it is taken only when some send on the
        insert is going to read it, so an insert with no pre fader send pays
        nothing. Which means the guard and the read have to agree exactly. A
        guard that is narrower than the reads hands `routeSends` a buffer
        `processInsert` did not fill, and the engine's own comment says what
        that is: this insert's audio from an earlier block, which is a few
        milliseconds of the right strip at the wrong time and is not a sound
        anybody could trace back to a mixer control.

        **The two predicates differ by one clause and the difference is
        deliberate**, which is the whole reason they are written down here
        rather than left as two inline expressions that look like a
        duplication somebody should tidy into one. `processInsert` runs first
        and `routeSends` runs second, in the same block, and `routeSends` is
        itself where a tap change is noticed: it moves `activeTap` on and arms
        the fade. So the two run either side of that move and see different
        state.

        `processInsert` has to anticipate it. On the block a tap changes it
        still sees the old `activeTap`, and the fade that will need the old
        tap's copy has not been armed yet, so it takes the copy on the
        strength of `activeTap` alone. `routeSends`, asking afterwards, has
        the armed fade to read instead, and its `fadeFrom` is that same
        `activeTap`. One clause, before and after the same fact is written
        down.

        Get that wrong in either direction and nothing announces it. Drop the
        anticipation and the first block of every fade off the pre fader tap
        reads a stale buffer or is latched to silence. Keep a clause that is
        no longer needed and the cost is one memcpy nobody can hear.

        The property worth asserting over both, and the one the test asserts,
        is not either formula: it is that neither can decline a copy that
        something is about to read. They are therefore written in terms of
        `sourceIsPreFader`, which is the function that actually decides which
        buffer a send reads, rather than against `Tap::preFader` again.
    */
    inline bool preFaderCopyWantedWhileRouting (Tap tap, double fade, Tap fadeFrom) noexcept
    {
        return sourceIsPreFader (tap)
            || (fade > 0.0 && sourceIsPreFader (fadeFrom));
    }

    /** As above, from before the tap change has been written down. */
    inline bool preFaderCopyWantedBeforeRouting (Tap tap, Tap activeTap,
                                                 double fade, Tap fadeFrom) noexcept
    {
        return sourceIsPreFader (tap)
            || sourceIsPreFader (activeTap)
            || (fade > 0.0 && sourceIsPreFader (fadeFrom));
    }

    /** Whether the strip's own cut, its mute and whatever somebody else's solo
        does to it, reaches a send taken at this tap.

        A post fader send always carries it, because it is copied out of the
        buffer the cut was applied to and there is no way for it not to. Ardour
        agrees and ships `mute-affects-post-fader` true, and `SoloGraph.h`
        spells out what dropping it would cost: a guitar silenced by a vocal
        solo would still feed the reverb bus that the solo is keeping audible,
        and would be heard through it.

        At the pre fader tap it is a setting, because that is the one point
        where the studios genuinely disagree. See
        `SoloGraph::Settings::muteAffectsPreFaderSends`.
    */
    inline bool carriesStripCut (Tap tap, bool muteAffectsPreFaderSends) noexcept
    {
        return tap == Tap::postFader || muteAffectsPreFaderSends;
    }

    //==========================================================================
    /** One sample of a send while it is travelling from one tap to the other.

        Each side arrives here **resolved**: the strip's copy at that tap times
        the gain the send is actually running at there. The answer is the
        straight line between those two, so it is a convex combination of two
        steady states and therefore cannot exceed either of them.

        That phrasing is the whole of this function and it is here because the
        obvious arrangement is wrong in a way nothing announces. The first
        version mixed the two **buffers** and then applied one gain to the
        blend, which looks equivalent and is not: the two taps differ in their
        gain as well as in their buffer, because `levelTarget` folds the cut
        into one and not the other. So a tap change was two moves at once and
        the output was their product. On a muted strip at the shipped default
        the pre fader gain is zero and the post fader buffer is zero, so both
        steady states are silence, and the product of a gain falling from one
        and a fade rising from nothing put a 5 ms burst at -12 dB of the muted
        strip's own level onto the bus. Measured, in the configuration a mute
        gives by default, where the right answer is exactly nothing. It then
        travelled through the send's alignment line and arrived up to 85 ms
        later, which is the same "nothing connects it to the control that caused
        it" the crossfade exists to avoid.

        A straight line rather than an equal power curve, deliberately: the two
        sides are the same audio at two gains rather than two signals, so they
        sum coherently and a curve would put a bump in the middle of a level
        that should simply travel.

        `t` runs from 0 to 1 and is not clamped here. The caller drives it,
        and the engine cannot currently take it outside that range: the fade is
        armed at one and floored at nothing. The test sweeps outside it anyway,
        to pin what it would do, which is to extrapolate rather than to
        saturate. That is a property nothing relies on today rather than a
        guarantee anybody is leaning on, and it is said that way round because
        this sentence previously claimed a sweep that did not exist.
    */
    inline double fadeSample (double fromSample, double fromGain,
                              double toSample,   double toGain,
                              double t) noexcept
    {
        const double from = fromSample * fromGain;
        const double to   = toSample   * toGain;

        // Written as the weighted sum rather than as "from, plus the distance
        // travelled", and the difference is not style. The travelled form is
        // exact at t of nothing and is **not** exact at t of one: `from + (to -
        // from)` loses the low bits of `from`, so at 0.7 by 0.5 against -0.2 by
        // 0.25 it misses the far endpoint by one part in 1.0e16. This form
        // multiplies each end by exactly 1 and exactly 0 at the two extremes,
        // so both are returned bit for bit. That matters because the blocks
        // either side of a fade take the steady path, so an endpoint out by a
        // rounding error would be a join that is a click of its own, twice,
        // at a level nothing would ever trace back to here.
        return from * (1.0 - t) + to * t;
    }

    /** The same sample, driven by the fade that is **left** rather than by the
        progress that has been made.

        Which is what the engine holds: `sendTapFade` is armed at one and
        counted down to nothing, so every caller of `fadeSample` had to write
        `1.0 - fade` first, and a caller that wrote `fade` instead would get a
        crossfade that runs backwards. That one is worth removing by
        construction rather than testing for, because of what it does and does
        not do. It still produces a convex combination of the same two steady
        states, so it cannot overshoot, cannot click at a join and stays
        monotone: every property `fadeSample` promises survives it. The only
        symptom is that the first sample after the button is the tap the send
        has just arrived at and the last one is the tap it left, so the
        discontinuity is moved from the start of the five milliseconds to the
        end of them instead of being removed.

        So the subtraction lives here, once, next to the reason for it, and the
        direction is pinned by a test that names the two ends: a whole fade
        remaining is the tap being left, and none remaining is the tap being
        arrived at.
    */
    inline double fadeSampleRemaining (double fromSample, double fromGain,
                                       double toSample,   double toGain,
                                       double remaining) noexcept
    {
        return fadeSample (fromSample, fromGain, toSample, toGain, 1.0 - remaining);
    }

    //==========================================================================
    /** The gain a send's own ramp should be heading for.

        `preFaderCutGain` is the figure `SoloGraph` worked out for the pre fader
        point, `State::preFaderSendGain`, and it is **read only on the pre fader
        branch**. That is not an optimisation: a post fader send is copied out
        of the buffer the cut was already applied to, so folding a cut in here
        as well would apply it twice and arrive at its square. At full mute that
        is still silence and at a partial solo cut it is a wet path collapsing
        instead of coming down with the mix, which is the failure written up in
        `SoloGraph.h` under WHAT A PARTIAL CUT DOES TO A WET PATH. The post
        fader branch therefore cannot be affected by this argument at all, which
        is a property the test asserts rather than a convention to be trusted.

        A number this function cannot make sense of reads as silence rather than
        being clamped to the nearest end. The discipline and the reason are
        `SoloGraph::safeSoloMuteGain`'s: the level control is 0 to 1, a file the
        application does not control can hold anything, every comparison against
        a NaN is false so a clamp hands the NaN straight back, and of the
        readings available for a number that means nothing, silence is the only
        one that cannot damage a mix. Above unity a send would arrive louder
        than the insert it was taken from and could clip a destination that was
        fine. Unity is not an acceptable fallback for an unreadable cut either:
        that would be a mute that did nothing.
    */
    inline double levelTarget (Tap tap, double sendLevel, double preFaderCutGain) noexcept
    {
        if (! (sendLevel >= 0.0 && sendLevel <= 1.0))
            return 0.0;

        if (tap != Tap::preFader)
            return sendLevel;

        if (! (preFaderCutGain >= 0.0 && preFaderCutGain <= 1.0))
            return 0.0;

        return sendLevel * preFaderCutGain;
    }
}
