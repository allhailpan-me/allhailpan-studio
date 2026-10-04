#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// The arithmetic that gets a MIDI file in and out of this project's beats.
//
// No JUCE in here, so Tests/MidiTimebaseTest.cpp can build it in a second with
// sanitizers on and check the round trip exhaustively. That matters more than
// usual: a part exported a hair off the grid is the kind of thing nobody
// notices until it is opened in another studio next to a drum loop, and a
// length that rounds to nothing is a note that silently does not sound.
//
// Written against the Standard MIDI File 1.0 specification. The division word
// in the MThd chunk, with bit 15 clear, is "the number of delta time 'ticks'
// which make up a quarter-note", so a tick is a fixed fraction of a beat and
// the beat conversion does not involve tempo at all. Tempo only enters for
// the SMPTE form of that word, where a tick is a fixed fraction of a second
// instead, and for the Set Tempo meta event (FF 51 03), whose three bytes are
// microseconds per quarter note.
// ---------------------------------------------------------------------------

/** Ticks per quarter note this studio writes.

    960 is what most studios write and it is not arbitrary: every division
    this project can produce lands on a whole tick. A quarter is 960, and
    halving reaches a 128th note (30) before running out; the channel rack's
    8, 16 and 32 steps to the bar are 480, 240 and 120; eighth note triplets
    are 320 and quintuplets are 192. Septuplets divide exactly at no common
    resolution, so those round like anything else off the grid, by half a tick
    at worst, which at 120 bpm is a third of a millisecond.
*/
static constexpr int kMidiExportPpq = 960;

/** Ticks are written into the file as a delta from the previous event, and the
    writer narrows the running total to a 32 bit int, so the whole arrangement
    has to stay inside that. This cap is a little over a million beats, which
    at any sane tempo is thousands of hours, and keeps the arithmetic below
    from ever being the thing that overflows. */
static constexpr long long kMidiMaxTick = 1LL << 30;

/** A note as this project holds it: beats, and velocity as a fraction. */
struct TimebaseNote
{
    double start    = 0.0;    // beats
    double length   = 0.25;   // beats
    int    note      = 60;
    float  velocity = 0.8f;   // 0..1
};

/** A note as a MIDI file holds it: two events at whole ticks, and a 7 bit
    velocity. The end is where the note off goes, so it is always at least one
    tick past the start: a note on and a note off at the same tick is a note
    that some studios draw and none of them play. */
struct TickNote
{
    long long startTick = 0;
    long long endTick   = 1;
    int       note      = 60;
    int       velocity  = 100;   // 1..127
};

// ---------------------------------------------------------------------------
// Scalars

inline long long midiTicksFromBeats (double beats, int ppq) noexcept
{
    if (! std::isfinite (beats) || ppq <= 0)
        return 0;

    const double ticks = std::floor (beats * (double) ppq + 0.5);
    if (ticks <= 0.0)
        return 0;
    if (ticks >= (double) kMidiMaxTick)
        return kMidiMaxTick;

    return (long long) ticks;
}

inline double midiBeatsFromTicks (long long ticks, int ppq) noexcept
{
    if (ppq <= 0)
        return 0.0;
    return (double) ticks / (double) ppq;
}

/** A note on with velocity 0 is a note off by convention every studio
    follows, so an exported note never carries one: the floor is 1. This is
    the same mapping the engine uses when it hands notes to a plugin, so what
    a file says and what you hear agree. */
inline int midiVelocityToByte (float velocity) noexcept
{
    if (! std::isfinite (velocity))
        return 1;
    const long rounded = std::lround (velocity * 127.0f);
    return (int) std::clamp (rounded, 1L, 127L);
}

inline float midiVelocityFromByte (int byteValue) noexcept
{
    return (float) std::clamp (byteValue, 1, 127) / 127.0f;
}

/** Microseconds per quarter note, for the Set Tempo meta event. The field is
    three bytes, so it cannot describe a tempo below about 3.6 bpm, which no
    project reaches; the clamp is there so a nonsense tempo cannot write a
    value that would be read back as something else entirely. */
inline int midiMicrosecondsPerQuarterNote (double bpm) noexcept
{
    if (! std::isfinite (bpm) || bpm <= 0.0)
        return 500000;        // 120 bpm, the value a file without a tempo means

    const double us = std::floor (60000000.0 / bpm + 0.5);
    return (int) std::clamp (us, 1.0, 16777215.0);
}

inline double midiBpmFromMicrosecondsPerQuarterNote (double microseconds) noexcept
{
    if (! std::isfinite (microseconds) || microseconds <= 0.0)
        return 120.0;
    return 60000000.0 / microseconds;
}

/** Beats for a file whose division word is the SMPTE form, where a tick is a
    fraction of a second rather than of a beat. Rare, but a file written by a
    video tool will be one, and reading its ticks as beats would place a part
    hundreds of bars out. */
inline double midiBeatsFromSmpteTicks (long long ticks, double secondsPerTick, double bpm) noexcept
{
    if (! std::isfinite (secondsPerTick) || ! std::isfinite (bpm) || bpm <= 0.0)
        return 0.0;
    return (double) ticks * secondsPerTick * bpm / 60.0;
}

// ---------------------------------------------------------------------------
// Notes

inline TickNote midiNoteToTicks (const TimebaseNote& n, int ppq) noexcept
{
    TickNote t;
    t.startTick = midiTicksFromBeats (n.start, ppq);

    // The end comes from the absolute end beat rather than from the length,
    // because that is what the file holds: two events, each at its own tick.
    // Taking it from the length instead would let a start that rounded up
    // drag the note off with it.
    const long long end = midiTicksFromBeats (n.start + n.length, ppq);

    t.endTick  = std::max (t.startTick + 1, end);
    t.note     = std::clamp (n.note, 0, 127);
    t.velocity = midiVelocityToByte (n.velocity);
    return t;
}

inline TimebaseNote midiNoteFromTicks (const TickNote& t, int ppq) noexcept
{
    TimebaseNote n;
    n.start    = midiBeatsFromTicks (t.startTick, ppq);
    n.length   = midiBeatsFromTicks (std::max (1LL, t.endTick - t.startTick), ppq);
    n.note     = std::clamp (t.note, 0, 127);
    n.velocity = midiVelocityFromByte (t.velocity);
    return n;
}

/** Puts a set of notes into a shape a MIDI file can actually hold, and says
    how many had to be thrown away.

    One MIDI channel has one note on and one note off per pitch, so two notes
    of the same pitch that overlap cannot both be described: the second note
    on arrives while the first is still sounding, and every reader resolves
    that by ending the first there. A piano roll will happily draw the
    overlap, so rather than write a file that reads back differently from what
    was written, the earlier note is shortened to where the later one starts.
    Two that begin on the same tick are a genuine loss, and counted.

    Running this twice changes nothing, which is what makes the round trip
    stable: a file exported, imported and exported again is the same file.
*/
inline int midiResolveSamePitchOverlaps (std::vector<TickNote>& notes)
{
    // By pitch, then by start, then longest first, so that of two notes
    // beginning on the same tick the one kept is the one that was audible.
    std::stable_sort (notes.begin(), notes.end(), [] (const TickNote& a, const TickNote& b)
    {
        if (a.note != b.note)           return a.note < b.note;
        if (a.startTick != b.startTick) return a.startTick < b.startTick;
        return a.endTick > b.endTick;
    });

    std::vector<TickNote> kept;
    kept.reserve (notes.size());
    int dropped = 0;

    for (const auto& n : notes)
    {
        if (! kept.empty() && kept.back().note == n.note
            && kept.back().startTick == n.startTick)
        {
            ++dropped;
            continue;
        }

        if (! kept.empty() && kept.back().note == n.note)
            kept.back().endTick = std::min (kept.back().endTick, n.startTick);

        kept.push_back (n);
    }

    // Back into playing order, which is the order the events are written in.
    std::stable_sort (kept.begin(), kept.end(), [] (const TickNote& a, const TickNote& b)
    {
        if (a.startTick != b.startTick) return a.startTick < b.startTick;
        return a.note < b.note;
    });

    notes = std::move (kept);
    return dropped;
}

/** Beats to ticks for a whole pattern, resolved so the file holds exactly
    what will be read back out of it. */
inline std::vector<TickNote> midiNotesToTicks (const std::vector<TimebaseNote>& notes,
                                               int ppq, int* droppedOut = nullptr)
{
    std::vector<TickNote> ticks;
    ticks.reserve (notes.size());
    for (const auto& n : notes)
        ticks.push_back (midiNoteToTicks (n, ppq));

    const int dropped = midiResolveSamePitchOverlaps (ticks);
    if (droppedOut != nullptr)
        *droppedOut = dropped;

    return ticks;
}

inline std::vector<TimebaseNote> midiNotesFromTicks (const std::vector<TickNote>& ticks, int ppq)
{
    std::vector<TimebaseNote> notes;
    notes.reserve (ticks.size());
    for (const auto& t : ticks)
        notes.push_back (midiNoteFromTicks (t, ppq));
    return notes;
}

// ---------------------------------------------------------------------------
// Which of a pattern's notes a clip actually plays.
//
// A MIDI clip is a window onto its pattern: the pattern's beat `offset` is
// anchored to the clip's start, and only what falls inside the clip's length
// sounds, with the last note cut off at the clip's end. The engine does this
// when it builds its arrangement snapshot, and the export has to do exactly
// the same thing, or a part written to a file disagrees with the part that was
// heard. That is the oldest hazard in this codebase, so the arithmetic lives
// here where a test can hold the two to the same answer.

struct MidiClipWindow
{
    double start  = 0.0;   // beat the clip begins on
    double offset = 0.0;   // beats into the pattern the clip starts reading at
    double length = 0.0;   // beats the clip plays for

    double origin() const noexcept { return start - offset; }
    double endBeat() const noexcept { return start + length; }
};

/** True when the clip plays this note, filling in where it lands in the
    arrangement and how long it sounds for. A note is included by its start
    alone: one that begins inside the clip is cut off at the clip's end, and
    one that begins before the clip does not sound at all, however far it
    would have run. */
inline bool midiClipPlaysNote (const MidiClipWindow& clip,
                               double noteStart, double noteLength,
                               double& atOut, double& lengthOut) noexcept
{
    const double at = clip.origin() + noteStart;

    if (! (at >= clip.start) || ! (at < clip.endBeat()))
        return false;

    atOut     = at;
    lengthOut = std::min (at + noteLength, clip.endBeat()) - at;
    return true;
}
