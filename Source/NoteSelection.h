#pragma once
#include "MidiTimebase.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

// ---------------------------------------------------------------------------
// Selecting more than one note, and editing the set as a set.
//
// The piano roll could edit exactly one note at a time: one `dragNote`, and
// drag modes for moving, resizing and erasing that one note. So there was no
// way to pick up a chord, transpose a riff, or move eight notes a sixteenth
// later. Every studio checked has done all of that for twenty years.
//
// None of what follows is UI. The hit testing and the mouse plumbing live in
// PianoRollComponent, and they are the part that cannot be tested. This is the
// part that can, and it is the part that would be silently wrong: a group drag
// that clamps each note individually at the top of the keyboard does not
// error, does not crash and does not look broken. It flattens the top of a
// chord into a unison, and whoever did it finds out later, by ear, with no
// idea which gesture did it. That is the failure shape this project keeps
// meeting, so the arithmetic is here, with no JUCE in it, and
// Tests/NoteSelectionTest.cpp is over it.
//
// --- What is published, and what is not -----------------------------------
//
// Six of the ten questions this needed answering have a published answer in a
// manufacturer's own manual. Four do not, and those four are decisions made
// here. They are labelled, because a borrowed authority that is not there is
// worse than an admitted choice.
//
// PUBLISHED.
//
// A marquee selects what it TOUCHES, not what it contains. Apple's own
// general selection chapter, Logic Pro 9 User Manual chapter 8, "Using
// Selection Techniques": "All elements touched, or enclosed, by the selection
// rectangle (shaded area) are selected." Avid says the same of the Grabber
// tool: "if any portion of the rectangle touches a note (either its start or
// end point), the note is included in the selection." Two more require one
// point inside rather than the whole note: Avid's Selector tool wants the
// start point, Steinberg's Score editor wants the notehead. No manual
// anywhere asks for full containment, so intersection is the only convention
// with explicit support.
//
// One Shift both adds and removes. Ableton, Live 12 manual 10.5.2: "You can
// add the Shift modifier to add more notes to your current selection", and
// "You can also remove a single note from your selection by holding down
// Shift and clicking on it." For a Shift modified marquee, Logic 9: "the
// active selection status of the elements is reversed", which is a toggle
// over the rectangle rather than an add. Nobody documents a second modifier
// for toggling, so there is not one here.
//
// A group keeps its relative positions. Steinberg, Moving and Transposing
// Note Events: "All selected note events are moved, maintaining their
// relative positions. Snap is taken into account."
//
// At a limit the WHOLE GROUP STOPS, and breaking the shape is opt in. Apple
// documents this exactly, for velocity: "If the velocity value of any of the
// selected notes reaches the minimum or maximum value (0 or 127), none of the
// notes can be adjusted further. You can override this behavior by pressing
// and holding Option while dragging", and then, naming the consequence rather
// than hiding it, "By overriding, the velocity relationship of this note to
// the other notes is altered." Ardour corroborates from the other side: its
// shortcut list carries a separate "(allow mush)" variant of every transpose
// and velocity command, so the plain command does not mush. The Ardour manual
// never defines the word, so only the command names are cited.
//
// Velocity and length on a group are ADDITIVE, not scaling. Apple: "When you
// edit the velocity of multiple notes, the relative differences in their
// velocity values are retained", and "The relative differences between the
// lengths of the selected notes are maintained." Nobody documents
// multiplicative scaling as a drag behaviour anywhere. Absolute, every note
// to one value, is a separate documented gesture everywhere it exists:
// Apple's Option-Shift, FL Studio's Shift and drag, Studio One's inspector.
// Length has two distinct absolute variants and both are here, because Apple
// publishes both and they diverge the moment the notes have different starts:
// Shift for "the same end point for the selected notes", Option-Shift to
// "make them all the same length".
//
// A duplicate lands immediately after the selection and BECOMES the
// selection. Studio One: "The duplicated Event is always placed after the
// original Event, and it is automatically selected once duplicated", and
// explicitly not at the playhead. FL Studio's Ctrl+B: "The selection, or all
// notes if nothing is selected, will be duplicated to the right of the
// original selection." The second half is the part worth copying: the copy
// becoming the selection is what makes three duplicates in a row build a
// chain rather than a pile.
//
// A moved note wins over what it lands on. Live 12 10.5.4: "If the new note
// overlaps with the beginning of the original note, the original note will be
// overwritten. If the new note overlaps with the end of the original, the
// original note will be shortened." The asymmetry is the rule: it turns on
// whether the covered note's start survives. Ableton is the only studio that
// states this unconditionally; Studio One has the same rule behind a "No
// overlap" switch, Steinberg leaves the overlap and gives you Delete Overlaps
// to resolve it, and four studios say nothing at all. It is followed here
// because a same pitch overlap left in the data is a note that will or will
// not sound depending on how the receiving synth handles a second note on
// before the note off, which is this project's favourite kind of fault:
// present, plausible, and wrong somewhere else.
//
// NOT PUBLISHED, so chosen here.
//
// Whether clicking a note that is ALREADY SELECTED keeps the selection. Not
// one of the eight manuals states it, and a group drag depends on it
// entirely. It is forced by documented procedures that are impossible
// otherwise: Apple's "Select the notes, then drag either edge of one selected
// note", with the relative lengths maintained, needs the other notes still to
// be in the selection. So it is preserved. It is also not collapsed on mouse
// up, because a click to audition a note should not destroy a selection
// somebody spent a minute building.
//
// Which note anchors the snap on a group drag. Nobody publishes it. The
// grabbed note, because it is the only choice that keeps the snap under the
// mouse, and because the clicked note is the documented reference in every
// other group operation (Studio One: "snap the length of all notes to match
// the length of the clicked note"). Earliest in selection is published by
// nobody and would make the same drag behave differently depending on which
// note happened to be grabbed.
//
// Whether a group drag snaps absolutely or relatively. Both are named, first
// class modes and there is no majority. Apple has "Snap Regions to Absolute
// Value", "When you move or edit an item, it snaps exactly to the nearest
// grid position", against "Snap Regions to Relative Value", which keeps "the
// same relative distance from their original grid positions". Steinberg has
// "Grid" against "Grid Relative", where "a moved event keeps its original
// position relative to the grid". Absolute is Apple's and Steinberg's
// default. Relative is Ableton's: "Note movements also snap to an 'offset,'
// based on each note's original placement relative to the grid."
//
// Absolute is the default here, and worth being clear about why, because the
// obvious argument for relative does not survive reading what absolute
// actually does to a group. Neither mode quantises a performance: in both,
// one delta moves every note and the group keeps its internal timing exactly.
// The modes differ only in where the GRABBED note ends up. Absolute puts it
// on a grid line and carries everything else along by the same amount;
// relative moves the whole group by a whole number of grid steps and leaves
// every note, the grabbed one included, at its own distance from the grid.
// So the choice is between placing a group precisely and nudging it without
// touching its relationship to the bar.
//
// Absolute wins the default on two counts that relative does not answer: it
// is what dragging a single note in this roll has always done, so no existing
// gesture changes under somebody's hands, and it is what Apple and Steinberg
// both ship. Relative is offered rather than dropped because it is the right
// answer for moving something played and left off the grid on purpose, which
// is the case absolute handles least well.
//
// What a group at a PITCH limit does. Published for velocity by Apple, not
// for pitch by anybody. The same rule is used, which is where both Apple's
// velocity text and Ardour's command names point.
//
// Reaper is deliberately absent from all of the above. Its user guide PDFs
// truncate before the MIDI editor chapter and its wiki serves a stub, so
// nothing here is sourced from it. Worth knowing for later: Reaper is the one
// studio where marquee and selection behaviour is configurable in
// preferences, so anything remembered about it is likely to be somebody's
// remembered configuration rather than a default.
// ---------------------------------------------------------------------------

namespace NoteSelection
{

/** The note type the rest of the studio already uses, so nothing has to be
    converted to come through here. */
using Note = TimebaseNote;

/** A selection is the indices of the selected notes, sorted and unique.

    Indices rather than pointers or copies because the piano roll edits the
    pattern's own vector in place and an index survives that, and sorted
    because every operation below either walks it or needs a membership test,
    and because two selections built by different routes then compare equal.
    Anything that erases a note has to remap this, which is what `remapAfter`
    exists for: an erase that leaves a stale index behind would have a later
    edit silently move the wrong note. */
using Set = std::vector<size_t>;

/** The lowest velocity a note can be given. Not zero: `MidiTimebase.h` has
    the reason, that "a note on with velocity 0 is a note off by convention
    every studio follows, so an exported note never carries one: the floor is
    1". `midiVelocityFromByte (1)` is exactly this, so a note held at the
    floor here survives a save and a load unchanged. Apple's published rule
    talks about 0 and 127; 0 is not available in a file, so this is the same
    rule at the floor this format actually has. */
inline constexpr float kMinVelocity = 1.0f / 127.0f;
inline constexpr float kMaxVelocity = 1.0f;

/** The shortest note an edit may produce: one export tick, for the reason
    `MidiTimebase.h` gives, that "a note on and a note off at the same tick is
    a note that some studios draw and none of them play". A group length edit
    is the operation most likely to ask for one. */
inline constexpr double kMinLengthBeats = 1.0 / (double) kMidiExportPpq;

/** The lowest and highest pitch a note can be given. */
inline constexpr int kLowestNote  = 0;
inline constexpr int kHighestNote = 127;

//===========================================================================
// The selection as a set.
//===========================================================================

inline bool contains (const Set& sel, size_t index) noexcept
{
    return std::binary_search (sel.begin(), sel.end(), index);
}

inline void add (Set& sel, size_t index)
{
    const auto at = std::lower_bound (sel.begin(), sel.end(), index);
    if (at == sel.end() || *at != index)
        sel.insert (at, index);
}

inline void remove (Set& sel, size_t index)
{
    const auto at = std::lower_bound (sel.begin(), sel.end(), index);
    if (at != sel.end() && *at == index)
        sel.erase (at);
}

/** Shift clicking a note adds it if it is out and removes it if it is in,
    which is the one documented convention: Ableton publishes both halves on
    the same gesture and nobody publishes a second modifier. */
inline void toggle (Set& sel, size_t index)
{
    const auto at = std::lower_bound (sel.begin(), sel.end(), index);
    if (at != sel.end() && *at == index)
        sel.erase (at);
    else
        sel.insert (at, index);
}

inline Set all (size_t noteCount)
{
    Set sel;
    sel.reserve (noteCount);
    for (size_t i = 0; i < noteCount; ++i)
        sel.push_back (i);
    return sel;
}

/** Every note at one pitch. Steinberg: "To select all notes of a certain
    pitch, press Ctrl/Cmd and click on a key in the keyboard display to the
    left." Apple has the same gesture on its keyboard strip, and compares it
    to "selecting all regions on a track by clicking the track name". */
inline Set atPitch (const std::vector<Note>& notes, int pitch)
{
    Set sel;
    for (size_t i = 0; i < notes.size(); ++i)
        if (notes[i].note == pitch)
            sel.push_back (i);
    return sel;
}

//===========================================================================
// The marquee.
//===========================================================================

/** A dragged rectangle, in pattern beats and pitch. Both ranges are stored
    already in order, so a rectangle dragged up and to the left is the same
    rectangle as one dragged down and to the right. */
struct Rect
{
    double startBeat = 0.0;
    double endBeat   = 0.0;
    int    lowNote   = 0;
    int    highNote  = 127;

    static Rect between (double beatA, double beatB, int noteA, int noteB) noexcept
    {
        Rect r;
        r.startBeat = std::min (beatA, beatB);
        r.endBeat   = std::max (beatA, beatB);
        r.lowNote   = std::min (noteA, noteB);
        r.highNote  = std::max (noteA, noteB);
        return r;
    }
};

/** Whether the rectangle catches this note.

    Touching is enough, per Apple and Avid, so this is an overlap test and not
    a containment one. The time comparison is half open at both ends: a
    rectangle that ends exactly where a note starts does not catch it, and one
    that starts exactly where a note ends does not either. No manual publishes
    that edge, so it is a choice, and it is the choice that makes two abutting
    marquees partition the notes between them with none caught twice and none
    missed, which is the property worth having.

    A zero width rectangle is a click, and it still catches the note under the
    point: a note is a span, so a point inside it overlaps it. The pitch range
    is inclusive at both ends because a pitch is a row rather than a span, and
    a one row rectangle has to be able to catch the notes on that row. */
inline bool caught (const Note& n, const Rect& r) noexcept
{
    if (n.note < r.lowNote || n.note > r.highNote)
        return false;

    const double noteEnd = n.start + std::max (0.0, n.length);

    // A zero length note would be caught by nothing under a strict overlap
    // test, so it is treated as the point it is. Notes like that should not
    // exist, since every edit here floors a length at one tick, but a loaded
    // file is not under this header's control.
    if (noteEnd <= n.start)
        return n.start >= r.startBeat && n.start <= r.endBeat;

    return n.start < r.endBeat && noteEnd > r.startBeat;
}

inline Set inRect (const std::vector<Note>& notes, const Rect& r)
{
    Set sel;
    for (size_t i = 0; i < notes.size(); ++i)
        if (caught (notes[i], r))
            sel.push_back (i);
    return sel;
}

/** A plain marquee replaces the selection; a Shift modified one reverses the
    membership of what it caught and leaves everything outside it alone. The
    second is Logic 9's documented rule: with Shift held "the active selection
    status of the elements is reversed". */
inline Set reverseInRect (const Set& sel, const std::vector<Note>& notes, const Rect& r)
{
    Set result = sel;
    for (size_t i = 0; i < notes.size(); ++i)
        if (caught (notes[i], r))
            toggle (result, i);
    return result;
}

//===========================================================================
// Moving a group.
//===========================================================================

enum class Snap
{
    off,        ///< the delta is used as it arrives, which is what a snap defeat modifier asks for
    relative,   ///< the delta is a whole number of grid steps, so an off grid group stays off grid
    absolute    ///< the grabbed note lands on a grid line, and everything else keeps its distance from it
};

/** Where the group is allowed to end up.

    `maxStart` is infinite by default and the piano roll leaves it that way,
    because a clip is a deliberately shorter window onto its pattern and
    dragging a note past the window's end hides it rather than being refused.
    That is what the roll's single note drag already did, and the quantiser
    deliberately refused to grow a clip for the same reason. The bound exists
    so that a caller who does want one has it, and so the test can exercise
    the clamp from both sides. */
struct Limits
{
    double minStart  = 0.0;
    double maxStart  = std::numeric_limits<double>::infinity();
    int    lowNote   = kLowestNote;
    int    highNote  = kHighestNote;
};

struct Request
{
    double beats       = 0.0;    ///< how far the pointer has moved, in beats
    int    semitones   = 0;      ///< how far it has moved in pitch
    double grid        = 0.25;   ///< beats per snap step
    Snap   snap        = Snap::absolute;
    double anchorStart = 0.0;    ///< the grabbed note's start, for absolute snapping
};

/** What will actually be applied.

    The two flags mean "the limits cut this", not "this is not what was
    asked": snapping also changes the delta, and reporting that as a limit
    would make the flags fire on almost every drag. So the time flag is
    computed by working out what the delta would have been with no limits at
    all and comparing, which is the only way to tell the two apart.

    Nothing in the studio reads them yet. They are here because a drag that
    has stopped moving because one note is against the top of the keyboard
    looks identical to a drag that has stopped for no reason, and whoever puts
    that on screen should not have to work the condition out again. Said
    plainly rather than implied, because a flag whose comment claims a use it
    does not have is how a wrong flag survives: the first version of the time
    one was wrong in 42 per cent of clamped moves and nothing noticed. */
struct Move
{
    double beats        = 0.0;
    int    semitones    = 0;
    bool   beatsLimited = false;
    bool   pitchLimited = false;
};

/** The largest multiple of the grid, in the allowed progression, no further
    from zero than `raw`.

    Both snap modes pick a delta out of an arithmetic progression of step
    `grid`. Relative snapping uses the progression through zero, so any
    multiple of the grid is allowed. Absolute snapping uses the progression
    that puts the anchor on a grid line, which is the same progression shifted
    by the anchor's own distance from the grid, and that shift is `phase`.
    Expressing both as one progression is what keeps the clamping identical
    between them, which matters because the clamp is the part that has to
    preserve the group's shape.

    The candidate nearest the request is taken and then pulled inside
    `[lo, hi]`, rather than the raw delta being clamped and then snapped. That
    order is the one that cannot deform the group: clamping afterwards could
    leave a delta that is not on the progression at all, and then a group
    dragged against the bottom of the pattern would come back off the grid. */
inline double snappedDeltaWithin (double raw, double grid, double phase,
                                  double lo, double hi) noexcept
{
    if (! (grid > 0.0) || ! std::isfinite (grid) || ! std::isfinite (raw))
        return std::clamp (0.0, std::min (lo, hi), std::max (lo, hi));

    const auto deltaFor = [grid, phase] (double k) noexcept
    {
        return (k + phase) * grid;
    };

    double kMin = std::ceil  (lo / grid - phase);
    double kMax = std::floor (hi / grid - phase);

    // ceil and floor of a quotient can land a step outside the range they
    // were meant to describe, because the quotient was rounded on the way in.
    // Correcting by testing the candidate itself is exact, where a tolerance
    // on the bound would only move the problem: a delta that is 1e-12 past a
    // limit still breaks the invariant the limit is there to state.
    if (deltaFor (kMin) < lo) kMin += 1.0;
    if (deltaFor (kMax) > hi) kMax -= 1.0;

    if (kMin > kMax)
    {
        // No position on the progression keeps the group inside its limits,
        // which absolute snapping can reach: the only deltas it offers land
        // the anchor on a grid line, and there may be no grid line the group
        // can reach. Standing still is the answer, and standing still is
        // always allowed, because the caller computed the limits so that zero
        // is in range.
        return 0.0;
    }

    const double wanted = std::clamp (std::round (raw / grid - phase), kMin, kMax);
    return deltaFor (wanted);
}

/** The delta that will be applied, with the group's shape preserved.

    The whole point is in one line: the limits are turned into limits on the
    DELTA, once, over the whole selection, and then one delta moves every
    note. Clamping each note as it is written is the fault this header exists
    to prevent, and it is the one Apple names out loud for velocity and Ardour
    gives a separate command name to.

    A limit only ever stops a move; it never forces one. Each bound is taken
    against zero, so standing still is always an allowed answer and a group
    that somehow starts outside its limits is free to move back inside but is
    never dragged there. The alternative was tried first and is worse than it
    sounds: with the bounds taken from the group alone, a selection whose top
    note sat above the ceiling came out transposed down by however far it was
    over, as the answer to a request to move it sideways. So the invariant is
    the honest one rather than the tidy one: a group that was inside its
    limits stays inside them, and no note is ever pushed further outside a
    limit than it already was. */
inline Move planMove (const std::vector<Note>& notes, const Set& sel,
                      const Request& request, const Limits& limits)
{
    Move move;

    if (sel.empty())
        return move;

    double lowestStart  = std::numeric_limits<double>::infinity();
    double highestStart = -std::numeric_limits<double>::infinity();
    int    lowestNote   = kHighestNote;
    int    highestNote  = kLowestNote;
    size_t counted      = 0;

    for (const auto index : sel)
    {
        if (index >= notes.size())
            continue;        // a stale index is ignored rather than trusted

        const auto& n = notes[index];
        lowestStart  = std::min (lowestStart,  n.start);
        highestStart = std::max (highestStart, n.start);
        lowestNote   = std::min (lowestNote,   n.note);
        highestNote  = std::max (highestNote,  n.note);
        ++counted;
    }

    if (counted == 0)
        return move;

    // Pitch. The requested shift is cut to what leaves every member in range,
    // so a chord whose top note is already at 127 does not move at all rather
    // than collapsing onto itself.
    {
        const int lo = std::min (0, limits.lowNote  - lowestNote);
        const int hi = std::max (0, limits.highNote - highestNote);
        const int wanted = request.semitones;

        move.semitones = std::clamp (wanted, lo, hi);
        move.pitchLimited = (move.semitones != wanted);
    }

    // Time. Same shape, through the snap progression so that the delta that
    // survives the clamp is still a snapped one.
    {
        const double lo = std::min (0.0, limits.minStart - lowestStart);
        const double hi = std::max (0.0, limits.maxStart - highestStart);
        const double wanted = std::isfinite (request.beats) ? request.beats : 0.0;

        if (request.snap == Snap::off || ! (request.grid > 0.0))
        {
            move.beats = std::clamp (wanted, lo, hi);
            move.beatsLimited = std::abs (move.beats - wanted) > 1e-12;
        }
        else
        {
            const double phase = request.snap == Snap::absolute
                                   ? -request.anchorStart / request.grid
                                   : 0.0;
            move.beats = snappedDeltaWithin (wanted, request.grid, phase, lo, hi);

            // Against what the same request would have done with no limits,
            // so that snapping is not mistaken for a limit. Comparing with
            // the request itself instead reported a limit on nearly every
            // drag and missed the real ones: the answer is the clamped end of
            // the grid progression, which can sit most of a grid step inside
            // the bound and so does not look clamped at all.
            constexpr double unbounded = 1e300;
            const double free = snappedDeltaWithin (wanted, request.grid, phase, -unbounded, unbounded);
            move.beatsLimited = std::abs (move.beats - free) > 1e-12;
        }
    }

    return move;
}

inline void applyMove (std::vector<Note>& notes, const Set& sel, const Move& move)
{
    for (const auto index : sel)
    {
        if (index >= notes.size())
            continue;

        auto& n = notes[index];
        n.start += move.beats;
        n.note  += move.semitones;
    }
}

/** Transposing is a move with no time in it, so it goes through the same
    clamp and gets the same group-stops behaviour for free. A semitone and an
    octave are the published increments and the agreement on them is the
    strongest in the whole of this research: Steinberg ("use the Up
    Arrow/Down Arrow keys", and "To transpose note events in steps of one
    octave, press Shift"), Bitwig, Ableton and Ardour all publish the pair and
    all agree. Apple and FL Studio publish no key increments at all, only
    dragging, so they are not cited for it. */
inline Move transpose (const std::vector<Note>& notes, const Set& sel,
                       int semitones, const Limits& limits)
{
    Request r;
    r.semitones = semitones;
    r.snap      = Snap::off;
    return planMove (notes, sel, r, limits);
}

//===========================================================================
// Velocity on a group.
//===========================================================================

enum class VelocityMode
{
    group,     ///< one offset for all, cut so nothing passes a limit: the documented default
    perNote,   ///< one offset for all, each clamped on its own: Apple's Option, which alters the relationship on purpose
    absolute   ///< every note to one value: Apple's Option-Shift
};

/** Apply a velocity edit.

    In `group` and `perNote` the amount is an offset; in `absolute` it is the
    value every selected note is set to. Additive rather than scaling because
    that is what Apple publishes and because scaling does not round trip: a
    pattern scaled by 1.2 and then by 1/1.2 does not come back, which is
    exactly the sort of quiet damage an edit must not do. */
inline void applyVelocity (std::vector<Note>& notes, const Set& sel,
                           double amount, VelocityMode mode)
{
    if (sel.empty() || ! std::isfinite (amount))
        return;

    if (mode == VelocityMode::absolute)
    {
        const float value = std::clamp ((float) amount, kMinVelocity, kMaxVelocity);
        for (const auto index : sel)
            if (index < notes.size())
                notes[index].velocity = value;
        return;
    }

    if (mode == VelocityMode::perNote)
    {
        for (const auto index : sel)
            if (index < notes.size())
                notes[index].velocity = std::clamp ((float) ((double) notes[index].velocity + amount),
                                                    kMinVelocity, kMaxVelocity);
        return;
    }

    // The group case. Cut the offset to what leaves every member in range, so
    // the differences between them survive, which is the published behaviour
    // and the reason a velocity ramp somebody played does not flatten the
    // first time the loudest note reaches the top.
    double lowest  = (double) kMaxVelocity;
    double highest = (double) kMinVelocity;
    bool   any     = false;

    for (const auto index : sel)
    {
        if (index >= notes.size())
            continue;
        const double v = (double) notes[index].velocity;
        lowest  = std::min (lowest, v);
        highest = std::max (highest, v);
        any = true;
    }

    if (! any)
        return;

    // Taken against zero for the same reason `planMove` does it: a limit
    // stops an edit and never forces one, so a note that somehow arrives
    // outside the range a file can hold does not drag the rest of the group
    // with it.
    const double lo = std::min (0.0, (double) kMinVelocity - lowest);
    const double hi = std::max (0.0, (double) kMaxVelocity - highest);
    const double delta = std::clamp (amount, lo, hi);

    // And nothing is clamped per note on the way out. The clamp above already
    // keeps every note that was in range in range, and clamping the write as
    // well would lift a note that arrived from a file below the floor while
    // leaving the others alone, which is the one thing this mode must not do:
    // its whole job is to keep the differences. A note that was already out
    // of range stays as it is. This header did not put it there and does not
    // make it worse, which is the same rule `planMove` and `applyLength`
    // follow.
    for (const auto index : sel)
        if (index < notes.size())
            notes[index].velocity = (float) ((double) notes[index].velocity + delta);
}

//===========================================================================
// Length on a group.
//===========================================================================

enum class LengthMode
{
    group,       ///< one offset for all, cut so nothing goes under a tick: Apple's relative differences
    sameLength,  ///< every note to one length: Apple's Option-Shift
    sameEnd      ///< every note to one end position: Apple's Shift
};

/** Apply a length edit.

    In `group` the amount is an offset in beats. In `sameLength` it is the
    length every selected note is given. In `sameEnd` it is the beat every
    selected note is made to end on.

    The last two are different operations and both are published, which is
    easy to miss: they agree whenever the notes start together and diverge the
    moment they do not. Apple carries both, as "Adjust selected notes to the
    same length" and "Adjust ends of all selected notes to the same
    position", and so does Studio One with different keys.

    `group` gets the same group-stops clamp as pitch and velocity. Apple
    publishes that rule for velocity only, so extending it to length is this
    project's consistency rather than Apple's text, and it is the right way
    round: the alternative flattens the short notes in a group onto the
    minimum while the long ones keep shrinking, which is the chord-collapse
    fault in another parameter. The two absolute modes clamp per note by
    definition, because being told to make everything one length is being
    told to destroy the differences. */
inline void applyLength (std::vector<Note>& notes, const Set& sel,
                         double amount, LengthMode mode,
                         double minLength = kMinLengthBeats)
{
    if (sel.empty() || ! std::isfinite (amount))
        return;

    const double floorLength = std::max (minLength, 0.0);

    if (mode == LengthMode::sameLength)
    {
        const double value = std::max (floorLength, amount);
        for (const auto index : sel)
            if (index < notes.size())
                notes[index].length = value;
        return;
    }

    if (mode == LengthMode::sameEnd)
    {
        for (const auto index : sel)
            if (index < notes.size())
                notes[index].length = std::max (floorLength, amount - notes[index].start);
        return;
    }

    double shortest = std::numeric_limits<double>::infinity();
    bool   any      = false;

    for (const auto index : sel)
    {
        if (index >= notes.size())
            continue;
        shortest = std::min (shortest, notes[index].length);
        any = true;
    }

    if (! any)
        return;

    // The shortest note decides how far the group can shrink, and the bound is
    // taken against zero for the same reason `planMove` does it: a limit
    // stops an edit and never forces one. Without that, a note arriving from
    // a file already shorter than a tick would make a request to SHORTEN the
    // group lengthen it instead, and at the first instant of a resize drag,
    // where the requested change is nothing, every selected note would grow
    // before the mouse had moved. That is the same fault `planMove`'s comment
    // records having tried and rejected, in another parameter.
    //
    // So nothing here is floored per note either. The group clamp already
    // guarantees the floor for every note that was above it, and flooring the
    // write as well would lift a note that arrived below it and break the
    // differences this mode exists to keep. A note that was already too short
    // stays as it is: this header did not make it and does not make it worse.
    const double delta = std::max (amount, std::min (0.0, floorLength - shortest));

    for (const auto index : sel)
        if (index < notes.size())
            notes[index].length += delta;
}

//===========================================================================
// Erasing, and keeping the selection honest across it.
//===========================================================================

/** What a selection index becomes once `erased` has been removed from the
    note list. A stale index is the fault that makes a later edit move a note
    nobody picked, so every erase in here goes through this. */
inline Set remapAfter (const Set& sel, const Set& erased)
{
    Set result;
    result.reserve (sel.size());

    for (const auto index : sel)
    {
        if (contains (erased, index))
            continue;        // it is gone, so it cannot be selected

        const auto below = (size_t) std::distance (
            erased.begin(), std::lower_bound (erased.begin(), erased.end(), index));
        result.push_back (index - below);
    }

    return result;
}

/** Remove the selected notes. The selection is empty afterwards, because
    every one of its members has gone. */
inline void eraseSelected (std::vector<Note>& notes, Set& sel)
{
    if (sel.empty())
        return;

    std::vector<Note> kept;
    kept.reserve (notes.size());

    for (size_t i = 0; i < notes.size(); ++i)
        if (! contains (sel, i))
            kept.push_back (notes[i]);

    notes = std::move (kept);
    sel.clear();
}

//===========================================================================
// Duplicating.
//===========================================================================

/** How far along the selection reaches: from the earliest start to the latest
    end. This is the distance a duplicate is placed by, per Studio One's "The
    duplicated Event is always placed after the original Event" and FL
    Studio's "duplicated to the right of the original selection". */
inline double extentBeats (const std::vector<Note>& notes, const Set& sel)
{
    double lowest  = std::numeric_limits<double>::infinity();
    double highest = -std::numeric_limits<double>::infinity();
    bool   any     = false;

    for (const auto index : sel)
    {
        if (index >= notes.size())
            continue;
        const auto& n = notes[index];
        lowest  = std::min (lowest, n.start);
        highest = std::max (highest, n.start + std::max (0.0, n.length));
        any = true;
    }

    return any ? std::max (0.0, highest - lowest) : 0.0;
}

/** Copy the selection one selection-length later and return the copies, which
    the caller makes the new selection: Studio One again, "it is automatically
    selected once duplicated". That is what makes three duplicates in a row
    lay three copies end to end instead of piling them in one place.

    Pitches are untouched, and the copies are appended, so no existing index
    moves and nothing else has to be remapped. */
inline Set duplicate (std::vector<Note>& notes, const Set& sel)
{
    const double offset = extentBeats (notes, sel);

    Set copies;
    copies.reserve (sel.size());

    // The size before anything is appended, because the list grows inside the
    // loop. Testing against `notes.size()` as it goes would let a stale index
    // past the end become valid as soon as the first copy landed, and the
    // copies would start being copied.
    const size_t original = notes.size();

    for (const auto index : sel)
    {
        if (index >= original)
            continue;

        Note copy = notes[index];
        copy.start += offset;
        copies.push_back (notes.size());
        notes.push_back (copy);
    }

    return copies;
}

//===========================================================================
// What a moved note does to what it lands on.
//===========================================================================

struct OverlapReport
{
    int deleted = 0;
    int trimmed = 0;
};

/** Resolve same pitch overlaps between the selection and everything else,
    following Ableton's published rule: a moved note covering an existing
    note's start overwrites it, and a moved note covering only its tail
    shortens it.

    Three things about the shape of this, each of which is a fault if it is
    got wrong.

    It only ever touches notes that are NOT selected. Resolving inside the
    selection would eat a chord's own voicing, and overlaps that were already
    in the group were put there on purpose.

    It is called once, when the gesture commits, and never while the pointer
    is moving. Resolving as a drag proceeds destroys every note the group
    passed over on its way, and an undo of the move does not bring them back
    because they were not there when it finished.

    Deleting a note moves every index after it, so the selection is remapped
    rather than left to point at whatever slid into the gap. */
inline OverlapReport resolveOverlaps (std::vector<Note>& notes, Set& sel,
                                      double minLength = kMinLengthBeats)
{
    OverlapReport report;

    if (sel.empty())
        return report;

    const double floorLength = std::max (minLength, 0.0);

    Set doomed;
    std::vector<std::pair<size_t, double>> shortened;

    for (size_t other = 0; other < notes.size(); ++other)
    {
        if (contains (sel, other))
            continue;

        const auto& victim = notes[other];
        const double victimEnd = victim.start + std::max (0.0, victim.length);

        double earliestCut = std::numeric_limits<double>::infinity();

        for (const auto index : sel)
        {
            if (index >= notes.size())
                continue;

            const auto& mover = notes[index];
            if (mover.note != victim.note)
                continue;

            const double moverEnd = mover.start + std::max (0.0, mover.length);

            // The start is covered, so the note is gone. Half open at the
            // mover's end, so a note starting exactly where the mover stops
            // survives: that is the same convention the marquee uses and it
            // is what lets two notes sit back to back.
            if (mover.start <= victim.start && victim.start < moverEnd)
            {
                add (doomed, other);
                earliestCut = std::numeric_limits<double>::infinity();
                break;
            }

            // Only the tail is covered, so the note is shortened to where the
            // mover begins. The earliest such mover wins, since a later one
            // is already inside the part being cut away.
            if (mover.start > victim.start && mover.start < victimEnd)
                earliestCut = std::min (earliestCut, mover.start);
        }

        if (std::isfinite (earliestCut) && ! contains (doomed, other))
        {
            const double length = earliestCut - victim.start;

            // A cut that would leave less than a note behind is a deletion,
            // not a trim. Leaving a one tick stub would be a note nobody can
            // see and something will still play.
            if (length < floorLength)
                add (doomed, other);
            else
                shortened.emplace_back (other, length);
        }
    }

    for (const auto& [index, length] : shortened)
    {
        notes[index].length = length;
        ++report.trimmed;
    }

    if (! doomed.empty())
    {
        std::vector<Note> kept;
        kept.reserve (notes.size());

        for (size_t i = 0; i < notes.size(); ++i)
            if (! contains (doomed, i))
                kept.push_back (notes[i]);

        notes = std::move (kept);
        sel = remapAfter (sel, doomed);
        report.deleted = (int) doomed.size();
    }

    return report;
}

} // namespace NoteSelection
