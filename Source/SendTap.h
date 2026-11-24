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
