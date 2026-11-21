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
// THE ROW THE RECEIVER COMPARISON CANNOT SEE
//
// The agreement check above drives the player's row, because a note from the
// wire only ever reaches the player's row. That left the arrangement's row
// compared against nothing, and a mutation run found two faults hiding there:
// `setPedal` not releasing the arrangement's latched notes, and a wire note
// written into the arrangement's row as well as the player's. Neither shows up
// in a receiver, because both rows reach the same voices, so both have their
// own checks: `theArrangementRowObeysTheSamePedal` drives the other row the
// way the engine drives it, and `aNoteFromTheWireNeverTouchesTheArrangement`
// asserts the separation directly.
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
    check (HoldPedal::isSounding (s.live[60]),
           "a latched note was reported as silent, which is the fault exactly");
    check (! HoldPedal::needsNoteOff (s.live[60], false),
           "a latched note was said to need another note off from a release that is leaving "
           "the pedal down: its off is already with the receiver, waiting on that same pedal");
    check (HoldPedal::needsNoteOff (s.live[60], true),
           "a latched note was said to need no note off from a release that is lifting the "
           "pedal, which is the one the specification asks for");

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

/** A channel mode message arriving from the wire leaves the model alone.

    This is the one place the model deliberately declines to follow a
    conformant receiver, so it is asserted directly rather than left to the
    randomised streams, which hold it only as an upper bound.

    All Notes Off is latched by the pedal exactly as an individual note off is,
    and All Sound Off is not latched at all. Both are true of a receiver and
    neither is something the engine may rely on, because "there is no
    equirement that a receiver recognize" All Notes Off (the typo is the
    specification's) and a plugin that ignores it leaves the notes sounding.
    A model that cleared itself on the strength of the message would then have
    forgotten notes that nothing afterwards sends an off for, which is the
    whole failure this header exists to end. Keeping them costs a note off
    nobody needed.

    An earlier version of this change did clear both rows here, and that was a
    regression against what the studio did before the pedal was modelled at
    all: it used to clear only the player's keys on that one MIDI channel, and
    clearing the arrangement's as well put the arrangement's notes beyond the
    reach of the next stop.
*/
static void aChannelModeMessageFromTheWireIsNotActedOn()
{
    for (const int controller : { 120, 123 })
    {
        Storage s;
        const auto c = s.channel();

        const unsigned char pedalDown[] = { 0xb0, 64, 100 };
        HoldPedal::receive (c, pedalDown, 3);

        for (int note : { 48, 60, 67 })
        {
            const unsigned char on[] = { 0x90, (unsigned char) note, 90 };
            HoldPedal::receive (c, on, 3);
        }

        const unsigned char off[] = { 0x80, 48, 0 };
        HoldPedal::receive (c, off, 3);     // one of them latched

        // The arrangement is holding something too, on this same channel,
        // which is how a loop wrap gets into this state.
        s.clip[72] = HoldPedal::sounding;

        const unsigned char mode[] = { 0xb0, (unsigned char) controller, 0 };
        HoldPedal::receive (c, mode, 3);

        check (s.live[48] == HoldPedal::latched, "a channel mode message moved a latched key");
        check (s.live[60] == HoldPedal::sounding && s.live[67] == HoldPedal::sounding,
               "a channel mode message forgot a key the engine may still have to release");
        check (s.clip[72] == HoldPedal::sounding,
               "a channel mode message from the wire reached the arrangement's keys");
        check (c.pedalDown(), "a channel mode message lifted the player's foot off the pedal");

        // And what the engine still owes for them is unchanged, which is the
        // point: the next release sends their offs.
        check (HoldPedal::needsNoteOff (s.live[60], false)
                 && HoldPedal::needsNoteOff (s.clip[72], false),
               "a channel mode message left the engine owing no release");
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

        // And what `setPedal` reports, which is the only part of it a caller
        // can read without looking at the rows. Nothing in the engine uses
        // the return value yet, so inverting it changed no state at all and
        // survived the first mutation run: an unread contract is where the
        // next caller's bug goes.
        Storage r;
        check (HoldPedal::setPedal (r.channel(), value) == expected,
               "setPedal reported the wrong position at a value of "
                 + std::to_string (value));
        check (r.channel().pedalDown() == expected,
               "setPedal wrote the wrong pedal byte at a value of "
                 + std::to_string (value));

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

    int pedalLifts = 0, noteOffs = 0, backstops = 0, liftsWithheld = 0;
    int becameLatched = 0, latchedLeftAlone = 0;

    for (int trial = 0; trial < 20000; ++trial)
    {
        Storage s;
        const bool withClip = half (rng);
        const auto c = s.channel (withClip);

        s.down = half (rng) ? 1 : 0;
        const bool pedalWasDown = s.down != 0;

        for (int note = 0; note < HoldPedal::kNotes; ++note)
        {
            s.live[note] = (unsigned char) aState (rng);
            s.clip[note] = (unsigned char) aState (rng);
        }

        unsigned char beforeLive[HoldPedal::kNotes], beforeClip[HoldPedal::kNotes];
        for (int note = 0; note < HoldPedal::kNotes; ++note)
        {
            beforeLive[note] = s.live[note];
            beforeClip[note] = s.clip[note];
        }

        const int midiChannel = aChannel (rng);
        const bool backstop   = half (rng);
        const bool liftPedal  = half (rng);
        const HoldPedal::Rows rows { half (rng), half (rng) };

        // Where the pedal is left is what the keys this releases become, so it
        // is worked out here too rather than read back off the thing under
        // test.
        const bool stillDown = pedalWasDown && ! liftPedal;

        // Which keys owed an off, worked out before the call. A latched key
        // owes nothing: its off is already with the receiver.
        int owed[HoldPedal::kNotes] {};
        for (int note = 0; note < HoldPedal::kNotes; ++note)
        {
            if (rows.live && HoldPedal::needsNoteOff (s.live[note], liftPedal))
                ++owed[note];

            if (withClip && rows.clip && HoldPedal::needsNoteOff (s.clip[note], liftPedal))
                ++owed[note];
        }

        HoldPedal::Message out[HoldPedal::kMaxReleaseMessages];
        const int count = HoldPedal::releaseSequence (c, midiChannel, rows, liftPedal, backstop,
                                                      out, HoldPedal::kMaxReleaseMessages);

        if (count < 0)
        {
            fail ("the release was refused with room for everything it can emit");
            continue;
        }

        int firstNoteOff = -1, lastNoteOff = -1, pedalAt = -1, backstopAt = -1;
        int sent[HoldPedal::kNotes] {};

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
                ++sent[m.data1 & 0x7f];
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

        // Both directions, because only one of them was asserted to begin
        // with and `lift = true` therefore survived the first mutation run.
        // A lift nobody asked for is a controller change the player did not
        // make. At a loop wrap it takes the player's foot off the pedal for
        // the rest of the pass, which is the behaviour this studio decided
        // against and Ardour decided for, and it damps every note the pedal
        // was holding up, including the ones this release was never about:
        // lifting it releases both rows, not just the selected one.
        check ((pedalAt >= 0) == liftPedal,
               liftPedal ? "a lift was wanted and not sent"
                         : "a lift was sent that nobody asked for");

        if (! liftPedal)
            ++liftsWithheld;

        // What each key must now be, worked out from the contract rather than
        // from the answer. A row this release was about has had its offs sent,
        // so its keys are silent, or LATCHED if the pedal is still down: the
        // receiver defers those offs, and recording them as gone is the exact
        // lie this header was written to end. A row it was not about is
        // untouched, except that lifting the pedal releases whatever it was
        // holding up there, because the receiver does that whether the model
        // follows or not. And the backstop reaches both rows.
        for (int note = 0; note < HoldPedal::kNotes; ++note)
        {
            for (int row = 0; row < 2; ++row)
            {
                const bool isClip = row == 1;

                if (isClip && ! withClip)
                    continue;

                const bool selected = isClip ? rows.clip : rows.live;
                const unsigned char was = isClip ? beforeClip[note] : beforeLive[note];
                const unsigned char now = isClip ? s.clip[note] : s.live[note];

                unsigned char expected = was;

                // A key that owed an off has had it sent, so it is silent, or
                // LATCHED if the pedal is still down: the receiver defers
                // that off and the note is still sounding. Recording it as
                // gone is the lie this header exists to end.
                //
                // A key that was already latched gets no message at all, and
                // only the pedal can move it. Sending it another off is what
                // put one out for every finished note at every loop wrap.
                if (selected && HoldPedal::needsNoteOff (was, liftPedal))
                    expected = stillDown ? HoldPedal::latched : HoldPedal::silent;
                else if (liftPedal && was == HoldPedal::latched)
                    expected = HoldPedal::silent;

                if (now != expected)
                {
                    fail (std::string (isClip ? "an arrangement key" : "a key")
                            + " is " + std::to_string ((int) now) + " where it should be "
                            + std::to_string ((int) expected)
                            + (selected ? " after being released" : " without being released")
                            + (stillDown ? " under a pedal that is still down" : ""));
                    break;
                }

                // THE COUNTER HAS TO BE ABOUT WHAT THE CALL DID, not about
                // what is lying around afterwards. The first version counted
                // every key that was latched at the end, which includes the
                // ones the randomised initialiser made latched and the
                // release never touched: an audit set the pedal up so that
                // nothing could possibly be latched BY the release, and the
                // count still read 160537 against a threshold of 100000. With
                // the counter blind, putting back the old lie,
                // `keys[note] = silent` for a key released under a held
                // pedal, survived the whole file.
                if (expected == HoldPedal::latched && was != HoldPedal::latched)
                    ++becameLatched;

                if (was == HoldPedal::latched && expected == HoldPedal::latched)
                    ++latchedLeftAlone;
            }
        }

        check (c.pedalDown() == stillDown,
               "the model disagrees with where the release left the pedal");

        // And exactly the keys that owed an off got one. Per key rather than
        // in total: every off sent one key too high passed a check on the
        // total, and that is the worst thing this code can do, because at a
        // stop it hangs every tracked note and cuts a neighbour instead.
        for (int note = 0; note < HoldPedal::kNotes; ++note)
            if (sent[note] != owed[note])
            {
                fail ("note " + std::to_string (note) + " was sent " + std::to_string (sent[note])
                        + " note offs where " + std::to_string (owed[note]) + " were owed");
                break;
            }
    }

    check (pedalLifts > 1000 && noteOffs > 100000 && backstops > 1000,
           "the generator did not reach all three kinds of message: "
             + std::to_string (pedalLifts) + " lifts, " + std::to_string (noteOffs)
             + " note offs, " + std::to_string (backstops) + " backstops");

    // The half of the lift assertion that fails on an unwanted lift is only
    // reached where neither the caller nor the model wants one, so the count
    // of those is what says it was reached at all.
    check (liftsWithheld > 1000,
           "the generator almost never asked for a release with the pedal already up, "
           "which is the only state an unwanted lift shows up in: "
             + std::to_string (liftsWithheld));

    // And the two states the whole header exists for. A key the release
    // LATCHED, which is a release that left the pedal down, so the off it
    // just sent is deferred rather than acted on: that is the loop wrap. And
    // a key that arrived latched and was left alone, which is the one that
    // must get no second off.
    check (becameLatched > 50000,
           "the release never latched a key, so the one state this header exists to represent "
           "went unchecked: " + std::to_string (becameLatched));
    check (latchedLeftAlone > 50000,
           "the release never left an already latched key alone, so nothing checked that it "
           "does not send a second off for one: " + std::to_string (latchedLeftAlone));

    std::printf ("       %d lifts, %d note offs and %d backstops emitted over 20000 states, "
                 "%d releases wanted no lift\n",
                 pedalLifts, noteOffs, backstops, liftsWithheld);
    std::printf ("       %d keys latched BY the release, %d already latched and left alone\n",
                 becameLatched, latchedLeftAlone);
}

/** The loop wrap, as the engine asks for it, and the thing it must not do
    twice.

    A wrap releases the arrangement's notes and nothing else, and does not
    lift the pedal, because a wrap is a continuation and taking the player's
    foot off for the rest of the pass is a worse answer than the ringing note
    it would fix. That is this studio's own position rather than the field's,
    which disagrees: Ardour lifts it at every wrap by default and Logic
    chases instead.

    So a clip note released while the pedal is down is latched, and the wrap
    must leave it alone. An earlier version of this change re-sent its note
    off at every wrap for as long as the foot stayed down, which an audit
    measured at five redundant offs per wrap on a five note pattern. Harmless
    on a conformant receiver, which defers them again, and it is also how the
    128 entry echo filter that keeps a recorded take from doubling its own
    notes gets filled with messages nobody needed.
*/
static void aWrapDoesNotSendTheSameNoteOffTwice()
{
    Storage s;
    const auto c = s.channel();

    const unsigned char pedalDown[] = { 0xb0, 64, 127 };
    HoldPedal::receive (c, pedalDown, 3);

    // A pattern whose notes all start and finish inside the pass, which is
    // the ordinary case: the engine schedules each one and records it.
    const int pattern[] = { 36, 38, 42, 46, 60 };

    for (const int note : pattern)
    {
        HoldPedal::noteOn (s.clip, note);
        HoldPedal::noteOff (s.clip, note, c.pedalDown());
        check (s.clip[note] == HoldPedal::latched,
               "a clip note released under the pedal was not latched");
    }

    // And one that is still held as the loop comes round, which is the note
    // the wrap exists for: its off beat lies past the loop end, so it would
    // never reach its own release.
    HoldPedal::noteOn (s.clip, 67);

    HoldPedal::Message out[HoldPedal::kMaxReleaseMessages];
    const HoldPedal::Rows arrangementOnly { false, true };

    for (int wrap = 0; wrap < 3; ++wrap)
    {
        const int count = HoldPedal::releaseSequence (c, 1, arrangementOnly, false, false,
                                                      out, HoldPedal::kMaxReleaseMessages);

        if (wrap == 0)
        {
            check (count == 1, "the first wrap should release the one note still held, and only it");
            check (count >= 1 && (out[0].status & 0xf0) == 0x80 && out[0].data1 == 67,
                   "the first wrap released the wrong note");
        }
        else
        {
            check (count == 0, "a later wrap sent a message for a note it had already released");
        }

        check (c.pedalDown(), "the wrap lifted the player's foot off the pedal");

        for (const int note : pattern)
            check (s.clip[note] == HoldPedal::latched,
                   "the wrap moved a key whose off is already with the receiver");

        check (s.clip[67] == HoldPedal::latched,
               "the note the wrap released is not recorded as latched, so nothing will release it "
               "when the foot comes up");
    }

    // The foot comes up, and everything the pedal was holding goes together.
    const unsigned char pedalUp[] = { 0xb0, 64, 0 };
    HoldPedal::receive (c, pedalUp, 3);

    for (const int note : pattern)
        check (s.clip[note] == HoldPedal::silent, "lifting the foot did not release the pattern");

    check (s.clip[67] == HoldPedal::silent, "lifting the foot did not release the held note");
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
        s.clip[note] = HoldPedal::sounding;
    }

    // Both rows sounding rather than one of them latched, because a latched
    // key owes no note off and so does not count towards the room needed.
    const int needed = 1 + 8 + 1;   // the lift, eight note offs, the backstop

    for (int capacity = 0; capacity < needed; ++capacity)
    {
        std::vector<HoldPedal::Message> out ((size_t) needed, HoldPedal::Message {});
        const int count = HoldPedal::releaseSequence (c, 1, HoldPedal::Rows {}, true, true,
                                                      out.data(), capacity);

        check (count == -1, "a release with room for only " + std::to_string (capacity)
                              + " messages was not refused");

        for (int note : { 40, 50, 60, 70 })
            check (s.live[note] == HoldPedal::sounding && s.clip[note] == HoldPedal::sounding,
                   "a refused release changed the model anyway");

        check (c.pedalDown(), "a refused release lifted the pedal anyway");
    }

    std::vector<HoldPedal::Message> out ((size_t) needed, HoldPedal::Message {});
    check (HoldPedal::releaseSequence (c, 1, HoldPedal::Rows {}, true, true, out.data(), needed) == needed,
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
    int exactStreams = 0, upperBoundOnly = 0, offsMatched = 0;

    for (int trial = 0; trial < 20000; ++trial)
    {
        Storage s;
        Receiver reference;

        // No clip row for some of the trials, which is every MIDI channel
        // other than 1. A null row must be left alone rather than walked.
        const bool withClip = half (rng);
        const auto c = s.channel (withClip);
        bool modeMessageSeen = false;

        // A third of the streams carry no channel mode message at all, so
        // that the half of this property which asserts EQUALITY is carried by
        // a decent share of the run rather than by whatever is left over.
        // Drawn freely, a stream of up to sixty messages almost always
        // contains one, and the exact comparison was down to 1652 of 20000
        // against a threshold of 1000.
        const bool modeFree = trial % 3 == 0;

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
                    data[0] = 0xb0; data[1] = (unsigned char) (modeFree ? 7 : 123); data[2] = 0;
                    break;

                case 9:                                       // All Sound Off
                    data[0] = 0xb0; data[1] = (unsigned char) (modeFree ? 10 : 120); data[2] = 0;
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

                    // Junk reaches a receiver masked, so a byte of 0xf8 is
                    // controller 120. In a mode free stream it has to be
                    // moved, or the stream is not mode free and the equality
                    // comparison is applied to one that is not.
                    if (modeFree && (data[1] & 0x7f) >= 120 && (data[1] & 0x7f) <= 127)
                        data[1] = 7;
                    break;
            }

            const bool wasAno = numBytes >= 3 && (data[0] & 0xf0) == 0xb0 && data[1] == 123
                                && reference.pedal && reference.soundingCount() > 0;

            HoldPedal::receive (c, data, numBytes);
            reference.receive (data, numBytes);

            if (wasAno)
                ++deferredAno;

            // A channel mode message is the one thing the model declines to
            // follow, because recognising All Notes Off is not required of a
            // receiver and forgetting is the costly guess. From the first one
            // onward the model is an upper bound rather than a match, and the
            // assertion says which of the two it is checking rather than
            // quietly weakening for the whole run.
            // Masked, as a receiver masks it. Unmasked, a junk byte of 0xf8
            // reaches the receiver as controller 120 and did not register
            // here, so the exact comparison was applied to a stream that had
            // had a channel mode message in it after all.
            if (numBytes >= 3 && (data[0] & 0xf0) == 0xb0
                              && ((data[1] & 0x7f) == 120 || (data[1] & 0x7f) == 123))
                modeMessageSeen = true;

            check (c.pedalDown() == reference.pedal,
                   "the model and the receiver disagree about the pedal");

            for (int note = 0; note < HoldPedal::kNotes; ++note)
            {
                const bool model = HoldPedal::isSounding (s.live[note]);
                const bool real  = reference.ringing[note];

                if (modeMessageSeen ? (real && ! model) : (model != real))
                {
                    fail (std::string (modeMessageSeen
                                         ? "the model has forgotten a note the receiver is still "
                                           "sounding, which it may never do: note "
                                         : "the model and the receiver disagree about note ")
                            + std::to_string (note) + ": the model says "
                            + (model ? "sounding" : "silent")
                            + " and the receiver says "
                            + (real ? "sounding" : "silent"));
                    break;
                }

                if (s.live[note] == HoldPedal::latched)
                    ++latchedSeen;
            }
        }

        if (modeMessageSeen)
            ++upperBoundOnly;
        else
            ++exactStreams;

        if (reference.soundingCount() > 0)
            ++ringingAtRelease;

        // And now the thing a release has to achieve, in THE CONFIGURATION THE
        // ENGINE ACTUALLY USES: both rows, the pedal lifted, and no backstop.
        //
        // The backstop was the mistake in the first version of this check. All
        // Notes Off at the end of the sequence silences a conformant receiver
        // by itself, so "nothing is left sounding" held however wrong the
        // individual note offs were: an audit broke every one of them, sending
        // each off one key too high, and this check passed. Asked for without
        // the backstop, the lift and the individual offs are the only things
        // that can do it, and that is also the configuration a stop sends.
        unsigned char owedLive[HoldPedal::kNotes], owedClip[HoldPedal::kNotes];
        for (int note = 0; note < HoldPedal::kNotes; ++note)
        {
            // `true`, because this release lifts the pedal, and then
            // everything sounding gets an off: a latched key too, where a
            // conformant receiver already has its release and a badly
            // implemented one may have dropped it.
            owedLive[note] = (unsigned char) (HoldPedal::needsNoteOff (s.live[note], true) ? 1 : 0);
            owedClip[note] = (unsigned char)
                (withClip && HoldPedal::needsNoteOff (s.clip[note], true) ? 1 : 0);
        }

        HoldPedal::Message out[HoldPedal::kMaxReleaseMessages];
        const int count = HoldPedal::releaseSequence (c, 1, HoldPedal::Rows {}, true, false,
                                                      out, HoldPedal::kMaxReleaseMessages);

        check (count >= 0, "the release was refused with room for everything");

        // Exactly the keys that owed an off got one, and nothing else did.
        // Counted per key rather than in total, because a total cannot tell
        // the right number of wrong notes from the right ones: every off sent
        // one key too high passed a check on the total.
        int sent[HoldPedal::kNotes] {};

        for (int i = 0; i < count; ++i)
        {
            const unsigned char bytes[3] { out[i].status, out[i].data1, out[i].data2 };
            reference.receive (bytes, 3);

            if ((out[i].status & 0xf0) == 0x80)
                ++sent[out[i].data1 & 0x7f];
        }

        for (int note = 0; note < HoldPedal::kNotes; ++note)
        {
            const int wanted = (int) owedLive[note] + (int) owedClip[note];

            if (sent[note] != wanted)
            {
                fail ("note " + std::to_string (note) + " was sent " + std::to_string (sent[note])
                        + " note offs where " + std::to_string (wanted) + " were owed");
                break;
            }

            offsMatched += wanted;
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
    // Lower than it was, and deliberately. A pedalled All Notes Off used to
    // latch every sounding key in the model at once, which inflated this; the
    // model no longer acts on one at all, so a latched key now only ever comes
    // from an individual note off under the pedal.
    check (latchedSeen > 2000,
           "the generator barely ever latched a note, which is the state this header exists for: "
             + std::to_string (latchedSeen));
    check (exactStreams > 5000 && upperBoundOnly > 5000,
           "the generator did not produce both kinds of stream: " + std::to_string (exactStreams)
             + " exact and " + std::to_string (upperBoundOnly) + " upper bound only");
    check (offsMatched > 50000,
           "almost no note off was matched to the key that owed it: " + std::to_string (offsMatched));
    check (deferredAno > 500,
           "All Notes Off was almost never sent into a pedal that was holding something: "
             + std::to_string (deferredAno));

    std::printf ("       %d of 20000 streams were still sounding at the release, "
                 "%d latched keys seen, %d pedalled All Notes Off\n",
                 ringingAtRelease, latchedSeen, deferredAno);
    std::printf ("       %d streams compared exactly, %d as an upper bound, "
                 "%d note offs matched to the key that owed them\n",
                 exactStreams, upperBoundOnly, offsMatched);
}

/** The arrangement's own notes obey the same pedal, against the same receiver.

    The randomised check above drives the live row only, because a note from
    the wire only ever reaches the live row. So nothing was comparing the
    arrangement row against anything, and `setPedal` forgetting to release the
    arrangement's latched notes survived a mutation run: the engine would have
    gone on believing it was holding notes a conformant receiver had already
    let go, and sent their offs again at the next stop.

    This drives the other row the way the engine drives it, which is directly
    through `noteOn` and `noteOff` where it schedules a clip, with the pedal
    and the channel mode messages still arriving from the wire. The player
    touches nothing, so one row against one receiver is unambiguous: the whole
    point of the clip row is that it reaches the same voices through the same
    cable.
*/
static void theArrangementRowObeysTheSamePedal()
{
    std::mt19937 rng (0xc11b64u);

    // The notes of one stream are drawn from one octave of a randomly placed
    // window rather than from the whole keyboard, and the window moves from
    // stream to stream so the whole range is still covered. Drawn from all
    // 128 keys a release almost never lands on a key the stream is still
    // holding, and the state this check is about, a clip note released while
    // the pedal holds it up, was reached a few hundred times in 8000 streams
    // instead of a few thousand. The counter at the bottom is what said so.
    std::uniform_int_distribution<int> aBase (0, 116);
    std::uniform_int_distribution<int> anOffset (0, 11);

    // Notes outnumber the channel mode messages, because All Notes Off and
    // All Sound Off each act on the whole row and a stream made mostly of
    // them spends its time wiping the state the check is about.
    std::uniform_int_distribution<int> aKind (0, 17);
    std::uniform_int_distribution<int> aValue (0, 127);
    std::uniform_int_distribution<int> aStream (1, 40);
    std::bernoulli_distribution half (0.5);

    int latchedSeen = 0, ringingAtRelease = 0, releasedByTheFoot = 0;

    for (int trial = 0; trial < 8000; ++trial)
    {
        Storage s;
        Receiver reference;
        const auto c = s.channel();
        bool modeMessageSeen = false;

        const int messages = aStream (rng);
        const int base = aBase (rng);

        for (int m = 0; m < messages; ++m)
        {
            const int kind = aKind (rng);

            if (kind <= 11)
            {
                // A clip note, scheduled and recorded in one place, which is
                // what the clip scheduling in AudioEngine::scheduleMidi and
                // releaseStaleClipNotes do.
                const int note = base + anOffset (rng);
                const bool on  = kind <= 5;

                // How often a clip note was released while the pedal was
                // holding it up, which is the loop wrap this header was found
                // by, counted before the move that defers it.
                if (! on && c.pedalDown() && HoldPedal::isSounding (s.clip[note]))
                    ++releasedByTheFoot;

                if (on)
                {
                    HoldPedal::noteOn (s.clip, note);
                    reference.noteOn (note);
                }
                else
                {
                    HoldPedal::noteOff (s.clip, note, c.pedalDown());
                    reference.noteOff (note);
                }
            }
            else
            {
                unsigned char data[3] { 0xb0, 0, 0 };

                if (kind <= 14)
                {
                    data[1] = 64;
                    data[2] = (unsigned char) (half (rng) ? aValue (rng)
                                                          : (half (rng) ? 127 : 0));
                }
                else if (kind == 15) data[1] = 123;
                else if (kind == 16) data[1] = 120;
                else               { data[1] = 121; data[2] = (unsigned char) aValue (rng); }

                // From the first channel mode message onward the model is
                // deliberately an upper bound rather than a match: it does not
                // act on one, because recognising All Notes Off is not
                // required of a receiver and forgetting a note is the costly
                // guess. See `aChannelModeMessageFromTheWireIsNotActedOn`.
                if (data[1] == 120 || data[1] == 123)
                    modeMessageSeen = true;

                HoldPedal::receive (c, data, 3);
                reference.receive (data, 3);
            }

            check (c.pedalDown() == reference.pedal,
                   "the model and the receiver disagree about the pedal");

            for (int note = 0; note < HoldPedal::kNotes; ++note)
            {
                const bool model = HoldPedal::isSounding (s.clip[note]);
                const bool real  = reference.ringing[note];

                if (modeMessageSeen ? (real && ! model) : (model != real))
                {
                    fail (std::string (modeMessageSeen
                                         ? "the arrangement row has forgotten a note the receiver "
                                           "is still sounding, which it may never do: note "
                                         : "the arrangement row and the receiver disagree about note ")
                            + std::to_string (note) + ": the model says "
                            + (model ? "sounding" : "silent")
                            + " and the receiver says "
                            + (real ? "sounding" : "silent"));
                    break;
                }

                if (s.clip[note] == HoldPedal::latched)
                    ++latchedSeen;
            }

            // The player's row is not the arrangement's, so nothing here may
            // have reached it.
            for (int note = 0; note < HoldPedal::kNotes; ++note)
                if (s.live[note] != HoldPedal::silent)
                {
                    fail ("scheduling the arrangement's notes reached the player's row");
                    break;
                }
        }

        if (reference.soundingCount() > 0)
            ++ringingAtRelease;

        HoldPedal::Message out[HoldPedal::kMaxReleaseMessages];
        const int count = HoldPedal::releaseSequence (c, 1, HoldPedal::Rows {}, true, true,
                                                      out, HoldPedal::kMaxReleaseMessages);
        check (count >= 0, "the release was refused with room for everything");

        for (int i = 0; i < count; ++i)
        {
            const unsigned char bytes[3] { out[i].status, out[i].data1, out[i].data2 };
            reference.receive (bytes, 3);
        }

        if (reference.soundingCount() != 0)
            fail ("the release left " + std::to_string (reference.soundingCount())
                    + " of the arrangement's notes sounding in a conformant receiver");
    }

    check (latchedSeen > 5000,
           "the generator barely ever latched one of the arrangement's notes: "
             + std::to_string (latchedSeen));
    check (ringingAtRelease > 4000,
           "too few streams had anything left sounding when the release came: "
             + std::to_string (ringingAtRelease));
    check (releasedByTheFoot > 2000,
           "the generator almost never released a clip note under the pedal, which is "
           "the loop wrap this header was found by: " + std::to_string (releasedByTheFoot));

    std::printf ("       %d latched clip keys seen, %d of 8000 streams sounding at the release, "
                 "%d clip notes released under the pedal\n",
                 latchedSeen, ringingAtRelease, releasedByTheFoot);
}

/** A note from the wire never touches the arrangement row.

    The two rows exist because the engine knows where each note came from, and
    a note it did not schedule is not one it is holding. Written as its own
    check because the receiver comparisons cannot see it: both rows reach the
    same voices, so a duplicate entry sends a duplicate note off and a
    conformant receiver is left silent either way. What it costs is in the
    engine, where `releaseStaleClipNotes` sends an off for any key of the clip
    row no clip is holding. A live note copied into that row is therefore cut
    off mid-phrase at the next block, by the arrangement, for a note the
    arrangement never played. Two mutations that copied a wire note into the
    clip row passed everything else in this file.
*/
static void aNoteFromTheWireNeverTouchesTheArrangement()
{
    std::mt19937 rng (0x57a9eu);
    std::uniform_int_distribution<int> aNote (0, 127);
    std::uniform_int_distribution<int> aState (0, 2);
    std::uniform_int_distribution<int> aKind (0, 2);
    std::uniform_int_distribution<int> aStream (1, 20);
    std::bernoulli_distribution half (0.5);

    int collisions = 0;

    for (int trial = 0; trial < 4000; ++trial)
    {
        Storage s;
        const auto c = s.channel();

        // The pedal is set directly rather than by message, because lifting
        // it is entitled to release the arrangement's latched notes and this
        // check is about note messages alone.
        s.down = half (rng) ? 1 : 0;
        const bool pedalWas = s.down != 0;

        for (int note = 0; note < HoldPedal::kNotes; ++note)
            s.clip[note] = (unsigned char) aState (rng);

        unsigned char before[HoldPedal::kNotes];
        for (int note = 0; note < HoldPedal::kNotes; ++note)
            before[note] = s.clip[note];

        const int messages = aStream (rng);

        for (int m = 0; m < messages; ++m)
        {
            const int note = aNote (rng);
            unsigned char data[3] { 0x90, (unsigned char) note, 0 };

            switch (aKind (rng))
            {
                case 0:  data[2] = (unsigned char) (1 + note % 126); break;   // struck
                case 1:  data[2] = 0;                                break;   // released at nought
                default: data[0] = 0x80;                             break;   // released properly
            }

            if (s.clip[note] != HoldPedal::silent)
                ++collisions;

            HoldPedal::receive (c, data, 3);
        }

        for (int note = 0; note < HoldPedal::kNotes; ++note)
            if (s.clip[note] != before[note])
            {
                fail ("a note from the wire changed the arrangement's row at note "
                        + std::to_string (note));
                break;
            }

        // Against what was written before the stream, not against
        // `c.pedalDown()`: that reads the same byte, so the comparison would
        // have held whatever the header did to it.
        check ((s.down != 0) == pedalWas, "a note message moved the pedal");
    }

    // The assertion is only about keys the wire note could have overwritten,
    // so the count of notes that landed on an occupied key is what says it was
    // asked anything at all.
    check (collisions > 2000,
           "the generator almost never aimed a wire note at a key the arrangement holds: "
             + std::to_string (collisions));

    std::printf ("       %d wire notes aimed at a key the arrangement was holding\n", collisions);
}

/** A row that arrives already latched with the pedal up is repaired.

    `HoldPedal.h` says plainly that everything latched is released whenever the
    pedal is not down, rather than only on a transition from down, and that
    the two differ only in a state it cannot itself produce. They do: a note
    off and a pedal lift both go through the pedal's own position, so nothing
    in the header leaves a latched key behind an up pedal. Driven over 1.6
    million randomised steps the two versions are byte for byte identical, so
    this is not a fault the header can reach.

    It is pinned anyway, because the rows are the engine's and the engine
    writes them by hand in three places. An unreachable state nobody tests is
    where the next caller's bug lives, and a repair that quietly becomes a
    no-op is how a decision written down in a comment gets undone.
*/
static void aRowThatArrivesLatchedWithThePedalUpIsRepaired()
{
    {
        Storage s;
        const auto c = s.channel();

        s.down = 0;
        s.live[60] = HoldPedal::latched;
        s.clip[72] = HoldPedal::latched;

        const unsigned char pedalUp[] = { 0xb0, 64, 0 };
        HoldPedal::receive (c, pedalUp, 3);

        check (s.live[60] == HoldPedal::silent,
               "a pedal already up left a latched key latched, so nothing can ever release it");
        check (s.clip[72] == HoldPedal::silent,
               "a pedal already up left a latched arrangement key latched");
    }

    {
        // And the other way into the same state: a release arriving for a key
        // that is latched while the pedal is up has to let it go, not decide
        // the key was not sounding.
        Storage s;
        s.down = 0;
        s.live[60] = HoldPedal::latched;

        HoldPedal::noteOff (s.live, 60, false);
        check (s.live[60] == HoldPedal::silent,
               "a note off with the pedal up ignored a latched key");
    }
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
    const int count = HoldPedal::releaseSequence (c, 3, HoldPedal::Rows {}, true, true,
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
    check (HoldPedal::releaseSequence (empty, 1, HoldPedal::Rows {}, true, true,
                                       out, HoldPedal::kMaxReleaseMessages) == 0,
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

    HoldPedal::noteOn (keys, -1);
    HoldPedal::noteOn (keys, 128);
    HoldPedal::noteOff (keys, -1, true);
    HoldPedal::noteOff (keys, 9999, false);

    for (int note = 0; note < HoldPedal::kNotes; ++note)
        check (keys[note] == HoldPedal::silent, "a note number off the end reached a key");
}

//===========================================================================
//===========================================================================
/** The receiver the belt over braces note off exists for, which is the only
    oracle that can see whether it was sent.

    `needsNoteOff` answers `isSounding` rather than `key == sounding` when the
    pedal is coming up, so a latched key gets a note off it does not strictly
    need. Against a conformant receiver that message is invisible by
    construction: the lift goes out first and a conformant receiver releases
    everything the pedal was holding on the lift, so the note off behind it
    changes nothing and both randomised checks in this file agree with the
    header whether it was sent or not.

    An audit measured that and it is worth stating as the gap it was: deleting
    the asymmetry from `needsNoteOff` left the whole of the rest of this file
    green, and the engine then sent, at a stop, byte for byte what it sent
    before the pedal was modelled at all. The one assertion holding it was a
    restatement of the expression under test.

    So the oracle has to be the receiver the header names as the reason:

        "it covers the receiver that implements the pedal badly enough to drop
        the release it was holding"

    This is that receiver, and it is a real half implementation rather than an
    invented one. It defers a note off while the pedal is down, as required,
    and then forgets it on the lift instead of acting on it. A plugin written
    that way sounds correct for as long as nobody uses a pedal and hangs the
    chord the moment somebody does, which is the hardest kind of wrong to
    diagnose and exactly what a belt over braces release is insurance against.

    The property asserted is the one a player would notice: after the
    sequence, with no All Notes Off backstop allowed to rescue it, this
    receiver has nothing sounding either.
*/
struct SloppyReceiver
{
    bool pedal = false;
    bool ringing[HoldPedal::kNotes] {};

    void noteOn (int note)  { ringing[note] = true; }

    void noteOff (int note)
    {
        // Deferred, as required. And then dropped rather than remembered,
        // which is the half of the rule this receiver gets wrong.
        if (! pedal)
            ringing[note] = false;
    }

    void receive (const unsigned char* data, int numBytes)
    {
        if (numBytes < 3)
            return;

        const int status = data[0] & 0xf0;
        const int first  = data[1] & 0x7f;

        if (status == 0x90)
        {
            if ((data[2] & 0x7f) != 0)
                noteOn (first);
            else
                noteOff (first);
        }
        else if (status == 0x80)
        {
            noteOff (first);
        }
        else if (status == 0xb0 && (first == 64 || first == 121))
        {
            pedal = first == 64 ? (data[2] & 0x7f) >= 64 : false;
        }
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

static void aReleaseSilencesAReceiverThatDropsWhatItDeferred()
{
    std::mt19937 rng (0x5e11a7u);
    std::uniform_int_distribution<int> aNote (0, HoldPedal::kNotes - 1);
    std::uniform_int_distribution<int> aCoin (0, 1);

    int hungWithoutTheOffs = 0, streamsWithLatchedKeys = 0;

    for (int stream = 0; stream < 4000; ++stream)
    {
        Storage s;
        const auto c = s.channel();
        SloppyReceiver sloppy;

        // A pedal down, some keys struck, some of them released under it.
        // Everything goes through the wire so that the model and the receiver
        // see exactly the same bytes.
        const unsigned char press[3] { 0xb0, 64, 127 };
        HoldPedal::receive (c, press, 3);
        sloppy.receive (press, 3);

        const int howMany = 1 + (int) (rng() % 8);

        for (int i = 0; i < howMany; ++i)
        {
            const int note = aNote (rng);
            const unsigned char on[3] { 0x90, (unsigned char) note, 100 };
            HoldPedal::receive (c, on, 3);
            sloppy.receive (on, 3);

            if (aCoin (rng) != 0)
            {
                const unsigned char off[3] { 0x80, (unsigned char) note, 0 };
                HoldPedal::receive (c, off, 3);
                sloppy.receive (off, 3);
            }
        }

        int latched = 0;
        for (int note = 0; note < HoldPedal::kNotes; ++note)
            if (s.live[note] == HoldPedal::latched)
                ++latched;

        if (latched > 0)
            ++streamsWithLatchedKeys;

        // What this receiver is left ringing, ON THE LATCHED KEYS ONLY, if the
        // release sends the lift and then no off for them, which is what it
        // sends if `needsNoteOff` stops asking about the pedal. Measured
        // rather than assumed, so the check cannot pass by there being nothing
        // to find, and counted per key rather than over the whole receiver: a
        // key that was never released is still ringing after a lift for an
        // ordinary reason, so counting those would saturate this figure and
        // say nothing about the property under test.
        {
            SloppyReceiver liftOnly = sloppy;
            const unsigned char lift[3] { 0xb0, 64, 0 };
            liftOnly.receive (lift, 3);

            // The release that drops the asymmetry still sends an off for
            // every key it calls `sounding`, so those are not what would hang.
            for (int note = 0; note < HoldPedal::kNotes; ++note)
                if (s.live[note] == HoldPedal::sounding)
                    liftOnly.noteOff (note);

            hungWithoutTheOffs += liftOnly.soundingCount();
        }

        // The release the engine makes at a stop, with no backstop: the
        // backstop would silence any receiver by itself and hide the whole
        // question.
        HoldPedal::Message out[HoldPedal::kMaxReleaseMessages];
        const int count = HoldPedal::releaseSequence (c, 1, HoldPedal::Rows {}, true, false,
                                                      out, HoldPedal::kMaxReleaseMessages);

        check (count >= 0, "the release was refused");

        for (int i = 0; i < count; ++i)
        {
            const unsigned char bytes[3] { out[i].status, out[i].data1, out[i].data2 };
            sloppy.receive (bytes, 3);
        }

        check (sloppy.soundingCount() == 0,
               "a receiver that drops the release it deferred was left ringing after a stop");
    }

    // The check is only worth anything if the thing it is insurance against
    // really does happen in these streams.
    check (hungWithoutTheOffs > 1000,
           "dropping the belt over braces off hangs almost nothing here, so the check proves nothing");
    check (streamsWithLatchedKeys > 1000,
           "almost no stream latched a key, so the belt over braces off was never exercised");

    std::printf ("       %d keys would hang without the belt over braces off, %d of 4000 streams latched something\n",
                 hungWithoutTheOffs, streamsWithLatchedKeys);
}

//===========================================================================
/** The worst case fits in `kMaxReleaseMessages`, exactly, and one less is
    refused.

    `AudioEngine::stopTrackedNotes` puts a `kMaxReleaseMessages` array on the
    stack and reads the returned count. A refusal there is not a safe failure:
    the engine's loop runs zero times, so every note off and the pedal lift
    for that MIDI channel are dropped with nothing in the signal to say so,
    which at a stop is every tracked note on that channel hanging. So the
    constant being big enough is load bearing, and nothing checked it: an
    audit cut it by one, which is the backstop's slot, and the whole of the
    rest of this file stayed green, because both randomised generators draw
    each key independently and never once produce all 256 keys sounding.

    This is that case, written out rather than generated.
*/
static void theWorstCaseFitsExactly()
{
    Storage s;
    const auto c = s.channel();

    // Both rows sounding, every key, and the pedal down so that the lift has
    // something to do. Sounding rather than latched because a latched key
    // owes no note off where the pedal stays down, and the most a release can
    // ever write is the case where every key owes one.
    s.down = 1;
    for (int note = 0; note < HoldPedal::kNotes; ++note)
    {
        s.live[note] = HoldPedal::sounding;
        s.clip[note] = HoldPedal::sounding;
    }

    const int worst = 1 + HoldPedal::kNotes + HoldPedal::kNotes + 1;

    check (HoldPedal::kMaxReleaseMessages == worst,
           "kMaxReleaseMessages is not the lift, both rows and the backstop");

    {
        std::vector<HoldPedal::Message> out ((size_t) worst, HoldPedal::Message {});
        check (HoldPedal::releaseSequence (c, 1, HoldPedal::Rows {}, true, true,
                                           out.data(), worst - 1) == -1,
               "a release one message short of the worst case was not refused");

        for (int note = 0; note < HoldPedal::kNotes; ++note)
            check (s.live[note] == HoldPedal::sounding && s.clip[note] == HoldPedal::sounding,
                   "a refused release changed the model anyway");

        check (c.pedalDown(), "a refused release lifted the pedal anyway");
    }

    // Exactly the room the constant promises, and a buffer exactly that long,
    // so that writing one message more is an overrun the sanitizers catch
    // rather than a number nobody compares.
    std::vector<HoldPedal::Message> out ((size_t) HoldPedal::kMaxReleaseMessages,
                                         HoldPedal::Message {});
    const int count = HoldPedal::releaseSequence (c, 1, HoldPedal::Rows {}, true, true,
                                                  out.data(), HoldPedal::kMaxReleaseMessages);

    check (count == worst, "the worst case did not write what the constant promises");

    // And it is the engine's configuration, with no backstop, that the engine
    // actually has to fit: one less than the constant.
    Storage s2;
    const auto c2 = s2.channel();
    s2.down = 1;
    for (int note = 0; note < HoldPedal::kNotes; ++note)
    {
        s2.live[note] = HoldPedal::sounding;
        s2.clip[note] = HoldPedal::sounding;
    }

    std::vector<HoldPedal::Message> out2 ((size_t) HoldPedal::kMaxReleaseMessages,
                                          HoldPedal::Message {});
    check (HoldPedal::releaseSequence (c2, 1, HoldPedal::Rows {}, true, false,
                                        out2.data(), HoldPedal::kMaxReleaseMessages)
             == worst - 1,
           "the release the engine makes did not fit the engine's own buffer");
}

//===========================================================================
/** Every release runs with a buffer exactly as long as the number of messages
    it should produce, over randomised states.

    This is the check that the room `releaseSequence` counts up front is the
    room it then uses, and it catches a disagreement in both directions
    without needing to see the private figure. Count too much and the call is
    refused at a capacity that should have been enough. Count too little and
    the write runs off the end of a buffer that is exactly the right length,
    which address sanitizer reports on the line that did it.

    An audit changed the counting loop's question from "does this key need an
    off, given what the pedal is doing" to "is this key sounding" and the rest
    of this file could not see it, because every other check hands over a
    buffer with room to spare.

    The expectation is built from the key states directly rather than by
    calling `needsNoteOff`, because a check that asks the function under test
    how many messages to expect agrees with it whatever it answers.
*/
static void theRoomCountedIsTheRoomUsed()
{
    std::mt19937 rng (0xf17c0u);
    std::uniform_int_distribution<int> aState (0, 2);
    std::uniform_int_distribution<int> aCoin (0, 1);

    int atCapacity = 0;

    for (int trial = 0; trial < 60000; ++trial)
    {
        Storage s;
        const bool withClip = aCoin (rng) != 0;
        const auto c = s.channel (withClip);

        s.down = (unsigned char) aCoin (rng);

        for (int note = 0; note < HoldPedal::kNotes; ++note)
        {
            s.live[note] = (unsigned char) aState (rng);
            s.clip[note] = (unsigned char) aState (rng);
        }

        // A key cannot be latched with the pedal up: that state is unreachable
        // from the wire and is repaired elsewhere, so it is not generated here.
        if (s.down == 0)
            for (int note = 0; note < HoldPedal::kNotes; ++note)
            {
                if (s.live[note] == HoldPedal::latched) s.live[note] = HoldPedal::sounding;
                if (s.clip[note] == HoldPedal::latched) s.clip[note] = HoldPedal::sounding;
            }

        const HoldPedal::Rows rows { aCoin (rng) != 0, aCoin (rng) != 0 };
        const bool lift     = aCoin (rng) != 0;
        const bool backstop = aCoin (rng) != 0;

        // What should go out, from the states themselves. A note off for every
        // key of a selected row that the receiver has not already been told to
        // stop, plus the latched ones too where the pedal is coming up.
        int expected = (lift ? 1 : 0) + (backstop ? 1 : 0);

        for (int which = 0; which < 2; ++which)
        {
            const unsigned char* keys = which == 0 ? (withClip && rows.clip ? s.clip : nullptr)
                                                   : (rows.live ? s.live : nullptr);
            if (keys == nullptr)
                continue;

            for (int note = 0; note < HoldPedal::kNotes; ++note)
            {
                if (keys[note] == HoldPedal::sounding)
                    ++expected;
                else if (keys[note] == HoldPedal::latched && lift)
                    ++expected;
            }
        }

        // Exactly that long, so one message too many is an overrun and not a
        // spare slot nobody looks at.
        std::vector<HoldPedal::Message> out ((size_t) (expected > 0 ? expected : 1),
                                             HoldPedal::Message {});

        // Taken before the call, because the call moves the keys it sends an
        // off for and the one short check has to start from the same state.
        Storage again = s;

        const int count = HoldPedal::releaseSequence (c, 1, rows, lift, backstop,
                                                      out.data(), expected);

        check (count == expected,
               "a release given exactly the room it needs wrote " + std::to_string (count)
                 + " where " + std::to_string (expected) + " was due");

        if (count == expected)
            ++atCapacity;

        // And one message short is refused, every time, which is the other
        // half of the same figure: a count that is too low would have fitted
        // here and overrun the buffer above.
        if (expected > 0)
        {
            const auto c2 = again.channel (withClip);
            std::vector<HoldPedal::Message> small ((size_t) expected, HoldPedal::Message {});

            check (HoldPedal::releaseSequence (c2, 1, rows, lift, backstop,
                                                small.data(), expected - 1) == -1,
                   "a release one message short of what it needs was not refused");
        }
    }

    check (atCapacity > 50000, "almost nothing ran at exact capacity");

    std::printf ("       %d releases ran with a buffer exactly as long as they needed\n",
                 atCapacity);
}

//===========================================================================
/** Every message a release writes carries the MIDI channel it was asked for.

    Nothing checked this: every other call in this file asks for channel 1,
    where the nibble is nought and dropping the mask entirely changes no byte.
    A mutation run removed `& 0x0f` and the whole file stayed green.

    It matters for two separate reasons. The engine calls this once per MIDI
    channel of a slot, so a nibble that did not follow the argument would send
    all sixteen channels' note offs to one channel and hang the other fifteen.
    And the mask is what keeps the status byte legal if a caller ever hands
    over a channel outside one to sixteen: without it, the number runs into the
    status nibble and a note off becomes some other kind of message entirely,
    which is a far worse failure than being sent to the wrong channel.
*/
static void everyMessageCarriesTheChannelItWasAskedFor()
{
    for (int midiChannel = 1; midiChannel <= 16; ++midiChannel)
    {
        Storage s;
        const auto c = s.channel();

        s.down = 1;
        s.live[60] = HoldPedal::sounding;
        s.live[64] = HoldPedal::latched;
        s.clip[67] = HoldPedal::sounding;

        HoldPedal::Message out[HoldPedal::kMaxReleaseMessages];
        const int count = HoldPedal::releaseSequence (c, midiChannel, HoldPedal::Rows {},
                                                      true, true,
                                                      out, HoldPedal::kMaxReleaseMessages);

        check (count == 1 + 3 + 1, "the release did not write what the state owed");

        const unsigned char want = (unsigned char) (midiChannel - 1);

        for (int i = 0; i < count; ++i)
        {
            check ((out[i].status & 0x0f) == want,
                   "a message went out on MIDI channel "
                     + std::to_string ((out[i].status & 0x0f) + 1) + " rather than "
                     + std::to_string (midiChannel));

            const int kind = out[i].status & 0xf0;
            check (kind == 0x80 || kind == 0xb0,
                   "a release wrote something that is neither a note off nor a controller");
        }
    }

    // A channel outside one to sixteen is masked rather than allowed to run
    // into the status nibble. The engine cannot produce one, so this is about
    // the byte staying a legal MIDI message rather than about where it lands.
    for (int midiChannel : { -7, 0, 17, 33, 1000 })
    {
        Storage s;
        const auto c = s.channel();

        s.down = 0;
        s.live[72] = HoldPedal::sounding;

        HoldPedal::Message out[HoldPedal::kMaxReleaseMessages];
        const int count = HoldPedal::releaseSequence (c, midiChannel, HoldPedal::Rows {},
                                                      false, false,
                                                      out, HoldPedal::kMaxReleaseMessages);

        check (count == 1, "the release did not write the one note off that was owed");
        check ((out[0].status & 0xf0) == 0x80,
               "an out of range channel turned a note off into another kind of message");
    }
}

int main()
{
    std::printf ("HoldPedal\n");

    aPedalledNoteOffIsDeferred();
    aChannelModeMessageFromTheWireIsNotActedOn();
    theThresholdIsSixtyFour();
    resetAllControllersLiftsThePedal();
    aMalformedMessageIsIgnored();
    aRetriggerUnderThePedalIsSoundingAgain();
    aChannelWithNoArrangementRowIsLeftAlone();
    anInvalidChannelIsRefused();
    aRowThatArrivesLatchedWithThePedalUpIsRepaired();
    theReleaseNeverPressesThePedal();
    aWrapDoesNotSendTheSameNoteOffTwice();
    aReleaseThatWillNotFitIsRefused();
    theWorstCaseFitsExactly();
    everyMessageCarriesTheChannelItWasAskedFor();
    theRoomCountedIsTheRoomUsed();
    aReleaseSilencesAReceiverThatDropsWhatItDeferred();
    theModelAgreesWithAConformantReceiver();
    theArrangementRowObeysTheSamePedal();
    aNoteFromTheWireNeverTouchesTheArrangement();

    if (failures == 0)
        std::printf ("  ok\n");
    else
        std::printf ("  %d failure(s)\n", failures);

    return failures == 0 ? 0 : 1;
}
