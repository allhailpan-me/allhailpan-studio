// ---------------------------------------------------------------------------
// Checks what the transport silences, and how gently.
//
// The decision is four booleans in and five messages out, and it lived in the
// device callback where nothing could reach it. Three faults lived in it
// there, all of them audible and none of them visible:
//
//   a chord held on a controller was killed at every loop wrap
//   the monitoring and the recorded take disagreed across that wrap
//   the first press of Stop already did the harshest thing there is, so the
//   second press, which is the panic button, had nothing left to escalate to
//
// So the properties below are asserted over every one of the sixteen
// combinations of events rather than over the handful somebody thought of.
// Two of them are what actually keeps this honest: a panic is strictly
// stronger than anything else, and adding an event to a block never takes a
// message away. A table that satisfies both cannot quietly acquire the fault
// that started this, where a wrap landing in the same block as nothing at all
// still sent the message that cuts a note dead.
//
//   ./Tests/run.sh
//
// The two messages and what they are each defined to do:
//   MIDI 1.0 Detailed Specification v4.2.1, MMA 96.1-4, "Channel Mode
//   Messages" p. 25 (All Sound Off) and Appendix, "Hold Pedal and 'All Notes
//   Off'" p. A-5 (All Notes Off is a release, and the pedal outranks it).
// ---------------------------------------------------------------------------

#include "TransportSilence.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace TransportSilence;

static int failures = 0;

static void fail (const std::string& what)
{
    std::printf ("  FAIL  %s\n", what.c_str());
    ++failures;
}

static void check (bool ok, const std::string& what)
{
    if (! ok)
        fail (what);
}

/** Every combination of the four, so nothing is checked only in the shape
    somebody happened to imagine. */
static std::vector<Event> everyEvent()
{
    std::vector<Event> all;

    for (int bits = 0; bits < 16; ++bits)
        all.push_back ({ (bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0, (bits & 8) != 0 });

    return all;
}

static std::string describe (const Event& e)
{
    std::string s;
    if (e.stopped)   s += "stopped ";
    if (e.jumped)    s += "jumped ";
    if (e.wrapped)   s += "wrapped ";
    if (e.panicking) s += "panicking ";
    return s.empty() ? "nothing" : s;
}

//==============================================================================
static void nothingHappenedMeansNothingIsSent()
{
    std::printf ("a block where nothing happened sends nothing\n");

    const auto action = actionFor ({});

    check (! action.anything(), "a quiet block sent something");
}

static void anythingAgreesWithTheTable()
{
    std::printf ("the shorthand for \"is there anything to send\" agrees with the table\n");

    // The engine asks this one question rather than five, to decide whether a
    // block that is already silencing something should also chase an edit, so
    // it has to mean exactly what the five flags say. A wrap is the case that
    // catches a wrong answer: it is the one event that asks for a single
    // message, so anything() built out of the wrong operator reads it as a
    // quiet block.
    for (const auto& e : everyEvent())
    {
        const auto a = actionFor (e);
        const bool byHand = a.clipNoteOffs || a.liveNoteOffs || a.sustainUp
                         || a.allNotesOff  || a.allSoundOff;

        check (a.anything() == byHand, "anything() disagrees with its own flags: " + describe (e));

        const bool happened = e.stopped || e.jumped || e.wrapped || e.panicking;
        check (a.anything() == happened,
               "anything() reads " + describe (e) + " the wrong way round");
    }
}

static void coversIsAnHonestComparison()
{
    std::printf ("covers() is an honest comparison\n");

    // Two of the properties below are written in terms of covers(), so a
    // covers() that said yes too easily would hollow both of them out without
    // failing anything. Checked against every pair of hand built actions
    // rather than against the five the table happens to produce.
    for (int a = 0; a < 32; ++a)
        for (int b = 0; b < 32; ++b)
        {
            Action x, y;
            bool* xf[] = { &x.clipNoteOffs, &x.liveNoteOffs, &x.sustainUp, &x.allNotesOff, &x.allSoundOff };
            bool* yf[] = { &y.clipNoteOffs, &y.liveNoteOffs, &y.sustainUp, &y.allNotesOff, &y.allSoundOff };

            for (int bit = 0; bit < 5; ++bit)
            {
                *xf[bit] = (a & (1 << bit)) != 0;
                *yf[bit] = (b & (1 << bit)) != 0;
            }

            // x covers y exactly when x has every flag y has.
            check (x.covers (y) == ((a & b) == b),
                   "covers() got " + std::to_string (a) + " against " + std::to_string (b)
                   + " wrong");

            // And anything() over every combination of flags rather than only
            // the five the table makes today. Checked only against those, it
            // reads correctly while being written out of the wrong operator,
            // because every action the table produces happens to send the clip
            // note offs and that one flag carries the answer on its own. A
            // helper the engine asks a question of has to be right for any
            // action, not for the ones that exist this week.
            check (x.anything() == (a != 0),
                   "anything() got the flag set " + std::to_string (a) + " wrong");
        }
}

static void onlyAPanicCuts()
{
    std::printf ("All Sound Off goes out for a panic and for nothing else\n");

    for (const auto& e : everyEvent())
    {
        const auto action = actionFor (e);

        if (e.panicking)
            check (action.allSoundOff, "a panic did not cut: " + describe (e));
        else
            check (! action.allSoundOff,
                   "All Sound Off went out without a panic: " + describe (e));
    }
}

static void aWrapIsAContinuation()
{
    std::printf ("a loop wrap leaves everything sounding that is still wanted\n");

    const auto action = actionFor ({ false, false, true, false });

    // The one thing it does. A clip note whose off beat lies past the loop end
    // never reaches its off, so without this it hangs for the rest of the
    // session.
    check (action.clipNoteOffs, "a wrap did not release the clip notes, so one can hang");

    // And everything it must not do. This is the fault that started this file:
    // a chord held on a controller across the loop point was released and
    // silenced, and nothing brought it back.
    check (! action.liveNoteOffs, "a wrap killed a note somebody was holding down");
    check (! action.sustainUp,    "a wrap lifted the sustain pedal, which nothing puts back");
    check (! action.allNotesOff,  "a wrap sent a channel mode message");
    check (! action.allSoundOff,  "a wrap cut every voice dead");
}

static void aStopReleasesAndDoesNotCut()
{
    std::printf ("a stop and a jump release, and are the same strength\n");

    const auto stop = actionFor ({ true, false, false, false });
    const auto jump = actionFor ({ false, true, false, false });

    for (const auto& a : { stop, jump })
    {
        check (a.clipNoteOffs, "a stop or a jump left a clip note sounding");
        check (a.liveNoteOffs, "a stop or a jump left a live note sounding");
        check (a.allNotesOff,  "a stop or a jump sent nothing for the notes it is not tracking");

        // The whole point: the tail rings out. Every other release path in the
        // studio is gentle and this one was not.
        check (! a.allSoundOff, "a stop or a jump cut every voice dead instead of releasing it");
    }

    // A locate is not a request to destroy the instrument's sound, so it asks
    // for exactly what a stop asks for.
    check (stop.covers (jump) && jump.covers (stop), "a stop and a jump disagree");
}

static void thePedalIsLiftedBeforeAnythingIsReleased()
{
    std::printf ("anything that releases lifts the pedal first\n");

    // A note being held up by the sustain pedal survives a note off, and
    // survives All Notes Off too: the specification gives the pedal priority
    // over both until it is released. So a release that does not lift the
    // pedal first is not a release at all on the one patch where it matters.
    for (const auto& e : everyEvent())
    {
        const auto action = actionFor (e);

        if (action.allNotesOff || action.liveNoteOffs)
            check (action.sustainUp,
                   "notes were released with the pedal still down: " + describe (e));
    }
}

static void theGentlerMessageAlwaysGoesWithTheHarsherOne()
{
    std::printf ("All Sound Off never travels without All Notes Off\n");

    // Recognising All Notes Off is not required of a receiver and neither is
    // All Sound Off, and they are not the same vintage: All Sound Off was
    // added to the specification later. Sending the pair costs nothing and a
    // receiver that knows only the older one still does the right thing.
    for (const auto& e : everyEvent())
    {
        const auto action = actionFor (e);

        if (action.allSoundOff)
            check (action.allNotesOff,
                   "All Sound Off went out alone: " + describe (e));
    }
}

static void aPanicIsStrictlyStronger()
{
    std::printf ("a panic does everything every other event does, and more\n");

    // This is what makes the second press of Stop mean something. It is
    // documented in MainComponent as the escalation for plugins that drone on
    // regardless of note offs, and it was reduced to nothing by the first
    // press already sending the harshest message there is.
    const auto panic = actionFor ({ false, false, false, true });

    for (const auto& e : everyEvent())
    {
        const auto action = actionFor (e);

        check (panic.covers (action),
               "a panic does less than " + describe (e) + " does");

        if (! e.panicking)
            check (! action.covers (panic),
                   describe (e) + " is already as strong as a panic, so nothing escalates");
    }
}

static void addingAnEventNeverTakesAMessageAway()
{
    std::printf ("two events in one block ask for at least what either asks alone\n");

    // Events are not independent: a block can stop and wrap at once. Reading
    // them as flags that accumulate is how the wrap came to send All Sound Off,
    // so the table is written as the strongest reason first. This is the check
    // that the rewrite did not lose anything on the way: for every pair of
    // event sets where one contains the other, the larger one has to ask for
    // at least as much.
    const auto all = everyEvent();

    for (int a = 0; a < 16; ++a)
        for (int b = 0; b < 16; ++b)
        {
            if ((a & b) != a)        // a is not a subset of b
                continue;

            const auto smaller = actionFor (all[(std::size_t) a]);
            const auto larger  = actionFor (all[(std::size_t) b]);

            check (larger.covers (smaller),
                   describe (all[(std::size_t) b]) + " asks for less than "
                   + describe (all[(std::size_t) a]) + " does");
        }
}

static void everyEventSilencesTheClipNotes()
{
    std::printf ("every event releases the clip notes the engine is holding\n");

    // Whatever else is true, a clip note the engine put down has to be picked
    // up again, or it hangs: nothing else in the studio sends its off.
    for (const auto& e : everyEvent())
    {
        const auto action = actionFor (e);

        if (e.stopped || e.jumped || e.wrapped || e.panicking)
            check (action.clipNoteOffs, "a clip note was left hanging by: " + describe (e));
    }
}

int main()
{
    std::printf ("transport silence\n");

    nothingHappenedMeansNothingIsSent();
    anythingAgreesWithTheTable();
    coversIsAnHonestComparison();
    onlyAPanicCuts();
    aWrapIsAContinuation();
    aStopReleasesAndDoesNotCut();
    thePedalIsLiftedBeforeAnythingIsReleased();
    theGentlerMessageAlwaysGoesWithTheHarsherOne();
    aPanicIsStrictlyStronger();
    addingAnEventNeverTakesAMessageAway();
    everyEventSilencesTheClipNotes();

    if (failures > 0)
    {
        std::printf ("\n%d transport silence check(s) failed\n", failures);
        return 1;
    }

    std::printf ("all transport silence checks passed\n");
    return 0;
}
