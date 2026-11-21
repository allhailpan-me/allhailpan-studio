#pragma once

//==============================================================================
/** What the transport silences, and how gently, for each thing that can
    happen to the play head.

    Four booleans go in and a set of messages comes out. That is all this is,
    and it is a header of its own for the reason `LatencyGraph.h` and
    `RecordAlign.h` are: the decision used to live in `AudioEngine::scheduleMidi`
    among the device callback, where nothing could reach it, and three separate
    faults lived in it undisturbed. Policy over four booleans is a table, and a
    table can be asserted.

    THE THING THAT WAS WRONG

    The engine sent the whole sequence, up to and including All Sound Off, on a
    stop, on a play head jump, **and on every loop wrap**. The two faults that
    follow from the wrap are the ones somebody hears:

    A chord held on a controller while a looping arrangement plays was released
    and silenced at the loop point, and nothing brought it back, because the
    live notes were handed note offs along with the clip notes. Playing along
    with a loop is the ordinary way to use one.

    And what was heard and what was recorded disagreed, which is the one thing
    this studio is least willing to be wrong about. The MIDI recorder reads the
    live input rather than what the engine schedules, so a pad held across the
    wrap was cut at the loop point in the monitoring and ran straight through
    it in the take.

    `AudioEngine::scheduleMidi` was also the only place in the studio that
    reached for All Sound Off at all. Pressing Stop a second time is documented
    in `MainComponent` as the panic button, the escalation for plugins that
    drone on regardless of note offs. The first press already sent the harshest
    message there is, so there was nothing left for the second one to escalate
    to.

    WHAT EACH EVENT GETS, AND WHY

    - **A loop wrap is a continuation.** Nothing stronger than note offs for the
      clip notes the engine itself is holding, and those only because a clip
      note whose off beat lies past the loop end never reaches its off and would
      hang. Everything else sounding is the continuation of what was just heard.
      That is not a new opinion: the device callback already says exactly this,
      twice and 33 lines apart, about this same event, when it declines to drop
      the delay compensation buffers at a wrap and declines to step the gain
      ramps at one. The MIDI half of the same callback contradicted both of
      them.

    - **A stop or a jump releases, and does not cut.** Note offs for everything
      the engine is tracking, the sustain pedal lifted first so a pedalled note
      is not left held by it, and All Notes Off behind them for notes the engine
      is not tracking, which is what a hosted arpeggiator or sequencer may be
      holding by itself. All Notes Off is specified as a release: it "should
      force voices to go to the release stage of the envelope, and not terminate
      the sound of the notes abruptly" (MIDI 1.0 Detailed Specification,
      Appendix, "The Relationship Between the Hold Pedal and 'All Notes
      Off'"). A tail ringing out after Stop is what every other release path
      in this studio already does.

      The page numbers this citation and the one further down carried were
      removed rather than corrected, and so were the document version and MMA
      number beside them: the MMA printing cannot be opened from the sandbox,
      so none of it was ever verified. The heading was wrong too, missing its
      first four words. A heading that can be checked is worth more than a page
      number that cannot.

      This quote is the one in this file that was read on a page: it is on
      SOT5.HTM of the IEEE Computer Society Press reprint of the specification
      body, Haus (ed.), Notes About MIDI Specifications, 1995, which is where
      `HoldPedal.h` reads its three hold pedal sentences. The All Sound Off
      quote further down is NOT: see the note against it.

    - **A wrap does not lift the sustain pedal, and the pedal is modelled now,
      which is what makes that answer defensible.** It used to lift it, and
      that was a fault: nothing tracked the pedal's real position, so lifting
      it at the loop point turned sustain off for the rest of the pass.

      Not lifting it had its own cost while the pedal was still unmodelled,
      and this paragraph used to end by saying the pedal was not modelled at
      all and was waiting in `Research/BACKLOG.md`. It is not waiting any
      more: `HoldPedal.h` is it. What remains true is that the clip note offs
      going out at the wrap are latched by a pedal that is still down, so a
      clip note near the end of a loop that the next pass does not retrigger
      rings on until the player lifts their foot.

      What changed is that the studio now knows it is holding that note
      instead of having forgotten it. The note was always going to be released
      by the next lift of the foot, with or without the model, because its
      note off is sitting in the receiver and the receiver is required to be
      holding it; that much an audit corrected in an earlier draft of this
      paragraph. What the model buys is that a release which does lift the
      pedal, which is every one except this wrap, now sends an off for it as
      well, and that the wrap sends it nothing, where a version of this change
      re-sent its off on every pass for as long as the foot stayed down.

      Which answer a wrap should give is a real disagreement between studios
      rather than something a specification settles: Ardour lifts the pedal at
      every wrap by default, Reaper offers it as a preference, and Logic
      chases rather than panicking. This studio keeps its own position, that a
      wrap is a continuation.

    - **Only a panic cuts.** All Sound Off is the one message defined to ignore
      the release: "all notes currently on are turned off and their volume
      envelopes are set to zero as soon as possible" (same document, "Channel
      Mode Messages"). That is the user's literal request when they ask for
      silence and it is wrong as an answer to anything else.

      **Not verified, and said rather than implied.** The page carrying the
      Channel Mode Messages section could not be reached from the sandbox, so
      this quote came back through a research agent rather than from a page
      read here, and it is attributed to its section for that reason. It is
      the same provenance as the sentence `HoldPedal.h` marks as its one
      unverified quote, and for the same reason: the same unreachable section.

      That All Sound Off is not latched by the hold pedal either is a separate
      claim, it is universally believed, and no normative sentence for it was
      found anywhere in MIDI 1.0 or its addenda. It is written down here as a
      belief rather than as a quotation. Nothing in this studio rests on it: a
      panic lifts the pedal before All Sound Off goes out, so whether a
      receiver would have latched it never decides anything.

    The escalation is the property to hold on to: a panic does everything a stop
    does and more, so pressing Stop twice is always strictly stronger than
    pressing it once. The test asserts that directly rather than trusting the
    table to stay that way.

    No JUCE.
*/
namespace TransportSilence
{

/** What just happened to the play head. More than one can be true in the same
    block: a stop that also wrapped, say, or a jump that landed on the end. */
struct Event
{
    bool stopped   = false;   /**< the transport went from running to not */
    bool jumped    = false;   /**< the play head was moved, by a locate or a rewind */
    bool wrapped   = false;   /**< the loop, or the end of the song, came round */
    bool panicking = false;   /**< somebody asked for silence */
};

/** What to send, declared in the order it has to go out in.

    The pedal is first because a note it is holding up survives a note off, and
    survives All Notes Off too, until it is lifted. The two channel mode
    messages are last, and the gentler one is ahead of the harsher one so that
    a receiver which knows only the older of the two still does the right
    thing.

    The order is written down here rather than left to the engine because the
    engine had it wrong, with a comment above it claiming otherwise, and an
    audit found that by reading the buffer rather than the comment.
*/
struct Action
{
    bool sustainUp    = false;   /**< controller 64 to nothing */
    bool clipNoteOffs = false;   /**< note offs for the clip notes the engine holds */
    bool liveNoteOffs = false;   /**< note offs for what is being played in live */
    bool allNotesOff  = false;   /**< controller 123: release every voice */
    bool allSoundOff  = false;   /**< controller 120: silence every voice at once */

    bool anything() const noexcept
    {
        return sustainUp || clipNoteOffs || liveNoteOffs || allNotesOff || allSoundOff;
    }

    /** True when this does everything `other` does. The escalation property
        leans on it, and so does anybody reasoning about two events landing in
        the same block. */
    bool covers (const Action& other) const noexcept
    {
        return (sustainUp    || ! other.sustainUp)
            && (clipNoteOffs || ! other.clipNoteOffs)
            && (liveNoteOffs || ! other.liveNoteOffs)
            && (allNotesOff  || ! other.allNotesOff)
            && (allSoundOff  || ! other.allSoundOff);
    }
};

/** The table.

    Written as the strongest reason first rather than as a set of independent
    flags, because the events are not independent: a block can stop and wrap at
    once, and what should go out then is whatever the stronger of the two asks
    for. Reading it the other way, as flags that accumulate, is how the wrap
    came to send All Sound Off in the first place.
*/
inline Action actionFor (const Event& event) noexcept
{
    Action action;

    if (event.panicking)
    {
        action.clipNoteOffs = true;
        action.liveNoteOffs = true;
        action.sustainUp    = true;
        action.allNotesOff  = true;
        action.allSoundOff  = true;   // the only event that gets this one
        return action;
    }

    if (event.stopped || event.jumped)
    {
        action.clipNoteOffs = true;
        action.liveNoteOffs = true;
        action.sustainUp    = true;
        action.allNotesOff  = true;
        return action;
    }

    if (event.wrapped)
    {
        // Only the clip notes, and only because a note whose off beat lies past
        // the loop end never reaches it. Anything else still sounding is the
        // continuation of what was just heard.
        action.clipNoteOffs = true;
        return action;
    }

    return action;
}

} // namespace TransportSilence
