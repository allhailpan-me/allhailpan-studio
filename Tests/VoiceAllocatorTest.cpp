// ---------------------------------------------------------------------------
// Checks the voice bookkeeping of the built-in synth.
//
// Voice stealing is where polyphonic synths go wrong, and none of the ways it
// goes wrong are things you can hear your way to:
//
//   a voice handed out twice      a note that will not stop
//   a voice never handed back     the synth goes quiet after a few bars and
//                                 is fine again after a reload
//   a steal that is not reported  a click, then a note cut off
//
// So this plays the thing like a keyboard rather than testing the methods one
// at a time: chords, repeated notes, note offs for notes that were never on,
// sustain past the end of the pool, and a long randomised run with the
// invariants checked after every single event. The invariants are what
// matter, because the ways this breaks are all invariant violations that
// produce a perfectly plausible sounding synth right up until they do not.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include "VoiceAllocator.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

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

using Pool = VoiceAllocator<8>;

/** Everything that must be true of the pool at every moment, whatever has
    just happened to it. */
static bool invariantsHold (const Pool& pool, std::string& why)
{
    std::set<int> heldNotes;

    for (int i = 0; i < Pool::voiceCount; ++i)
    {
        const auto& v = pool.voice (i);

        // A voice that is not sounding must not be claiming a note, or the
        // next search will skip it and the pool slowly shrinks.
        if (! v.sounding && v.note != -1)
        {
            why = "voice " + std::to_string (i) + " is silent but still holds note "
                + std::to_string (v.note);
            return false;
        }

        // Held implies sounding. A voice with a key down that is not making
        // a sound is a lost note.
        if (v.held && ! v.sounding)
        {
            why = "voice " + std::to_string (i) + " is held but not sounding";
            return false;
        }

        // No note may be held by two voices at once. That is the one that
        // sounds like a note that will not stop, because the second note off
        // releases one of them and the other keeps going.
        if (v.held)
        {
            if (! heldNotes.insert (v.note).second)
            {
                why = "note " + std::to_string (v.note) + " is held by two voices";
                return false;
            }
        }
    }

    return true;
}

static void aSingleNoteGoesRoundTheWholeCycle()
{
    std::printf ("one note is taken, released and handed back\n");

    Pool pool;
    pool.reset();

    int stolen = -1;
    const int v = pool.noteOn (60, stolen);

    check (v >= 0, "no voice was given out for the first note");
    check (stolen == -1, "the first note of all stole from something");
    check (pool.countHeld() == 1, "the note was not counted as held");
    check (pool.countSounding() == 1, "the note was not counted as sounding");

    check (pool.noteOff (60) == 1, "the note off did not release exactly one voice");
    check (pool.countHeld() == 0, "still held after note off");
    check (pool.countSounding() == 1, "stopped sounding the instant the key came up");

    pool.voiceFinished (v);
    check (pool.countSounding() == 0, "the voice was not handed back when it finished");
    check (pool.voice (v).note == -1, "the voice still claims its note");
}

static void retriggeringReusesTheSameVoice()
{
    std::printf ("a note sent twice without a note off does not stack up\n");

    Pool pool;
    pool.reset();

    int stolen = -1;
    const int first = pool.noteOn (64, stolen);

    for (int i = 0; i < 20; ++i)
    {
        const int again = pool.noteOn (64, stolen);
        check (again == first, "a repeat of a held note took a second voice");
        check (stolen == -1, "a repeat of a held note reported a steal");
    }

    check (pool.countHeld() == 1, "a repeated note ended up held " + std::to_string (pool.countHeld()) + " times");
}

static void theOldestReleasingVoiceGoesFirst()
{
    std::printf ("stealing prefers a voice that is already fading\n");

    Pool pool;
    pool.reset();

    int stolen = -1;
    std::vector<int> used;

    for (int i = 0; i < Pool::voiceCount; ++i)
        used.push_back (pool.noteOn (60 + i, stolen));

    // Let two of them go, oldest first, so both are releasing.
    pool.noteOff (60);
    pool.noteOff (61);

    // The next note should take the longest released one rather than
    // interrupting anything the player is still holding down.
    const int next = pool.noteOn (80, stolen);

    check (next == used[0], "the steal did not take the longest releasing voice");
    check (stolen == 60, "the steal reported " + std::to_string (stolen) + " rather than 60");
    check (pool.countHeld() == Pool::voiceCount - 1,
           "a held note was taken while a releasing one was available");
}

static void aFullChordStealsTheOldestHeldNote()
{
    std::printf ("with every key down, the longest held note is the one that goes\n");

    Pool pool;
    pool.reset();

    int stolen = -1;
    const int firstVoice = pool.noteOn (40, stolen);

    for (int i = 1; i < Pool::voiceCount; ++i)
        pool.noteOn (40 + i, stolen);

    check (pool.countHeld() == Pool::voiceCount, "the pool did not fill");

    const int next = pool.noteOn (90, stolen);
    check (next == firstVoice, "the steal did not take the oldest held voice");
    check (stolen == 40, "the steal reported " + std::to_string (stolen) + " rather than 40");
    check (pool.countHeld() == Pool::voiceCount, "the pool lost a voice during a steal");
}

static void noteOffsForNotesThatWereNeverOnAreHarmless()
{
    std::printf ("a note off for something that was never played does nothing\n");

    Pool pool;
    pool.reset();

    check (pool.noteOff (60) == 0, "released something that was never played");

    int stolen = -1;
    pool.noteOn (60, stolen);

    check (pool.noteOff (61) == 0, "a note off hit the wrong note");
    check (pool.countHeld() == 1, "an unrelated note off released a held note");

    // And finishing a voice index that is not real must not reach outside
    // the array, which is what the sanitizers are here for.
    pool.voiceFinished (-1);
    pool.voiceFinished (Pool::voiceCount);
    pool.voiceFinished (99999);

    check (pool.countHeld() == 1, "an out of range finish disturbed the pool");
}

static void playingItHardKeepsEveryInvariant()
{
    std::printf ("fifty thousand random events, invariants checked after each\n");

    Pool pool;
    pool.reset();

    unsigned rng = 20261004u;
    auto next = [&rng] { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };

    std::vector<int> sounding;
    std::string why;
    int steals = 0;

    for (int step = 0; step < 50000; ++step)
    {
        const unsigned roll = next() % 100;

        if (roll < 45)
        {
            int stolen = -1;
            const int note = 21 + (int) (next() % 88);
            const int v = pool.noteOn (note, stolen);

            if (v >= 0 && stolen >= 0)
                ++steals;
        }
        else if (roll < 80)
        {
            pool.noteOff (21 + (int) (next() % 88));
        }
        else if (roll < 97)
        {
            // A voice finishing its release, which the audio side would
            // report. Only ever for a voice that is actually sounding and
            // no longer held, which is the real condition.
            sounding.clear();

            for (int i = 0; i < Pool::voiceCount; ++i)
                if (pool.voice (i).sounding && ! pool.voice (i).held)
                    sounding.push_back (i);

            if (! sounding.empty())
                pool.voiceFinished (sounding[next() % sounding.size()]);
        }
        else
        {
            pool.allNotesOff();
        }

        if (! invariantsHold (pool, why))
        {
            fail ("after step " + std::to_string (step) + ": " + why);
            return;
        }
    }

    std::printf ("    %d steals along the way, no invariant broken\n", steals);
    check (steals > 100, "the run never filled the pool, so stealing was barely exercised");
}

static void everythingCanBeRecovered()
{
    std::printf ("the pool always comes back, however tangled it got\n");

    Pool pool;
    pool.reset();

    int stolen = -1;
    for (int i = 0; i < 200; ++i)
        pool.noteOn (21 + (i % 88), stolen);

    pool.allNotesOff();

    for (int i = 0; i < Pool::voiceCount; ++i)
        pool.voiceFinished (i);

    check (pool.countSounding() == 0, "voices left sounding after everything finished");
    check (pool.countHeld() == 0, "voices left held after all notes off");

    // And it can be played again from there.
    const int v = pool.noteOn (60, stolen);
    check (v >= 0, "the pool would not give out a voice after being emptied");
    check (stolen == -1, "the first note after emptying stole from something");
}

int main()
{
    std::printf ("voice allocator\n");

    aSingleNoteGoesRoundTheWholeCycle();
    retriggeringReusesTheSameVoice();
    theOldestReleasingVoiceGoesFirst();
    aFullChordStealsTheOldestHeldNote();
    noteOffsForNotesThatWereNeverOnAreHarmless();
    playingItHardKeepsEveryInvariant();
    everythingCanBeRecovered();

    if (failures > 0)
    {
        std::printf ("\n%d voice allocator check(s) failed\n", failures);
        return 1;
    }

    std::printf ("all voice allocator checks passed\n");
    return 0;
}
