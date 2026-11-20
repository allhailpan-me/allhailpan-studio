// ---------------------------------------------------------------------------
// The hold pedal, controller 64.
//
// WHAT THIS IS FOR
//
// `Source/HoldPedal.h` exists because the engine's model of what is still
// sounding was wrong exactly when a pedal was down, and every release path
// reasons from that model. So the property this file asserts is not any
// formula in the header. It is:
//
//   what the engine believes still owes a note off is exactly what a
//   conformant receiver still has sounding
//
// and, following from it:
//
//   after the sequence the header says to send, a conformant receiver has
//   nothing sounding at all
//
// Both are measured against a receiver written out separately below, from the
// specification's own sentences rather than from the header. It keeps a
// different set of facts on purpose: whether each key is physically down and
// whether each voice is ringing, where the header keeps one state per key. A
// test that compares a thing against a reimplementation of itself proves
// nothing, which this project has already learned the hard way.
//
// THE ONE THAT MATTERS MOST
//
// `theReleaseNeverPressesThePedal`. The intuitive fix for a latched note off
// is to lift the pedal, send the offs, and put the pedal back down, and
// AMEI/MMA M2-113-UM section 6 says a receiver re-catches any note still
// above its envelope sustain level on a damper off to on transition: on a
// piano, whose sustain level is nought, that is every note just released. So
// the sequence would release nothing and restart every decay, and it would
// measure as working on any patch with a non-zero sustain level, which is
// most test tones. The check asserts over every reachable state that no
// message the header emits is a controller 64 above 63.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include "HoldPedal.h"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

static int failures = 0;

static void fail (const std::string& what)
{
    std::printf ("  FAIL %s\n", what.c_str());
    ++failures;
}

static void check (bool ok, const std::string& what)
{
    if (! ok)
        fail (what);
}

//===========================================================================
// A conformant receiver, written from the specification rather than from the
// header.
//
// "If Note Off messages are received while the hold pedal (controller 64
// (40H)) is 'on' ... they must be recognized but not acted upon until the
// release of the hold pedal." / "The same is true for the All Notes Off
// message."  MIDI 1.0 Detailed Specification, appendix, "The Relationship
// Between the Hold Pedal and 'All Notes Off'".
//
// Deliberately a different shape from the thing it is checking. It keeps two
// independent facts per note, where the header keeps one state, and it
// decides whether a voice is ringing from them rather than storing a state
// called `latched` at all. So an error in the header's transitions cannot be
// mirrored by an identical error here.
//===========================================================================
struct Receiver
{
    bool pedal = false;
    bool keyDown[HoldPedal::kNotes] {};   // the player is holding the key
    bool ringing[HoldPedal::kNotes] {};   // a voice is sounding

    void noteOn (int note)
    {
        keyDown[note] = true;
        ringing[note] = true;
    }

    void noteOff (int note)
    {
        keyDown[note] = false;

        // Recognised but not acted upon while the pedal is down.
        if (! pedal)
            ringing[note] = false;
    }

    void allNotesOff()
    {
        for (int note = 0; note < HoldPedal::kNotes; ++note)
            noteOff (note);
    }

    void allSoundOff()
    {
        // No pedal exemption: the envelopes are zeroed "as soon as possible".
        for (int note = 0; note < HoldPedal::kNotes; ++note)
        {
            keyDown[note] = false;
            ringing[note] = false;
        }
    }

    void setPedal (bool down)
    {
        pedal = down;

        if (down)
            return;

        // Lifting it lets through every release that was deferred. A key
        // still physically down is not one of them.
        for (int note = 0; note < HoldPedal::kNotes; ++note)
            if (! keyDown[note])
                ringing[note] = false;
    }

    /** One message in, as it arrived on the wire. The same malformed message
        rules as the header, written out separately: under two bytes is
        nothing, a note or controller message needs its third byte. */
    void receive (const unsigned char* data, int numBytes)
    {
        if (numBytes < 2)
            return;

        const int status = data[0] & 0xf0;
        const int first  = data[1] & 0x7f;

        if (status == 0x90 && numBytes >= 3)
        {
            if ((data[2] & 0x7f) != 0)
                noteOn (first);
            else
                noteOff (first);

            return;
        }

        if (status == 0x80 && numBytes >= 3)
        {
            noteOff (first);
            return;
        }

        if (status != 0xb0 || numBytes < 3)
            return;

        const int value = data[2] & 0x7f;

        if (first == 64)
            setPedal (value >= 64);
        else if (first == 121)
            setPedal (false);
        else if (first == 123)
            allNotesOff();
        else if (first == 120)
            allSoundOff();
    }

    int soundingCount() const
    {
        int n = 0;
        for (int note = 0; note < HoldPedal::kNotes; ++note)
            if (ringing[note])
                ++n;
        return n;
    }
};

//===========================================================================
// A channel's storage, laid out the way the engine lays it out: two rows of
// keys and one byte for the pedal.
//===========================================================================
struct Storage
{
    unsigned char live[HoldPedal::kNotes] {};
    unsigned char clip[HoldPedal::kNotes] {};
    unsigned char down = 0;

    HoldPedal::Channel channel (bool withClip = true)
    {
        HoldPedal::Channel c;
        c.live = live;
        c.clip = withClip ? clip : nullptr;
        c.down = &down;
        return c;
    }
};

//===========================================================================
/** A note off while the pedal is down is remembered and not acted on.

    The normative sentence, as a case, so that the file says plainly what it
    is about before it starts generating things.
*/
static void aPedalledNoteOffIsDeferred()
{
    Storage s;
    const auto c = s.channel();

    const unsigned char pedalDown[] = { 0xb0, 64, 127 };
    const unsigned char on[]        = { 0x90, 60, 100 };
    const unsigned char off[]       = { 0x80, 60, 0 };
    const unsigned char pedalUp[]   = { 0xb0, 64, 0 };

    HoldPedal::receive (c, pedalDown, 3);
    check (c.pedalDown(), "a pedal at 127 is not down");

    HoldPedal::receive (c, on, 3);
    check (s.live[60] == HoldPedal::sounding, "a struck key is not sounding");

    HoldPedal::receive (c, off, 3);
    check (s.live[60] == HoldPedal::latched,
           "a note off under the pedal was acted on instead of deferred");
    check (HoldPedal::needsNoteOff (s.live[60]),
           "a latched note was reported as owing no note off, which is the fault exactly");

    HoldPedal::receive (c, pedalUp, 3);
    check (s.live[60] == HoldPedal::silent, "lifting the pedal did not let the release through");

    // And a key still held when the pedal comes up keeps sounding, because
    // the pedal was never what was holding it.
    HoldPedal::receive (c, pedalDown, 3);
    HoldPedal::receive (c, on, 3);
    HoldPedal::receive (c, pedalUp, 3);
    check (s.live[60] == HoldPedal::sounding,
           "lifting the pedal silenced a key the player is still holding down");
}

/** All Notes Off is latched identically, and All Sound Off is not latched at
    all. If these two measure the same, one of them is wrong, and this pair is
    the only thing that separates them.
*/
static void theTwoChannelModeMessagesDifferUnderThePedal()
{
    for (int which = 0; which < 2; ++which)
    {
        const bool soundOff = which == 1;

        Storage s;
        const auto c = s.channel();

        const unsigned char pedalDown[] = { 0xb0, 64, 100 };
        HoldPedal::receive (c, pedalDown, 3);

        for (int note : { 48, 60, 67 })
        {
            const unsigned char on[] = { 0x90, (unsigned char) note, 90 };
            HoldPedal::receive (c, on, 3);
        }

        // The arrangement is holding something too, on this same channel,
        // which is how a loop wrap gets into this state.
        s.clip[72] = HoldPedal::sounding;

        const unsigned char mode[] = { 0xb0, (unsigned char) (soundOff ? 120 : 123), 0 };
        HoldPedal::receive (c, mode, 3);

        for (int note : { 48, 60, 67 })
            check (s.live[note] == (soundOff ? HoldPedal::silent : HoldPedal::latched),
                   soundOff ? "All Sound Off left a note sounding under the pedal"
                            : "All Notes Off was not latched by the pedal");

        check (s.clip[72] == (soundOff ? HoldPedal::silent : HoldPedal::latched),
               "a channel mode message from the wire did not reach the arrangement's notes");

        check (c.pedalDown(), "a channel mode message lifted the player's foot off the pedal");
    }
}

/** The threshold, over every value a data byte can carry.

    0 to 63 off and 64 to 127 on is what a receiver is required to do, and the
    boundary is where an off by one lives. Checked through `isDown` and again
    through a real message, because the two could disagree.
*/
static void theThresholdIsSixtyFour()
{
    for (int value = 0; value < 128; ++value)
    {
        const bool expected = value >= 64;

        check (HoldPedal::isDown (value) == expected,
               "the threshold is wrong at " + std::to_string (value));

        // And with the high bit set, which is what a caller handing over a
        // raw byte can produce.
        check (HoldPedal::isDown (value | 0x80) == expected,
               "the threshold is wrong for a byte with its high bit set at "
                 + std::to_string (value));

        Storage s;
        const auto c = s.channel();

        const unsigned char pedal[] = { 0xb0, 64, (unsigned char) value };
        const unsigned char on[]    = { 0x90, 60, 100 };
        const unsigned char off[]   = { 0x80, 60, 0 };

        HoldPedal::receive (c, pedal, 3);
        HoldPedal::receive (c, on, 3);
        HoldPedal::receive (c, off, 3);

        check (s.live[60] == (expected ? HoldPedal::latched : HoldPedal::silent),
               "a note off was " + std::string (expected ? "acted on" : "deferred")
                 + " at a pedal value of " + std::to_string (value));
    }
}

/** Reset All Controllers takes the pedal to nothing, whatever value byte it
    carries, which is the one thing about it that matters here. */
static void resetAllControllersLiftsThePedal()
{
    Storage s;
    const auto c = s.channel();

    const unsigned char pedalDown[] = { 0xb0, 64, 127 };
    const unsigned char on[]        = { 0x90, 55, 80 };
    const unsigned char off[]       = { 0x80, 55, 0 };
    const unsigned char reset[]     = { 0xb0, 121, 127 };   // a value it must ignore

    HoldPedal::receive (c, pedalDown, 3);
    HoldPedal::receive (c, on, 3);
    HoldPedal::receive (c, off, 3);
    check (s.live[55] == HoldPedal::latched, "the note was not latched to begin with");

    HoldPedal::receive (c, reset, 3);
    check (! c.pedalDown(), "Reset All Controllers left the pedal down");
    check (s.live[55] == HoldPedal::silent, "Reset All Controllers did not let the release through");
}

/** A malformed message changes nothing. A controller message with no value
    byte read as a value of nought would lift a pedal nobody lifted. */
static void aMalformedMessageIsIgnored()
{
    Storage s;
    const auto c = s.channel();

    const unsigned char pedalDown[] = { 0xb0, 64, 127 };
    const unsigned char on[]        = { 0x90, 60, 100 };
    HoldPedal::receive (c, pedalDown, 3);
    HoldPedal::receive (c, on, 3);

    const unsigned char shortPedal[] = { 0xb0, 64 };
    const unsigned char shortNote[]  = { 0x90, 60 };
    const unsigned char oneByte[]    = { 0xb0 };

    HoldPedal::receive (c, shortPedal, 2);
    check (c.pedalDown(), "a controller message with no value byte lifted the pedal");

    HoldPedal::receive (c, shortNote, 2);
    check (s.live[60] == HoldPedal::sounding, "a note message with no velocity changed a key");

    HoldPedal::receive (c, oneByte, 1);
    HoldPedal::receive (c, nullptr, 3);
    check (c.pedalDown() && s.live[60] == HoldPedal::sounding, "a runt message changed something");

    // A note on with a velocity of nought is a note off, which is how a lot
    // of hardware sends releases.
    const unsigned char zeroVelocity[] = { 0x90, 60, 0 };
    HoldPedal::receive (c, zeroVelocity, 3);
    check (s.live[60] == HoldPedal::latched,
           "a note on at velocity nought was not treated as a release");
}

/** A retrigger under the pedal takes back the release it was owed. */
static void aRetriggerUnderThePedalIsSoundingAgain()
{
    Storage s;
    const auto c = s.channel();

    const unsigned char pedalDown[] = { 0xb0, 64, 127 };
    const unsigned char on[]        = { 0x90, 64, 100 };
    const unsigned char off[]       = { 0x80, 64, 0 };
    const unsigned char pedalUp[]   = { 0xb0, 64, 0 };

    HoldPedal::receive (c, pedalDown, 3);
    HoldPedal::receive (c, on, 3);
    HoldPedal::receive (c, off, 3);
    HoldPedal::receive (c, on, 3);
    check (s.live[64] == HoldPedal::sounding, "a retriggered key is not sounding");

    HoldPedal::receive (c, pedalUp, 3);
    check (s.live[64] == HoldPedal::sounding,
           "lifting the pedal released a key that had been struck again under it");
}

//===========================================================================
/** The release sequence never presses the pedal, over every reachable state.

    This is the check that stands against the intuitive fix. See the head of
    this file for the specified reason a press is wrong.

    It also asserts the order, which is three specification sentences rather
    than a preference: the pedal first, because everything behind it is
    latched otherwise; a note off for every key, because recognising All Notes
    Off is not required of a receiver; All Notes Off last as the backstop.
*/
static void theReleaseNeverPressesThePedal()
{
    std::mt19937 rng (20261120u);
    std::uniform_int_distribution<int> aState (0, 2);
    std::uniform_int_distribution<int> aChannel (1, 16);
    std::bernoulli_distribution half (0.5);

    int pedalLifts = 0, noteOffs = 0, backstops = 0;

    for (int trial = 0; trial < 20000; ++trial)
    {
        Storage s;
        const bool withClip = half (rng);
        const auto c = s.channel (withClip);

        s.down = half (rng) ? 1 : 0;

        for (int note = 0; note < HoldPedal::kNotes; ++note)
        {
            s.live[note] = (unsigned char) aState (rng);
            s.clip[note] = (unsigned char) aState (rng);
        }

        const int midiChannel = aChannel (rng);
        const bool backstop   = half (rng);
        const bool liftAlways = half (rng);

        HoldPedal::Message out[HoldPedal::kMaxReleaseMessages];
        const int count = HoldPedal::releaseSequence (c, midiChannel, backstop, liftAlways,
                                                      out, HoldPedal::kMaxReleaseMessages);

        if (count < 0)
        {
            fail ("the release was refused with room for everything it can emit");
            continue;
        }

        int firstNoteOff = -1, lastNoteOff = -1, pedalAt = -1, backstopAt = -1;

        for (int i = 0; i < count; ++i)
        {
            const auto& m = out[i];

            check ((m.status & 0x0f) == (midiChannel - 1),
                   "a released message went out on the wrong MIDI channel");

            if ((m.status & 0xf0) == 0xb0 && m.data1 == HoldPedal::kController)
            {
                // THE WHOLE POINT. A pedal value above 63 is a press, and a
                // press re-catches every note whose level is above its
                // envelope sustain level: on a piano, all of them.
                check (m.data2 <= 63,
                       "the release pressed the pedal, which re-catches the notes it just let go");
                pedalAt = i;
                ++pedalLifts;
            }
            else if ((m.status & 0xf0) == 0x80)
            {
                if (firstNoteOff < 0)
                    firstNoteOff = i;
                lastNoteOff = i;
                ++noteOffs;
            }
            else if ((m.status & 0xf0) == 0xb0 && m.data1 == HoldPedal::kAllNotesOff)
            {
                backstopAt = i;
                ++backstops;
            }
            else
            {
                fail ("the release emitted something that is not a lift, a note off or the backstop");
            }
        }

        if (pedalAt >= 0)
            check (pedalAt == 0, "the pedal lift did not go out first, so everything behind it is latched");

        if (backstopAt >= 0)
        {
            check (backstopAt == count - 1, "the backstop did not go out last");

            if (lastNoteOff >= 0)
                check (lastNoteOff < backstopAt,
                       "All Notes Off went out ahead of an individual note off");
        }

        check ((backstopAt >= 0) == backstop, "the backstop was not sent as asked");
        check (pedalAt >= 0 || ! liftAlways,
               "an unconditional lift was asked for and not sent");

        // And the model is left with nothing owing a note off, because the
        // offs have just gone out.
        for (int note = 0; note < HoldPedal::kNotes; ++note)
        {
            check (s.live[note] == HoldPedal::silent,
                   "a key still owed a note off after the release");

            if (withClip)
                check (s.clip[note] == HoldPedal::silent,
                       "an arrangement key still owed a note off after the release");
        }

        if (pedalAt >= 0)
            check (! c.pedalDown(), "the model still thinks the pedal is down after lifting it");
    }

    check (pedalLifts > 1000 && noteOffs > 100000 && backstops > 1000,
           "the generator did not reach all three kinds of message: "
             + std::to_string (pedalLifts) + " lifts, " + std::to_string (noteOffs)
             + " note offs, " + std::to_string (backstops) + " backstops");

    std::printf ("       %d lifts, %d note offs and %d backstops emitted over 20000 states\n",
                 pedalLifts, noteOffs, backstops);
}

/** A release that will not fit is refused rather than truncated.

    A truncated release is a silently dropped note off, which is the fault the
    whole header is about. So it has to change nothing at all, not even the
    messages that would have fitted.
*/
static void aReleaseThatWillNotFitIsRefused()
{
    Storage s;
    const auto c = s.channel();

    s.down = 1;
    for (int note : { 40, 50, 60, 70 })
    {
        s.live[note] = HoldPedal::sounding;
        s.clip[note] = HoldPedal::latched;
    }

    const int needed = 1 + 8 + 1;   // the lift, eight note offs, the backstop

    for (int capacity = 0; capacity < needed; ++capacity)
    {
        std::vector<HoldPedal::Message> out ((size_t) needed, HoldPedal::Message {});
        const int count = HoldPedal::releaseSequence (c, 1, true, false, out.data(), capacity);

        check (count == -1, "a release with room for only " + std::to_string (capacity)
                              + " messages was not refused");

        for (int note : { 40, 50, 60, 70 })
            check (s.live[note] == HoldPedal::sounding && s.clip[note] == HoldPedal::latched,
                   "a refused release changed the model anyway");

        check (c.pedalDown(), "a refused release lifted the pedal anyway");
    }

    std::vector<HoldPedal::Message> out ((size_t) needed, HoldPedal::Message {});
    check (HoldPedal::releaseSequence (c, 1, true, false, out.data(), needed) == needed,
           "a release with exactly enough room was refused");
}

//===========================================================================
/** The property the header exists to produce, over randomised streams.

    Two halves, asserted after every single message:

      what the engine believes still owes a note off is exactly what a
      conformant receiver still has sounding

    and, at the end of each stream:

      after the sequence the header says to send, the receiver has nothing
      sounding at all

    The second is the one that would have caught the fault this header fixes.
    Fed the old behaviour, where a note off cleared the row whatever the pedal
    was doing, the engine emits no note off for a pedalled note and the
    receiver is left ringing.
*/
static void theModelAgreesWithAConformantReceiver()
{
    std::mt19937 rng (0x5eed40u);
    std::uniform_int_distribution<int> aNote (0, 127);
    std::uniform_int_distribution<int> aKind (0, 11);
    std::uniform_int_distribution<int> aValue (0, 127);
    std::uniform_int_distribution<int> aByte (0, 255);
    std::uniform_int_distribution<int> aLength (1, 3);
    std::uniform_int_distribution<int> aStream (1, 60);
    std::bernoulli_distribution half (0.5);

    int ringingAtRelease = 0, latchedSeen = 0, deferredAno = 0;

    for (int trial = 0; trial < 20000; ++trial)
    {
        Storage s;
        Receiver reference;

        // No clip row for some of the trials, which is every MIDI channel
        // other than 1. A null row must be left alone rather than walked.
        const bool withClip = half (rng);
        const auto c = s.channel (withClip);

        const int messages = aStream (rng);

        for (int m = 0; m < messages; ++m)
        {
            unsigned char data[3] { 0, 0, 0 };
            int numBytes = 3;

            switch (aKind (rng))
            {
                case 0: case 1: case 2:                       // a key struck
                    data[0] = 0x90; data[1] = (unsigned char) aNote (rng);
                    data[2] = (unsigned char) (1 + aValue (rng) % 127);
                    break;

                case 3: case 4: case 5:                       // a key released
                    data[0] = (unsigned char) (half (rng) ? 0x80 : 0x90);
                    data[1] = (unsigned char) aNote (rng);
                    data[2] = 0;
                    break;

                case 6: case 7:                               // the pedal moves
                    data[0] = 0xb0; data[1] = 64;
                    data[2] = (unsigned char) (half (rng) ? aValue (rng)
                                                          : (half (rng) ? 127 : 0));
                    break;

                case 8:                                       // All Notes Off
                    data[0] = 0xb0; data[1] = 123; data[2] = 0;
                    break;

                case 9:                                       // All Sound Off
                    data[0] = 0xb0; data[1] = 120; data[2] = 0;
                    break;

                case 10:                                      // Reset All Controllers
                    data[0] = 0xb0; data[1] = 121;
                    data[2] = (unsigned char) aValue (rng);
                    break;

                default:                                      // junk and runts
                    data[0] = (unsigned char) aByte (rng);
                    data[1] = (unsigned char) aByte (rng);
                    data[2] = (unsigned char) aByte (rng);
                    numBytes = aLength (rng);
                    break;
            }

            const bool wasAno = numBytes >= 3 && (data[0] & 0xf0) == 0xb0 && data[1] == 123
                                && reference.pedal && reference.soundingCount() > 0;

            HoldPedal::receive (c, data, numBytes);
            reference.receive (data, numBytes);

            if (wasAno)
                ++deferredAno;

            check (c.pedalDown() == reference.pedal,
                   "the model and the receiver disagree about the pedal");

            for (int note = 0; note < HoldPedal::kNotes; ++note)
            {
                if (HoldPedal::needsNoteOff (s.live[note]) != reference.ringing[note])
                {
                    fail ("the model and the receiver disagree about note "
                            + std::to_string (note) + ": the model says "
                            + (HoldPedal::needsNoteOff (s.live[note]) ? "sounding" : "silent")
                            + " and the receiver says "
                            + (reference.ringing[note] ? "sounding" : "silent"));
                    break;
                }

                if (s.live[note] == HoldPedal::latched)
                    ++latchedSeen;
            }
        }

        if (reference.soundingCount() > 0)
            ++ringingAtRelease;

        // And now the thing a release has to achieve. The sequence goes into
        // the receiver exactly as the engine would send it.
        HoldPedal::Message out[HoldPedal::kMaxReleaseMessages];
        const int count = HoldPedal::releaseSequence (c, 1, true, false,
                                                      out, HoldPedal::kMaxReleaseMessages);

        check (count >= 0, "the release was refused with room for everything");

        for (int i = 0; i < count; ++i)
        {
            const unsigned char bytes[3] { out[i].status, out[i].data1, out[i].data2 };
            reference.receive (bytes, 3);
        }

        if (reference.soundingCount() != 0)
            fail ("the release left " + std::to_string (reference.soundingCount())
                    + " notes sounding in a conformant receiver");
    }

    // The generator has to reach the states the properties are about, or the
    // assertions above are decoration. A pedal that is never down and a
    // release with nothing to release would both pass everything.
    check (ringingAtRelease > 10000,
           "too few streams had anything left sounding when the release came: "
             + std::to_string (ringingAtRelease));
    check (latchedSeen > 10000,
           "the generator barely ever latched a note, which is the state this header exists for: "
             + std::to_string (latchedSeen));
    check (deferredAno > 500,
           "All Notes Off was almost never sent into a pedal that was holding something: "
             + std::to_string (deferredAno));

    std::printf ("       %d of 20000 streams were still sounding at the release, "
                 "%d latched keys seen, %d pedalled All Notes Off\n",
                 ringingAtRelease, latchedSeen, deferredAno);
}

/** A channel with no arrangement row, which is every MIDI channel but the
    first, must be left alone rather than walked. Driven on its own as well as
    inside the randomised streams, because an out of bounds read through a
    null row is the kind of thing the sanitizers catch and a release build
    believes.
*/
static void aChannelWithNoArrangementRowIsLeftAlone()
{
    Storage s;
    const auto c = s.channel (false);

    s.clip[60] = HoldPedal::sounding;      // must not be touched through this channel

    const unsigned char pedalDown[] = { 0xb0, 64, 127 };
    const unsigned char on[]        = { 0x90, 60, 100 };
    const unsigned char off[]       = { 0x80, 60, 0 };
    const unsigned char ano[]       = { 0xb0, 123, 0 };
    const unsigned char aso[]       = { 0xb0, 120, 0 };

    HoldPedal::receive (c, pedalDown, 3);
    HoldPedal::receive (c, on, 3);
    HoldPedal::receive (c, off, 3);
    HoldPedal::receive (c, ano, 3);
    HoldPedal::receive (c, aso, 3);

    check (s.clip[60] == HoldPedal::sounding,
           "a channel with no arrangement row reached one anyway");

    HoldPedal::Message out[HoldPedal::kMaxReleaseMessages];
    const int count = HoldPedal::releaseSequence (c, 3, true, false,
                                                  out, HoldPedal::kMaxReleaseMessages);
    check (count >= 0, "the release was refused");
    check (s.clip[60] == HoldPedal::sounding, "the release reached a row it was not given");
}

/** An invalid channel does nothing rather than reaching through a null
    pointer. The engine resolves these from indices that arrive from atomics
    and from saved projects, which is the same reason `LatencyGraph` clamps
    its own. */
static void anInvalidChannelIsRefused()
{
    HoldPedal::Channel empty;
    const unsigned char on[] = { 0x90, 60, 100 };

    HoldPedal::receive (empty, on, 3);
    check (! empty.pedalDown(), "an empty channel reported a pedal");
    check (! HoldPedal::setPedal (empty, 127), "an empty channel took a pedal press");

    HoldPedal::Message out[HoldPedal::kMaxReleaseMessages];
    check (HoldPedal::releaseSequence (empty, 1, true, true, out, HoldPedal::kMaxReleaseMessages) == 0,
           "an empty channel emitted messages");

    unsigned char keys[HoldPedal::kNotes] {};
    unsigned char down = 0;
    HoldPedal::Channel noPedal;
    noPedal.live = keys;
    check (! noPedal.valid(), "a channel with no pedal byte reported itself usable");

    HoldPedal::Channel noKeys;
    noKeys.down = &down;
    check (! noKeys.valid(), "a channel with no keys reported itself usable");

    // And the row helpers on their own, which the engine calls directly where
    // it schedules the arrangement's notes.
    HoldPedal::noteOn (nullptr, 60);
    HoldPedal::noteOff (nullptr, 60, true);
    HoldPedal::allNotesOff (nullptr, true);
    HoldPedal::allSoundOff (nullptr);

    HoldPedal::noteOn (keys, -1);
    HoldPedal::noteOn (keys, 128);
    HoldPedal::noteOff (keys, -1, true);
    HoldPedal::noteOff (keys, 9999, false);

    for (int note = 0; note < HoldPedal::kNotes; ++note)
        check (keys[note] == HoldPedal::silent, "a note number off the end reached a key");
}

//===========================================================================
int main()
{
    std::printf ("HoldPedal\n");

    aPedalledNoteOffIsDeferred();
    theTwoChannelModeMessagesDifferUnderThePedal();
    theThresholdIsSixtyFour();
    resetAllControllersLiftsThePedal();
    aMalformedMessageIsIgnored();
    aRetriggerUnderThePedalIsSoundingAgain();
    aChannelWithNoArrangementRowIsLeftAlone();
    anInvalidChannelIsRefused();
    theReleaseNeverPressesThePedal();
    aReleaseThatWillNotFitIsRefused();
    theModelAgreesWithAConformantReceiver();

    if (failures == 0)
        std::printf ("  ok\n");
    else
        std::printf ("  %d failure(s)\n", failures);

    return failures == 0 ? 0 : 1;
}
