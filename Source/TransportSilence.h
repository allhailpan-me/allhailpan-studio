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
      That is not a new opinion: `scheduleMidi` already says exactly this, twice,
      about this same event, when it declines to drop the delay compensation
      buffers and declines to step the gain ramps at a wrap. The MIDI half
      contradicted both of them.

    - **A stop or a jump releases, and does not cut.** Note offs for everything
      the engine is tracking, the sustain pedal lifted first so a pedalled note
      is not left held by it, and All Notes Off behind them for notes the engine
      is not tracking, which is what a hosted arpeggiator or sequencer may be
      holding by itself. All Notes Off is specified as a release: it "should
      force voices to go to the release stage of the envelope, and not terminate
      the sound of the notes abruptly" (MIDI 1.0 Detailed Specification v4.2.1,
      MMA 96.1-4, Appendix, "Hold Pedal and 'All Notes Off'", p. A-5). A tail
      ringing out after Stop is what every other release path in this studio
      already does.

    - **Only a panic cuts.** All Sound Off is the one message defined to ignore
      the release: "all notes currently on are turned off and their volume
      envelopes are set to zero as soon as possible" (same document, "Channel
      Mode Messages", p. 25). That is the user's literal request when they ask
      for silence and it is wrong as an answer to anything else.

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

/** What to send. The engine sends them in the order they are declared, which
    is the order they have to go out in: the pedal has to be lifted before the
    note offs or a pedalled note is still being held up by it when they
    arrive. */
struct Action
{
    bool clipNoteOffs = false;   /**< note offs for the clip notes the engine holds */
    bool liveNoteOffs = false;   /**< note offs for what is being played in live */
    bool sustainUp    = false;   /**< controller 64 to nothing */
    bool allNotesOff  = false;   /**< controller 123: release every voice */
    bool allSoundOff  = false;   /**< controller 120: silence every voice at once */

    bool anything() const noexcept
    {
        return clipNoteOffs || liveNoteOffs || sustainUp || allNotesOff || allSoundOff;
    }

    /** True when this does everything `other` does. The escalation property
        leans on it, and so does anybody reasoning about two events landing in
        the same block. */
    bool covers (const Action& other) const noexcept
    {
        return (clipNoteOffs || ! other.clipNoteOffs)
            && (liveNoteOffs || ! other.liveNoteOffs)
            && (sustainUp    || ! other.sustainUp)
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
