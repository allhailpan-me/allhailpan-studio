#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>

//==============================================================================
/** Solo, as arithmetic: which inserts stay audible when something is soloed.

    Muting everything that is not soloed is the obvious implementation and it
    is wrong in a way the person mixing cannot diagnose. Soloing a vocal that
    sends to a reverb would give a dry vocal, because the reverb is on another
    insert and that insert is not the one soloed. Soloing the reverb would give
    a reverb with nothing going into it. Neither errors, neither crackles, and
    what somebody hears is "solo does not work on this" with no way to say why.

    So the decision of what stays audible lives here, over booleans and a
    reachability matrix, with no JUCE in it, and the engine's job is reduced to
    reading the routing out of its atomics, calling this, and multiplying one
    number into the fader gain it already applies.

    WHERE THE BEHAVIOUR COMES FROM

    Ardour, read from its own source tree at `d66bd6ce` rather than from
    anybody's description of it, because it is the only one of the four studios
    researched that publishes an algorithm for this at all. Pro Tools and Logic
    both document a per track exemption instead (`Solo Safe` and solo-safe,
    which are this header's `isolated`), and Avid's own Solo Safe page says it
    is "especially useful" on "Aux tracks, busses, and effects returns, so
    shared effects like reverb or delay aren't cut off when you solo a related
    track". That sentence is the evidence that Pro Tools does not propagate: a
    studio that did would not have to tell people to solo-safe their returns.
    Reaper's vocabulary is inverted relative to everybody else's and was
    deliberately not borrowed. Research/BACKLOG.md has the full sourcing and
    names the four questions no reachable primary source answered.

    THE RULE

    Ardour keeps two counters per route, `_soloed_by_others_upstream` and
    `_soloed_by_others_downstream`, and moves them incrementally in
    `Session::route_solo_changed` as solos come and go. Read the names twice
    before working from them: on route R, `_soloed_by_others_upstream` means
    something *upstream of R* is soloed. The counter is named for where the
    soloed route sits, not where R sits.

    The same answer as a closure over a set S of soloed inserts, which is what
    this computes. An insert is kept audible by the solo system when:

      1. it is soloed itself, or
      2. something soloed feeds it, directly or through other inserts, so that
         the soloed thing can be heard at all, or
      3. it feeds something soloed, so that a soloed bus plays what goes into
         it, EXCEPT where that soloed thing is itself fed by something soloed,
         or
      4. it is solo isolated.

    Rule 3's exception is the whole difficulty and it is where a naive
    implementation fails. Vocal on 3 and guitar on 4 both send to a reverb on
    9. Solo 9 and all of 3, 4 and 9 are audible, which is Ardour's documented
    behaviour: "Soloing a bus will also solo any tracks or busses which feed
    that bus". Now solo 3 as well, and the guitar has to go silent while the
    vocal and the reverb stay, because somebody soloing one of the things
    feeding a soloed bus is asking for that one. Ardour arrives there by a
    correction it calls the inverse push, with its own comment: "When we solo 1
    track out of N that feed a bus, that track will cause mod_solo_by_upstream
    (+1) to be called on the bus. The bus then needs to call
    mod_solo_by_downstream (-1) on all tracks that feed it." Computed as a
    closure the correction is not needed: the term rule 3 would have granted
    the guitar comes from insert 9, and insert 9 is fed by the soloed insert 3,
    so the exception removes it and nothing has to be pushed anywhere.

    WHY A CLOSURE RATHER THAN COUNTERS

    Ardour holds counters because its graph is arbitrary and its routes come
    and go; this mixer is a fixed number of inserts with an acyclic send
    matrix, so recomputing the whole answer costs a few hundred operations and
    makes the hardest fault in the feature impossible. That fault is a leaked
    counter, and it is not hypothetical: Ardour ships
    `SoloControl::clear_all_solo_state` as a repair path for it, with user
    visible logging of each counter it had to clear, and it has two further
    pieces of code (`InternalSend::propagate_solo`, and zeroing a route's solo
    before removing it) whose only job is keeping the counters honest when the
    graph changes underneath them. A leaked counter is an insert that is
    audible, or silent, for no reason the project can account for.

    That is not only an argument about tidiness, and the test went looking.
    Ardour's incremental algorithm is implemented in `SoloGraphTest.cpp` from
    its own source and driven alongside this closure over thousands of
    generated mixers, and what came back is sharper than expected:

      The two agree EXACTLY, on every insert of every graph, whenever the
      solos are switched on upstream first. They diverge only when a solo
      downstream of another is switched on first, and that is also exactly
      when Ardour's algorithm disagrees with ITSELF: the same set of soloed
      inserts, pressed in a different order, gives a different mix in about
      12 per cent of generated cases.

    The cause is that the inverse push subtracts from every insert upstream of
    the bus it fires on, rather than from the inserts holding a term for that
    bus, so with two solos whose upstream sets overlap it can take away a term
    an insert never had. The smallest case found is five inserts: 1 sends to 2,
    2 sends to 3 and 4, 3 sends to 4. Solo 2 then 4 and insert 1 is audible,
    which is right, because it feeds soloed insert 2 and nothing soloed feeds
    insert 2. Solo 4 then 2 and insert 1 goes silent, so soloing a bus loses
    its own input depending on which button was pressed first.

    So this closure is the order independent reading of the published
    algorithm, and where it differs from Ardour it differs by not losing a
    feed. The test asserts the agreement, asserts that the divergence is only
    ever in that direction, and demonstrates the order dependence rather than
    taking it on trust.

    One more asymmetry worth writing down, since it is the other half of the
    same story: propagation skips solo isolated routes
    (`session.cc`'s `continue` on `solo_isolated()`), while the inverse push
    does not, so an isolated route can be decremented for a term it was never
    given. Both of Ardour's mutators clamp at zero, which absorbs it, and a
    clamped underflow is a leaked counter the next time round. With isolation
    in play the counters here do not return to zero when every solo is
    cleared, which is presumably what the repair path is for.

    SIX GUARDS IN HERE ARE DELIBERATELY REDUNDANT

    Written down so nobody goes looking for the test that should have caught
    them. Of 49 deliberate breakages of this header, 43 were caught and these
    six produce identical output, each for a reason rather than by luck:

    - **`soloedAt`'s `i >= 1`, and the propagation scan starting at 1.** These
      two are a pair, and removing either one alone changes nothing because
      the other still keeps insert 0 out. Removing both would let a solo
      stored against the master silence the mixer. Both are kept, because one
      comparison is cheaper than a rule that says "this is safe as long as
      every loop bound elsewhere stays as it is".
    - **The `s != i` in the taken over scan, and the `s == i` in the
      propagation scan.** Both rest on no insert reaching itself, which the
      forward only send rule guarantees. They are the guards that would matter
      the day somebody relaxes that rule, which is the same day this header's
      single downward pass stops working, and `isLegalSendTarget` says so too.
    - **Rule 3 being reached by an `else if` rather than an `if`.** Both rules
      set the same flag and the loop stops on it, so the two orderings cannot
      differ. It is an `else if` because asking the second question after the
      first has already answered yes is work for no reason.
    - **The `i >= 1` on isolate.** The master is downstream of everything, so
      it is at unity whether or not it is isolated, and a muted master is at
      zero in both branches of `gainFor` regardless. It is kept for the same
      reason as the first pair: a project file can say anything, and the
      guard, not an argument about reachability, is what makes that harmless.

    WHAT THE GAIN IS APPLIED TO

    Ardour applies this per output point rather than per route, in
    `Delivery::run`, and with its shipped defaults
    (`mute-affects-main-outs` and `mute-affects-post-fader` both true) an
    insert silenced by somebody else's solo has **both its main output and its
    post fader sends** cut. The second half is the one that is easy to miss and
    it is load bearing: without it a guitar silenced by a vocal solo still
    feeds the reverb bus, the reverb bus is audible because the vocal soloed it
    upstream, and the guitar is heard through the reverb. One gain per insert
    is enough to get both here only because this mixer's sends are taken post
    fader out of the same buffer the fader was applied to. If a pre fader send
    is ever added, this is the comment that says it needs its own figure.
*/
namespace SoloGraph
{
    //==========================================================================
    /** One mixer insert's solo state and routing. Index 0 is the master. */
    struct Insert
    {
        bool selfSoloed = false;   /**< the user clicked solo on this one */
        bool muted      = false;   /**< the user clicked mute on this one */

        /** Exempt from being muted by somebody else's solo, and still soloable
            itself. Ardour's Solo Isolate, Pro Tools' Solo Safe, Logic's
            solo-safe: three names for one behaviour, and all three manuals
            describe it the same way. Ardour: "When tracks or busses are soloed
            the isolated ones will not mute."

            Not Ardour's internal `SoloSafeControl`, which is a different
            control that freezes an insert's solo state against being changed
            at all, and which Ardour's manual calls Solo Lock. Reading Ardour's
            source with Pro Tools habits implements the wrong feature. */
        bool isolated = false;

        /** Per send: the destination insert, or anything that is not a legal
            forward target, meaning no send. */
        std::vector<int> sendTo;
    };

    //==========================================================================
    /** The two settings that change what the arithmetic answers, rather than
        what the user asked for. Both are application preferences in Ardour. */
    struct Settings
    {
        /** What an insert silenced by somebody else's solo is attenuated to,
            as a linear gain. Ardour's `solo-mute-gain` is also a linear
            coefficient and also defaults to full mute: "Setting this level to
            -inf dB will mean that other tracks will not be heard at all".

            Anything above unity is refused rather than clamped into range,
            along with anything negative or not a number, because a value this
            header cannot make sense of has exactly one safe reading: the
            documented default. Above unity would mean soloing one thing made
            everything else *louder*, which nobody wants and which can clip a
            master that was fine. */
        float soloMuteGain = 0.0f;

        /** Whether a self soloed insert is heard even though it is also muted.

            Ardour's `solo-mute-override`, which ships false, so by default an
            explicit mute wins. Its own manual says the opposite in one place
            ("by default, Solo overrides Mute, i.e. if a track is both Soloed
            and Muted, it will play") and that sentence disagrees with both the
            config default and the arithmetic in `MuteMaster::mute_gain_at`, so
            it is treated as stale documentation. No primary source was
            reachable for Pro Tools, Logic or Reaper on this question, so there
            is no majority to follow and the default here is a choice: a mute
            somebody set by hand is an instruction, and a solo somewhere else
            is a temporary audition. */
        bool soloOverridesMute = false;
    };

    struct Setup
    {
        std::vector<Insert> inserts;   /**< at least one: the master */
        Settings settings;
    };

    //==========================================================================
    /** What one insert ends up at. */
    struct State
    {
        /** What to multiply that insert's output by, which carries its post
            fader sends with it. Unity, the solo mute gain, or exactly zero. */
        float gain = 1.0f;

        /** Audible because of somebody else's solo rather than its own: rules
            2 and 3 above. The interface needs this and cannot derive it:
            Ardour's solo button has three states, not two
            (`RouteUI::solo_active_state` returns explicit, implicit or off),
            and without the third one soloing a vocal lights nothing on the
            reverb bus and there is no way to tell why the reverb is still
            heard. */
        bool soloedByOthers = false;

        /** Silent, or attenuated, only because something else is soloed.
            Ardour draws the mute button outlined on these (`show-solo-mutes`,
            which ships on) and Logic flashes it. Same purpose as above: the
            strip has to be able to say why it is quiet. */
        bool mutedByOthersSoloing = false;
    };

    struct Result
    {
        std::vector<State> inserts;

        /** Whether anything at all is soloed, which is what lights the
            indicator that clears every solo. Ardour's is internally called
            "rude solo" and its tooltip is the whole argument for having one:
            "When active, something is soloed. Click to de-solo everything". */
        bool anySoloed = false;

        /** Row major, `numInserts * numInserts`, reused rather than rebuilt.
            Exposed because the test compares it against a closure written
            separately, which is the only way to check the production one
            without restating it. */
        std::vector<unsigned char> reach;

        /** Per insert: soloed, and itself fed by something soloed, so that the
            other things feeding it do not stay audible. Rule 3's exception.

            Its own array rather than a field of `State`, which is where it
            started and which was wrong: the pass that decides each insert's
            answer reads this flag for *other* inserts, so holding it in the
            field that pass overwrites means an insert's answer depends on
            whether the inserts it asks about have been visited yet. Nothing
            about that announces itself; it would simply make soloing a bus
            give a different mix depending on which insert numbers the sends
            happened to use. */
        std::vector<unsigned char> takenOver;
    };

    //==========================================================================
    /** True when this insert may legally be a send destination from `from`.

        Forward only, which is the rule that keeps the mixer acyclic, and never
        the master, which every insert already reaches by its direct path.

        This has to agree exactly with what `AudioEngine::routeSends` acts on
        and with `LatencyGraph::isLegalSendTarget`. A send the engine ignores
        but this counts would keep an insert audible for a route carrying no
        audio; a send the engine acts on but this ignores would silence an
        insert whose signal is reaching the soloed thing.
    */
    inline bool isLegalSendTarget (int from, int to, int numInserts) noexcept
    {
        return to > from && to < numInserts;
    }

    /** A send turned down to nothing is still a send, and still propagates.

        Deliberate, and the reason is worth keeping because the other choice
        looks tidier. Ardour's `Route::feeds` is about connections and takes no
        account of a send's gain, and `LatencyGraph` already compensates a send
        whose level is zero for the same reason: the level is a knob the user
        is in the middle of moving, and a routing that appeared and disappeared
        as it crossed zero would make the mixer's behaviour depend on a
        continuous control. "Off" for a send here is its destination, not its
        level.
    */
    inline constexpr bool levelAffectsRouting = false;

    //==========================================================================
    /** The solo mute gain, as a number this header is willing to act on.

        A setting is read from a file the application does not control, so it
        can be negative, above unity, or not a number at all, and every
        comparison against a NaN is false: a clamp hands the NaN straight back.
        Written as "is it in range" rather than "is it out of range" for that
        reason, which is the same discipline `Prefs::number` uses and for the
        same reason.

        Out of range falls back to the documented default rather than to the
        nearest bound, because the one reading that is certainly safe for a
        value this header cannot make sense of is full mute: the worst it can
        do is make solo more silent than asked, which is audible immediately
        and cannot damage a mix.
    */
    inline float safeSoloMuteGain (const Settings& settings) noexcept
    {
        const float g = settings.soloMuteGain;

        return g >= 0.0f && g <= 1.0f ? g : 0.0f;
    }

    //==========================================================================
    /** The gain one insert ends up at, given everything known about it.

        `MuteMaster::mute_gain_at`, both of its branches, with the redundant
        tests folded out. Ardour's version asks `!_soloed_by_others &&
        muted_by_others_soloing_at (mp)` where the second term already implies
        the first; `mutedByOthersSoloing` below is its definition,
        `_session.soloing() && !soloed() && !solo_isolated()`, so the pairs
        agree term for term.

        The asymmetry in the override branch is the part a paraphrase of "solo
        overrides mute" loses, and it is deliberate in Ardour: the test there
        is `_soloed_by_self`, not `soloed()`. Self solo beats an explicit mute;
        being soloed by propagation does not. So with the option on, a muted
        reverb return that is only audible because a soloed vocal feeds it
        stays muted, which is right: nobody asked to hear that return, they
        asked to hear the vocal.
    */
    inline float gainFor (bool muted, bool selfSoloed, bool soloedByOthers,
                          bool isolated, bool anySoloed, const Settings& settings) noexcept
    {
        const bool mutedByOthersSoloing = anySoloed && ! selfSoloed
                                                    && ! soloedByOthers
                                                    && ! isolated;

        if (settings.soloOverridesMute)
        {
            if (selfSoloed)
                return 1.0f;

            if (muted)
                return 0.0f;

            return mutedByOthersSoloing ? safeSoloMuteGain (settings) : 1.0f;
        }

        if (muted)
            return 0.0f;

        return mutedByOthersSoloing ? safeSoloMuteGain (settings) : 1.0f;
    }

    //==========================================================================
    /** Fills in `result`, reusing whatever storage it already has.

        That reuse is the point rather than a tidiness, and it is the same
        argument `LatencyGraph::compute` makes: this runs with the engine's
        graph lock held, and the audio thread only ever *tries* that lock and
        drops the block when it cannot get it. An allocation in here is a
        chance of an audible gap every time somebody clicks solo.
    */
    inline void compute (const Setup& setup, Result& result)
    {
        const int n = (int) setup.inserts.size();

        result.anySoloed = false;

        if (n <= 0)
        {
            result.inserts.clear();
            result.reach.clear();
            result.takenOver.clear();
            return;
        }

        result.inserts.assign ((std::size_t) n, State{});
        result.reach.assign ((std::size_t) n * (std::size_t) n, 0);
        result.takenOver.assign ((std::size_t) n, 0);

        auto reaches = [&result, n] (int from, int to) -> bool
        {
            return result.reach[(std::size_t) from * (std::size_t) n + (std::size_t) to] != 0;
        };

        // Insert 0 is the master: it is where every other insert is summed, so
        // soloing it would mean soloing the whole mix, and isolating it would
        // be exempting the one thing nothing can silence. Ardour draws the
        // same line with `Route::can_solo`, which is false for its master.
        auto soloedAt = [&setup] (int i)
        {
            return i >= 1 && setup.inserts[(std::size_t) i].selfSoloed;
        };

        for (int i = 1; i < n; ++i)
            if (soloedAt (i))
                result.anySoloed = true;

        // ---- reachability ----
        //
        // One pass downward, which works because of two facts about this
        // mixer and would not work without them: a send may only feed a higher
        // numbered insert, and the direct path from every insert goes to
        // insert 0, which has no output of its own to follow. So by the time
        // insert i is reached, everything it can feed is already resolved.
        //
        // The direct path to the master is added for every insert
        // unconditionally, because it is not a send and cannot be switched
        // off. That is what makes the master downstream of everything, which
        // is why it is audible whenever anything is soloed and needs no
        // isolate of its own.
        for (int i = n - 1; i >= 1; --i)
        {
            auto* row = &result.reach[(std::size_t) i * (std::size_t) n];

            auto follow = [&] (int to)
            {
                row[(std::size_t) to] = 1;

                const auto* theirs = &result.reach[(std::size_t) to * (std::size_t) n];

                for (int k = 0; k < n; ++k)
                    if (theirs[(std::size_t) k] != 0)
                        row[(std::size_t) k] = 1;
            };

            follow (0);

            for (int to : setup.inserts[(std::size_t) i].sendTo)
                if (isLegalSendTarget (i, to, n))
                    follow (to);
        }

        if (! result.anySoloed)
        {
            // Nothing is soloed, so nothing is kept audible by the solo system
            // and nothing is silenced by it either. Only the mutes are left,
            // and `gainFor` is still what decides them, so that an insert's
            // gain comes from one place whether or not a solo is up.
            for (int i = 0; i < n; ++i)
            {
                const auto& in = setup.inserts[(std::size_t) i];
                result.inserts[(std::size_t) i].gain =
                    gainFor (in.muted, false, false, i >= 1 && in.isolated, false, setup.settings);
            }

            return;
        }

        // ---- which soloed inserts still carry their own feeds ----
        //
        // Rule 3's exception, computed once per soloed insert rather than
        // once per pair. A soloed insert that is itself fed by something
        // soloed has been taken over by it: whoever soloed the upstream one
        // was asking for that one in particular, so the soloed bus's other
        // feeds go quiet. See the header comment for the case this is for,
        // which is the one a naive implementation fails.
        for (int i = 1; i < n; ++i)
        {
            if (! soloedAt (i))
                continue;

            for (int s = 1; s < n; ++s)
            {
                if (s != i && soloedAt (s) && reaches (s, i))
                {
                    result.takenOver[(std::size_t) i] = 1;
                    break;
                }
            }
        }

        for (int i = 0; i < n; ++i)
        {
            const auto& in = setup.inserts[(std::size_t) i];

            const bool selfSoloed = soloedAt (i);
            const bool isolated   = i >= 1 && in.isolated;

            bool byOthers = false;

            for (int s = 1; s < n && ! byOthers; ++s)
            {
                if (s == i || ! soloedAt (s))
                    continue;

                // Rule 2: something soloed feeds this insert, so this insert
                // has to pass it on.
                if (reaches (s, i))
                    byOthers = true;

                // Rule 3: this insert feeds something soloed, so the soloed
                // thing has something to play, unless that soloed thing has
                // been taken over from upstream.
                else if (reaches (i, s) && result.takenOver[(std::size_t) s] == 0)
                    byOthers = true;
            }

            auto& state = result.inserts[(std::size_t) i];

            state.soloedByOthers       = byOthers && ! selfSoloed;
            state.mutedByOthersSoloing = ! selfSoloed && ! byOthers && ! isolated;
            state.gain                 = gainFor (in.muted, selfSoloed, byOthers,
                                                  isolated, true, setup.settings);
        }
    }

    //==========================================================================
    /** What a click on a solo button means.

        One function for the preference and the modifier together, following
        Ardour, where a plain click under `exclusive-solo` and a modified click
        without it reach the same code. Two separate paths would be two things
        to keep in step.

        Additive is the default: Ardour ships `exclusive-solo = false`, and
        Logic documents additive solo with a modifier for the other. The
        clearing gesture is Logic's, whose manual gives both uses of the same
        modifier click in two sentences: soloing an inactive strip "unsolos any
        other channel strip", and on an active one, "The solo state of all
        channel strips is deactivated".
    */
    enum class Gesture
    {
        toggle,      /**< turn this insert's own solo on or off, leaving others */
        exclusive,   /**< solo this one and clear every other */
        clearAll     /**< clear every solo, including this one */
    };

    inline Gesture gestureFor (bool exclusivePreference, bool modifierHeld,
                               bool alreadySelfSoloed) noexcept
    {
        // The cheapest possible way out of a solo nobody can find, so it is
        // tested before anything else: whatever the preference says, a
        // modified click on something already soloed clears the lot.
        if (modifierHeld && alreadySelfSoloed)
            return Gesture::clearAll;

        const bool wantExclusive = modifierHeld ? ! exclusivePreference
                                                : exclusivePreference;

        // Exclusive has nothing to say about turning a solo off, and Ardour's
        // own condition is the same: its exclusive path is guarded on the
        // route not already being self soloed, so a second click on a soloed
        // strip is an ordinary toggle in either preference.
        return wantExclusive && ! alreadySelfSoloed ? Gesture::exclusive
                                                    : Gesture::toggle;
    }
}
