#pragma once

//==============================================================================
/** Whether a player hears their own input, and what the transport has to do
    with it.

    The studio had one setting here and nothing else: monitoring off, on for
    the armed strips, or on for every strip with an input. All three answers
    are the same answer whatever the transport is doing, which is the part
    that was missing. A strip left armed is heard live over the whole of
    playback, so listening back to a take means hearing yourself over it, and
    somebody punching into a vocal hears their own voice across the entire
    pass rather than only across the punch.

    Every studio looked at has a rule for this, and two of them arrived at the
    same three cases under the same name. Reaper's User Guide 7.26 has
    "Monitor Input (Tape Auto Style)", which "monitors input only when the
    transport has stopped or when you are recording", and "During playback you
    will hear only the timeline audio, not the input." Steinberg's Preferences
    > VST > Auto Monitoring has "Tapemachine Style", which "Activates input
    monitoring in stop mode and during recording, but not during playback."
    Two manufacturers, the same rule, the same machine in the name. That is
    why this is implemented as the published behaviour rather than as
    something invented here: a musician who has used either one already knows
    what it does.

    The three rules are Cubase's three arm dependent modes, in Steinberg's own
    words, from the shared Cubase and Nuendo preferences topic
    `preferences_vst_r.html`:

    - "While Record-Enabled": "Connects the audio source to the channel input
      when you click Record Enable."
    - "While Record Running": "Switches to input monitoring during recording
      only."
    - "Tapemachine Style": as quoted above.

    Steinberg's fourth mode, "Manual", "Turns input monitoring on or off when
    you click Monitor", is `Choice::always` here and is deliberately outside
    the rule: it is somebody saying they want to hear the input, and a rule
    that overrode that would be answering a question nobody asked. Ardour
    draws the same line in the same place, by returning from
    `Route::monitoring_state()` before the transport is ever read when the
    route's own choice is not `MonitorAuto` (`route.cc:6622`).

    Kept free of JUCE so the whole table can be tested, which is worth doing
    for the reason `LatencyGraph.h` is: nothing in here can announce that it
    is wrong. A monitoring rule with one cell transposed does not error, does
    not crackle and looks right in the menu. It is heard once, as "I cannot
    hear myself", by somebody holding a guitar who then has to work out
    whether to blame the studio, the interface or the cable.

    ## What this decides, and what it deliberately does not

    This decides whether the **live input** is added to an insert. It does not
    touch what the arrangement is playing back on that insert, and that
    boundary is a real one rather than a convenience, so it is worth being
    exact about where it lies.

    In Ardour, monitoring is not a boolean about the input at all. It is one
    three valued decision over the input and the playback together:
    `MonitorState` is `MonitoringSilence`, `MonitoringInput`, `MonitoringDisk`
    or `MonitoringCue`, where the last is the first two together
    (`types.h:536`), and `disk_reader.cc` applies it by construction rather
    than by a rule: the disk gain goes to zero unless the `MonitoringDisk` bit
    is set (`:330`), the disk is read into a scratch buffer instead of the
    output when `MonitoringInput` is set (`:435`), and it is mixed back in
    only for the state that asks for both (`:500`). So in Ardour, monitoring
    your input means you are not hearing that track's existing material, and
    hearing both at once is a state with a name, reached only through
    `RecSoundOnSound` record mode or inside the record pre-roll window.

    Hearing both at once is what this studio does today, for the whole time a
    strip is armed. This rule does not create that state and cannot remove it:
    what it does is shrink it from "the entire time a strip is armed" to "only
    while a take is actually being recorded", under the rule somebody chose.

    Removing it altogether is a different feature, and naming it is the point
    of this paragraph. It is Ardour's `RecordMode { RecLayered, RecNonLayered,
    RecSoundOnSound }`, a session setting that the monitoring decision reads
    rather than decides (`audio_track.cc:190`), and building it here means the
    arrangement being able to gate one playlist track's clips while a take is
    running. That is layered against sound on sound recording, which is a
    control of its own with a night of its own. It is not added here as a
    `disk` or `both` state that nothing could produce, because a declared
    state nothing reaches is the failure this project keeps meeting.

    ## Latency: nothing moves, and that is a finding

    An insert carrying a live input is exempt from delay compensation, and
    that exemption is deliberately not conditioned on whether the input is
    being heard right now: `updateLatency` is driven by the monitoring choice
    and the input assignment, both of which change on the message thread,
    while the arm and the recorder change without a graph update. So an insert
    can be exempt while this rule is saying nothing is audible, exactly as one
    could already be exempt while unarmed, and the trade is the one documented
    at that line: being exempt when it did not have to be costs the alignment
    of other material on that insert, and being compensated when it should not
    be is latency the player feels.

    **So a monitoring rule is not something `updateLatency` has to be called
    for.** That is worth stating rather than leaving to be rediscovered: it is
    true only because every rule in here is a function of the transport and
    the arm, which the graph already refuses to depend on.

    ## The ramp was already there

    A monitoring change is a gain discontinuity. Ardour spends `route.cc:491`
    to `:494` and `disk_reader.cc:331` to `:368` on declicking one, with a
    preference distinct from its transport fades, and Pro Tools publishes a
    four millisecond monitor only crossfade as a floor it always applies. This
    engine's monitor path already ramps, at `GainRamp::levelSeconds`, which is
    the five milliseconds every other level in the mixer travels at.

    That matters here because a transport aware rule switches monitoring at
    moments nothing used to switch it: the first beat of playback, the punch
    the player is rolling into, the stop at the end. Those switches are
    clickless because the ramp in `mixMonitorInput` was already there, not
    because this header does anything about it.
*/
namespace MonitorRule
{
    //==========================================================================
    /** What the studio has been asked to monitor. The transport bar's own
        control, and the outer gate on everything below.

        The values are fixed and must stay so: the toolbar stores the combo
        box id and casts the id minus one to this, so moving them would make
        every existing settings file mean something else. `Prefs::monitorMode`
        is the matching list.

        `armed` is the only one the rule below applies to. `off` is nothing and
        `always` is everything with an input, and both are somebody stating an
        answer rather than asking for one.
    */
    enum class Choice
    {
        off    = 0,
        armed  = 1,
        always = 2
    };

    //==========================================================================
    /** What "armed" means while the transport is moving.

        Ordered so that the first entry is the one that is safe to fall back
        to, which is the discipline every other choice list in this project
        follows: it is what the studio did before this header existed, so a
        stored value nobody recognises leaves monitoring behaving exactly as
        it always has rather than silently taking a player's input away.
    */
    enum class Rule
    {
        /** Cubase's "While Record-Enabled": "Connects the audio source to the
            channel input when you click Record Enable". Transport independent,
            and what this studio has always done. */
        whileArmed = 0,

        /** Cubase's "While Record Running": "Switches to input monitoring
            during recording only". An armed strip is silent until a take is
            running, including while the transport is stopped. */
        whileRecording = 1,

        /** Cubase's "Tapemachine Style" and Reaper's "Tape Auto Style":
            input when stopped and while recording, the arrangement alone
            during playback.

            The one case that separates this from `whileRecording` is the
            transport being stopped, which is a single cell of the table.
            Steinberg's two sentences differ in nothing else, so a table that
            collapses that cell has silently built two identical rules, and
            the test asserts that they differ there. */
        tapeMachine = 2
    };

    inline constexpr int numRules = 3;

    /** What a project that has never been told starts on.

        Deliberately not Ardour's answer. Ardour ships `auto_input = true`
        (`session_configuration_vars.inc.h:38`), so out of the box an armed
        Ardour track hears the disk during playback rather than its input.
        Two reasons for not following it, and the first is the stronger.

        Changing what comes out of a musician's headphones is the last thing
        to do without being asked, and this default is the one that changes
        nothing: every project made before this header, and every project
        made by somebody who never opens the menu, monitors exactly as it did.

        And playing along to the arrangement before pressing record is a real
        use, which is the one the tape machine rule takes away. It is why
        Cubase offers "While Record-Enabled" at all, and Reaper's own default
        is plain input monitoring rather than its Tape Auto Style: "By
        default, Input Monitoring should be on."

        The tape machine rule is the one to recommend to somebody who punches
        in. Recommending is a release note's job, not a default's.
    */
    inline constexpr Rule defaultRule = Rule::whileArmed;

    //==========================================================================
    /** What the transport is doing, latched once per block.

        Its own type rather than a loose bool so that it cannot be swapped
        with one of the insert's three at a call site. That is not a
        hypothetical: the flags handed to `setSoloSettings` were documented in
        the wrong order for a while, and an audit is what found it.

        There is no `recording` member, and that is the divergence from Ardour
        worth knowing about. Ardour's rolling and not recording row is decided
        by `session_rec`, the session's own record state (`route.cc:6710`),
        while here the question is asked of the insert's own recorder instead.
        The engines differ in the underlying fact rather than in the intent:
        arming a track mid pass in Ardour starts capturing it, and arming a
        strip mid pass here does not, because the recorders are all begun once
        in `startAudioRecording`. So "is this strip being recorded" is
        answered here by the thing that knows, which is the recorder, and
        `mixMonitorInput` had already chosen that discipline for the same
        reason: "This insert's own recorder, not the instrument one."

        It also disposes of the count-in without a special case. The recorders
        begin before the transport rolls, so an armed strip is monitored
        through the count, which is also Ardour's answer inside its record
        pre-roll window (`route.cc:6722`): the player needs to hear themselves
        coming in.
    */
    struct Transport
    {
        /** The transport is moving, whether or not anything is being
            recorded. True through a count-in. */
        bool rolling = false;
    };

    /** One insert's own state, as the monitoring decision needs it. */
    struct Insert
    {
        /** This insert has an input that resolves to channels the device
            really has. Nothing else in here can make an input audible
            without it, which is why it is first: the question "is this
            audible" has to be answerable as no for a strip with nothing
            plugged into it, under every rule. */
        bool hasInput = false;

        /** The strip is armed to record. */
        bool armed = false;

        /** This insert's own recorder is running, which is this engine's
            answer to "is this strip being recorded". */
        bool capturing = false;
    };

    //==========================================================================
    /** Whether this insert's live input should be heard.

        The whole decision in one place, so that the engine asks rather than
        decides and the table can be tested without a device, a project or a
        transport.
    */
    inline constexpr bool inputAudible (Choice choice, Rule rule,
                                       Transport transport, Insert insert) noexcept
    {
        // Nothing plugged in, or an assignment the device cannot satisfy.
        // Before everything else because no rule and no choice may make a
        // socket that is not there audible.
        if (! insert.hasInput)
            return false;

        switch (choice)
        {
            case Choice::off:    return false;
            case Choice::always: return true;
            case Choice::armed:  break;
        }

        // The recorder as well as the arm, so that a take already running
        // keeps being monitored if the arm is cleared under it. The existing
        // engine comment is the argument: clearing an arm mid take should not
        // deafen the player who is still playing.
        if (! (insert.armed || insert.capturing))
            return false;

        switch (rule)
        {
            case Rule::whileArmed:     return true;
            case Rule::whileRecording: return insert.capturing;

            // Stopped or recording, and silent during playback. The two terms
            // are the two halves of Steinberg's sentence and of Reaper's.
            case Rule::tapeMachine:    return insert.capturing || ! transport.rolling;
        }

        // A value this header does not define, which only a bad cast can
        // produce. Silence rather than the first rule's answer: an input the
        // studio cannot name the rule for should not be opened into whatever
        // the player is monitoring through, and `ruleFromStored` means a
        // loaded project never arrives here.
        return false;
    }

    //==========================================================================
    /** The number a project stores for a rule.

        Numbered rather than named, which is the choice `SendTap.h` made and
        for the same reason: the mixer settings saved in the same element are
        already small integers, and what protects a saved choice is the
        mapping being explicit rather than it being a string. The numbers are
        fixed for ever. A fourth rule takes 3.
    */
    inline constexpr int ruleToStored (Rule rule) noexcept
    {
        switch (rule)
        {
            case Rule::whileArmed:     return 0;
            case Rule::whileRecording: return 1;
            case Rule::tapeMachine:    return 2;
        }

        // The default's own number, written out rather than reached by
        // calling back into this function: a recursive fallback is one
        // changed default away from not terminating.
        return 0;
    }

    /** The rule a stored number means, falling back to the default for
        anything that is not one of them.

        Dropping an unreadable setting onto the rule that changes nothing is
        the same discipline the modulation matrix uses for an id nobody knows:
        a setting that is less than it was beats a setting that is wrong,
        because the second does not look broken.
    */
    inline constexpr Rule ruleFromStored (int stored) noexcept
    {
        switch (stored)
        {
            case 0:  return Rule::whileArmed;
            case 1:  return Rule::whileRecording;
            case 2:  return Rule::tapeMachine;
            default: return defaultRule;
        }
    }

    /** The rules in menu order, which is the order they are declared in. */
    inline constexpr Rule ruleAt (int index) noexcept
    {
        return ruleFromStored (index);
    }

    /** What the menu calls each rule.

        Here rather than in the mixer so that the words somebody reads and the
        behaviour they get cannot drift apart: both come out of this file.

        Named after what they do rather than after any product. "Tape machine"
        is the exception and it is not a product name: it is what the machines
        these rules come from actually were, and Steinberg and Reaper both
        reached for the same word independently.
    */
    inline constexpr const char* name (Rule rule) noexcept
    {
        switch (rule)
        {
            case Rule::whileArmed:     return "Monitor whenever a strip is armed";
            case Rule::whileRecording: return "Monitor only while recording";
            case Rule::tapeMachine:    return "Monitor like a tape machine: stopped and recording, not during playback";
        }

        return "Monitor whenever a strip is armed";
    }
}
