#pragma once

#include <cstddef>

//==============================================================================
/** The hold pedal, controller 64, as a receiver is required to treat it.

    This is the rule rather than the policy. `TransportSilence.h` decides what
    each thing that can happen to the play head should silence; this decides
    what a synth on the other end of the cable will actually do with what gets
    sent, which is the question the studio has never been able to answer.

    THE THING THAT WAS WRONG

    Nothing in the studio tracked the pedal's position.
    `AudioEngine::trackLiveMessage` tracked note on, note off, controller 120
    and controller 123, and nothing else. So the engine's own model of what
    was sounding cleared a note the moment it sent a note off for it, and the
    specification says that is exactly when it must not:

        "If Note Off messages are received while the hold pedal (controller 64
        (40H)) is 'on' ... they must be recognized but not acted upon until
        the release of the hold pedal."

        "The same is true for the All Notes Off message."

        "A Hold or Sustain pedal 'On' message takes priority over Note Off and
        All Notes Off until it is released."

    MIDI 1.0 Detailed Specification, appendix, "The Relationship Between the
    Hold Pedal and 'All Notes Off'". Quoted from the IEEE Computer Society
    Press reprint of the specification body, Haus (ed.), Notes About MIDI
    Specifications, 1995. The MMA printing of v4.2.1 could not be opened from
    the sandbox, so the section heading is the citation and the page numbers
    quoted elsewhere in this studio are unverified.

    So a note ringing on after the engine has released it is a conformant
    synth doing what it is required to do, and the engine sending a message it
    has no reason to believe took effect. Three faults follow from the model
    being wrong, and only one of them is the loop wrap everybody noticed:

    - **Switching the live channel left a pedalled note sounding for ever.**
      `scheduleMidi` sends note offs and All Notes Off to the channel being
      left and no pedal lift, so a pedal that is down latches both. And the
      lift, when the player's foot eventually comes up, is routed to the newly
      selected channel, because that is where live MIDI goes: the channel that
      was left never hears it at all. That note rings until somebody finds the
      panic button.

    - **At a stop, no note off was sent for a pedalled note at all**, because
      the engine had already forgotten it. That path works today only because
      the blanket pedal lift is ahead of it and All Notes Off is behind it, and
      the specification is explicit that the second of those is not something
      to lean on: "Since recognition of All Notes Off is not required of the
      receiver, all notes should first be turned off by transmitting individual
      Note Off messages."

    - **At a loop wrap the clip note offs are latched**, which is where this
      was found. The policy there is unchanged and deliberately so: see below.

    THE THRESHOLD, WHICH HAS ONE ANSWER AND LOOKS LIKE TWO

    Both claims in circulation are in one paragraph of the Control Change
    section, addressed to different parties. A transmitter *may* send 0 and
    127, which is the sentence people quote, and it is about the modulation
    wheel. A receiver **must** read the range:

        "If a receiver is expecting switch information it should recognize
        0-63 (00H-3FH) as "OFF" and 64-127 (40H-7FH) as "ON". This is because
        a receiver has no way of knowing whether the message information is
        from a switch or a continuous controller."

    The later restatement, AMEI/MMA M2-113-UM v1.0 section 6, keeps only the
    receiver rule. This studio is both: it reads an incoming pedal by
    threshold, and it emits nothing but zero. Which brings us to the thing
    this header exists to stop somebody doing.

    NEVER PUT THE PEDAL BACK DOWN

    The obvious fix for a latched note off is to lift the pedal, send the note
    off, and put the pedal back down, because the player's foot still is. It
    is wrong, and it is wrong in the way this project keeps meeting: it
    measures as working on almost anything and fails on the one instrument a
    sustain pedal is for. M2-113-UM section 6:

        "If the level of a note that has been released ... is greater than the
        envelope Sustain level, the device should switch back to the Decay or
        Sustain portion of the envelope ... So, for example, an Organ note,
        having received a note-off followed by a Damper ON, will not be
        'caught' by the damper. A piano note, however, with its Sustain level
        of zero, would be 'caught.'"

    A piano's sustain level is zero, so every note just released is still
    above it, so every one is specified to be re-caught with its decay
    restarted. The sequence releases nothing and restarts everything. No
    established studio does it either: six places in Ardour write controller
    64 and every one of them writes zero.

    That is why `releaseSequence` cannot express a pedal press, and why its
    test asserts over every reachable state that no message it emits is a
    controller 64 above 63. The rule is in the type rather than in a comment.

    WHAT THE FIELD DOES AT A LOOP WRAP, WHICH IS DISAGREE

    No specification covers what a sequencer re-sends when the play head
    moves. There is no normative text on chasing anywhere in MIDI 1.0 or its
    addenda, so this paragraph is convention and is written as convention.

    Ardour lifts the pedal and leaves it up, at a stop and, by default, at
    every loop wrap: `midi_panic_when_looping` is true, switchable under
    "Output Port Panic". Reaper offers the same thing as a preference, "Reset
    MIDI CC/Pitch on playback start and/or playback stop and/or playback
    loop/skip". Logic chases instead of panicking, and is the only one that
    names this exact case: "Notes: Sustained", which "Sends any notes that are
    still playing at the playback start point (due to a sustain pedal event)".
    Cubase's cycle behaviour is not in its published documentation.

    This studio keeps its own position, which is that a loop wrap is a
    continuation, so the pedal is not lifted there. What changes is that the
    wrap now keeps a true record of what it left latched, so those notes are
    released by the next lift of the foot, which is what a pedal means, or by
    the next stop, which lifts the pedal properly. Before, the engine forgot
    them, and a forgotten note can only be got rid of with a panic.

    WHAT IS NOT MODELLED, SAID RATHER THAN IMPLIED

    Sostenuto, controller 66, which latches only the notes already down when
    it arrives, and the soft pedal, controller 67. Neither is sent or tracked
    anywhere in the studio, and a half implemented sostenuto would be worse
    than none.

    Half damper is deliberately absent. M2-113-UM permits it and specifies
    nothing about it: "Default Control Change Mapping Profile Devices may also
    respond to Hold1 as a continuous controller." There is no published
    mapping from a value to a damper position, so any curve here would be
    invented, and the threshold is what every receiver is required to do.

    No JUCE.
*/
namespace HoldPedal
{

/** Controller 64. */
inline constexpr int kController = 64;

/** Controller 121, Reset All Controllers, which takes the pedal to nothing on
    a conformant device: MMA RP-015, reproduced in M2-113-UM appendix A, where
    controller 64's "Reset All Controllers" column is 0. The specification
    tells a sequencer not to rely on it, and this studio does not: it sends an
    explicit pedal lift of its own, which is the advice given under Reset All
    Controllers ("should send what they believe to be the initial state of all
    controllers first, followed by this message"). It is handled here because
    a control surface may send it, and the model has to stay true to whatever
    the plugin was told. */
inline constexpr int kResetAllControllers = 121;

inline constexpr int kAllSoundOff = 120;
inline constexpr int kAllNotesOff = 123;

/** The lowest value a receiver must read as a pedal that is down. */
inline constexpr int kOnFrom = 64;

inline constexpr int kNotes = 128;

/** The receiver's half of the threshold. Masked to seven bits because a data
    byte is seven bits, so a caller handing over a raw byte with the high bit
    set gets the answer the wire would have given. */
inline bool isDown (int value) noexcept
{
    return (value & 0x7f) >= kOnFrom;
}

/** What one key is doing.

    `latched` is the state that did not exist before and is the whole point:
    the note has had its release asked for and the pedal is holding it up, so
    it is still sounding and it still needs a note off of its own once the
    pedal is out of the way. Stored as a byte so it can live in the rows the
    engine already keeps, and ordered so that the test every release path
    already uses, "is this anything other than nought", keeps its meaning. */
enum Key : unsigned char
{
    silent   = 0,
    sounding = 1,
    latched  = 2
};

/** Whether this key still owes a note off. True for a latched key: that is
    the difference between this and what the engine used to believe. */
inline bool needsNoteOff (unsigned char key) noexcept
{
    return key != silent;
}

//==============================================================================
/** One receiver: the keys and the pedal for one MIDI channel of one plugin.

    Pointers into rows the engine owns rather than storage of its own, because
    the engine already keeps exactly these rows and the point of this header is
    to take the rule off it, not the memory.

    `clip` may be null. The arrangement's own notes all go out on MIDI channel
    1, so only that channel has a second row, and a pedal pressed on channel 1
    latches the arrangement's notes as well as the player's because they reach
    one receiver through one cable. That is the loop wrap fault in one
    sentence.
*/
struct Channel
{
    unsigned char* live = nullptr;   /**< 128 keys the player is holding */
    unsigned char* clip = nullptr;   /**< 128 keys the arrangement is holding, or null */
    unsigned char* down = nullptr;   /**< one byte: whether the pedal is down */

    bool pedalDown() const noexcept  { return down != nullptr && *down != 0; }
    bool valid() const noexcept      { return live != nullptr && down != nullptr; }
};

//==============================================================================
/** A key is struck. A note on for a key the pedal is holding up is a
    retrigger: it is sounding again on its own account, and the release it was
    owed is gone, which is what a player hears when they replay a note with
    the pedal down. */
inline void noteOn (unsigned char* keys, int note) noexcept
{
    if (keys != nullptr && (unsigned) note < (unsigned) kNotes)
        keys[note] = sounding;
}

/** A key is released. Deferred rather than acted on while the pedal is down,
    which is the normative sentence at the top of this file. */
inline void noteOff (unsigned char* keys, int note, bool pedalDown) noexcept
{
    if (keys == nullptr || (unsigned) note >= (unsigned) kNotes)
        return;

    if (keys[note] == silent)
        return;

    keys[note] = pedalDown ? latched : silent;
}

/** All Notes Off. "The same is true for the All Notes Off message", so every
    sounding key is deferred by a pedal that is down exactly as an individual
    note off would be. Written as the same transition for each key rather than
    as its own rule, because it is the same rule. */
inline void allNotesOff (unsigned char* keys, bool pedalDown) noexcept
{
    if (keys == nullptr)
        return;

    for (int note = 0; note < kNotes; ++note)
        noteOff (keys, note, pedalDown);
}

/** All Sound Off, which the pedal does not latch: it is specified to zero the
    envelopes rather than release them, "as soon as possible", with no
    exemption for anything. The pedal's own position is untouched, because the
    player's foot has not moved: this silences voices, it does not lift a
    controller. */
inline void allSoundOff (unsigned char* keys) noexcept
{
    if (keys == nullptr)
        return;

    for (int note = 0; note < kNotes; ++note)
        keys[note] = silent;
}

/** The pedal moves. Returns whether it is now down.

    Everything latched is released whenever it is not down, rather than only on
    a transition from down. The two differ just when something is latched with
    the pedal already up, which nothing here can produce, and in that case
    releasing it is the repair and keeping it is a note nobody can turn off. */
inline bool setPedal (const Channel& c, int value) noexcept
{
    if (! c.valid())
        return false;

    const bool nowDown = isDown (value);
    *c.down = nowDown ? (unsigned char) 1 : (unsigned char) 0;

    if (! nowDown)
    {
        for (int note = 0; note < kNotes; ++note)
        {
            if (c.live[note] == latched)
                c.live[note] = silent;

            if (c.clip != nullptr && c.clip[note] == latched)
                c.clip[note] = silent;
        }
    }

    return nowDown;
}

//==============================================================================
/** Feed one MIDI message in, as it arrived on the wire.

    The channel nibble is ignored: the caller has already resolved which
    channel this is, which is the only way the clip row can be attached to the
    right one.

    A note on with a velocity of nought is a note off, which is how a lot of
    hardware sends releases, and three bytes are required for a note message
    because one without a velocity is malformed. Both of those rules were in
    `AudioEngine::trackLiveMessage` and are here now so that they are tested
    rather than merely written.

    A note from the wire only ever reaches the live row: the arrangement's
    notes are the engine's own doing and are recorded where it schedules them.
    A channel mode message reaches both, and has to. If a control surface
    sends All Notes Off, the plugin releases the arrangement's notes along
    with the player's, so an engine that kept its clip row would go on
    believing it was holding notes that are already gone and would send their
    offs again at the next stop.
*/
inline void receive (const Channel& c, const unsigned char* data, int numBytes) noexcept
{
    if (! c.valid() || data == nullptr || numBytes < 2)
        return;

    const int status = data[0] & 0xf0;
    const int first  = data[1] & 0x7f;

    if (status == 0x90 && numBytes >= 3)
    {
        if ((data[2] & 0x7f) != 0)
            noteOn (c.live, first);
        else
            noteOff (c.live, first, c.pedalDown());

        return;
    }

    if (status == 0x80 && numBytes >= 3)
    {
        noteOff (c.live, first, c.pedalDown());
        return;
    }

    // A controller message with no value byte is malformed, so it is ignored
    // rather than read as a value of nought. Reading it as nought would lift
    // a pedal nobody lifted.
    if (status != 0xb0 || numBytes < 3)
        return;

    const int value = data[2] & 0x7f;

    if (first == kController || first == kResetAllControllers)
    {
        // Reset All Controllers takes the pedal to its initial value, which
        // for controller 64 is nothing, whatever value byte it carries.
        setPedal (c, first == kController ? value : 0);
        return;
    }

    if (first == kAllNotesOff)
    {
        allNotesOff (c.live, c.pedalDown());

        if (c.clip != nullptr)
            allNotesOff (c.clip, c.pedalDown());

        return;
    }

    if (first == kAllSoundOff)
    {
        allSoundOff (c.live);
        allSoundOff (c.clip);
    }
}

//==============================================================================
/** One MIDI message to send. */
struct Message
{
    unsigned char status = 0, data1 = 0, data2 = 0;
};

/** The most `releaseSequence` can ever write: a pedal lift, a note off for
    every key of both rows, and the backstop. */
inline constexpr int kMaxReleaseMessages = 1 + kNotes + kNotes + 1;

/** Which of a channel's two sets of keys a release is about.

    They are separate because the engine's reasons for releasing them are
    separate: switching the live channel releases what the player is holding
    and leaves the arrangement playing, and a loop wrap is the other way
    round. */
struct Rows
{
    bool live = true;   /**< what the player is holding */
    bool clip = true;   /**< what the arrangement is holding */
};

/** The messages that leave a conformant receiver with nothing sounding on this
    channel, in the order they have to go out, and the model updated to match.

    The two are deliberately one call. Sending the messages and recording that
    they were sent are the halves that drifted apart in the first place, and a
    caller that can do one without the other will eventually do one without the
    other.

    THE ORDER IS THE SPECIFICATION, not a preference. The pedal first, because
    everything behind it is latched otherwise. Then a note off for each key,
    because "recognition of All Notes Off is not required of the receiver, all
    notes should first be turned off by transmitting individual Note Off
    messages". Then All Notes Off last, if a backstop is wanted, for whatever a
    hosted arpeggiator is holding that the engine never saw.

    `midiChannel` is one to sixteen.

    `liftPedal` decides whether the pedal comes up as part of this, and it is
    the caller's to decide because the answer is a policy and differs between
    them. A stop lifts it. Switching the live channel has to, because the
    channel being left can never hear the lift afterwards. A loop wrap does
    not, because a wrap is a continuation and taking the player's foot off for
    the rest of the pass is a worse answer than the one it fixes. A caller
    that wants it lifted only where this model says it is down can ask
    `pedalDown()` first, which is cheaper than a wrong default.

    **What the pedal is doing decides what the keys become, which is the
    reason this is one function rather than a loop at each call site.** If the
    pedal stays down, the note offs this writes are deferred by the receiver,
    so the keys become `latched` and still owe a release. Recording them as
    silent is exactly the lie the studio used to tell: the engine forgot them,
    so nothing sent their offs again and nothing but a panic could reach them.

    Returns how many messages were written, or -1 if `capacity` could not hold
    them, in which case nothing is written and nothing is changed. Refused
    rather than truncated: a truncated release is a silently dropped note off,
    which is the fault this whole header is about.
*/
inline int releaseSequence (const Channel& c, int midiChannel, Rows rows,
                            bool liftPedal, bool backstop,
                            Message* out, int capacity) noexcept
{
    if (! c.valid() || out == nullptr)
        return 0;

    const unsigned char channel = (unsigned char) ((midiChannel - 1) & 0x0f);

    // Where the pedal is left, which is what the keys this releases become.
    const bool stillDown = c.pedalDown() && ! liftPedal;

    unsigned char* const selected[2] { rows.clip ? c.clip : nullptr,
                                       rows.live ? c.live : nullptr };

    int needed = (liftPedal ? 1 : 0) + (backstop ? 1 : 0);

    for (auto* keys : selected)
        if (keys != nullptr)
            for (int note = 0; note < kNotes; ++note)
                if (needsNoteOff (keys[note]))
                    ++needed;

    if (needed > capacity)
        return -1;

    int count = 0;

    if (liftPedal)
        out[count++] = { (unsigned char) (0xb0 | channel), (unsigned char) kController, 0 };

    // A note off per key, the arrangement's before the player's only so that
    // the order is fixed and reviewable. Both rows reach the same voices, so
    // no receiver can tell the two orders apart.
    for (auto* keys : selected)
    {
        if (keys == nullptr)
            continue;

        for (int note = 0; note < kNotes; ++note)
        {
            if (! needsNoteOff (keys[note]))
                continue;

            out[count++] = { (unsigned char) (0x80 | channel), (unsigned char) note, 0 };
            keys[note] = stillDown ? latched : silent;
        }
    }

    if (backstop)
        out[count++] = { (unsigned char) (0xb0 | channel), (unsigned char) kAllNotesOff, 0 };

    // The lift goes through the pedal's own rule rather than writing the byte,
    // because lifting it releases everything it was holding up, on BOTH rows,
    // including a row this release was not even about. The receiver does that
    // whether the model follows or not.
    if (liftPedal)
        setPedal (c, 0);

    // And the backstop reaches both rows too: a receiver cannot tell which of
    // its sounding notes the engine thinks belong to the arrangement.
    if (backstop)
    {
        allNotesOff (c.live, c.pedalDown());
        allNotesOff (c.clip, c.pedalDown());
    }

    return count;
}

} // namespace HoldPedal
