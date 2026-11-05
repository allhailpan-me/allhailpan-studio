// ---------------------------------------------------------------------------
// Tests/NoteSelectionTest.cpp
//
// What Source/NoteSelection.h exists to produce, asserted as properties
// rather than as a table of hand picked cases.
//
// The property that matters most is one line: a group edit does not deform
// the group. Every pairwise difference in start, in pitch, in velocity and in
// length is unchanged after a move, a transpose, a velocity edit or a length
// edit, whatever was asked for and however hard the limits bite. That single
// assertion kills every per note clamping bug at once, including the one that
// only appears when a single member of a ten note chord happens to sit at
// pitch 127, which is the version of this fault that would otherwise reach a
// release.
//
// Two things about the generators, both learned from what the deliberate
// breakages of this file did and did not survive.
//
// A generator that only produces comfortable cases tests nothing. Rectangles
// drawn generously around whole notes cannot tell intersection from
// containment, so the marquee generator counts how many of its cases are
// partial overlaps and fails if there are not plenty: a test that is secretly
// vacuous reports a strength it does not have. The same goes for the clamps,
// so the move generator deliberately plants notes against the limits, and for
// snapping, where a group that is already on the grid cannot distinguish
// relative from absolute snapping or detect per note snapping at all.
//
// And the discriminating property for additive against multiplicative is the
// difference, not the round trip. A float velocity scaled by 1.2 and then by
// 1/1.2 comes back to within float epsilon, so a round trip proves nothing
// here; a difference between two velocities survives an offset and does not
// survive a scale.
//
// --- Two deliberate breakages survive on purpose -------------------------
//
// Written down so nobody goes looking for the test that should have caught
// them. Both were checked rather than assumed.
//
// Clamping each note inside `applyMove` produces identical output, which is
// worth saying out loud because it is the headline fault this whole header
// exists to prevent. `planMove` has already cut the delta to one that keeps
// every member in range, so a clamp afterwards cannot bite. The meaningful
// versions of that mutation live in `planMove` itself and are killed hard:
// dropping the bound against zero on the pitch ceiling fails over twenty
// thousand checks here. The per note clamps that CAN bite, the ones on the
// way out of a velocity or a length edit, are caught, because a note arriving
// from a file outside the range is planted for exactly that.
//
// Swapping `lower_bound` for `upper_bound` in `remapAfter` is a no-op. Any
// index present in the erased set is skipped by the `contains` guard one line
// above, and for every index that reaches the count the two bounds agree.
//
// Deleting the correction on the quotient's rounding inside
// `snappedDeltaWithin` is very nearly a no-op. Measured, it fires zero times
// in three million realistic cases and seven times in five million
// adversarial ones, with the phase pushed out to a billion and the grid down
// to two to the minus twenty. It stays rather than being deleted because what
// would have to be trusted instead is `ceil` of a rounded quotient, and a
// delta a fraction past a limit still breaks the invariant the limit states.
// ---------------------------------------------------------------------------

#include "NoteSelection.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace
{

int failures = 0;

void check (bool condition, const std::string& what)
{
    if (! condition)
    {
        std::printf ("  FAIL  %s\n", what.c_str());
        ++failures;
    }
}

void checkClose (double a, double b, double tolerance, const std::string& what)
{
    if (! (std::abs (a - b) <= tolerance))
    {
        std::printf ("  FAIL  %s (%.17g against %.17g, tolerance %.3g)\n",
                     what.c_str(), a, b, tolerance);
        ++failures;
    }
}

/** The file's own generator rather than <random>, for the reason
    PanOneBreed.h already gives: the standard distributions are not specified
    exactly and give different sequences on MSVC, so a seed that reproduces a
    failure here has to reproduce it everywhere. */
struct Rng
{
    std::uint64_t state;

    explicit Rng (std::uint64_t seed) : state (seed) {}

    std::uint64_t next() noexcept
    {
        state += 0x9e3779b97f4a7c15ull;
        auto z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    double unit() noexcept { return (double) (next() >> 11) / 9007199254740992.0; }

    double between (double lo, double hi) noexcept { return lo + unit() * (hi - lo); }

    int intBetween (int lo, int hi) noexcept
    {
        if (hi <= lo)
            return lo;
        return lo + (int) (next() % (std::uint64_t) (hi - lo + 1));
    }
};

using NoteSelection::Note;
using NoteSelection::Set;

std::vector<Note> randomNotes (Rng& rng, size_t count, double maxBeat = 16.0)
{
    std::vector<Note> notes;
    notes.reserve (count);

    for (size_t i = 0; i < count; ++i)
    {
        Note n;
        n.start    = rng.between (0.0, maxBeat);
        n.length   = rng.between (0.05, 2.0);
        n.note     = rng.intBetween (0, 127);
        n.velocity = (float) rng.between (0.05, 0.95);
        notes.push_back (n);
    }

    return notes;
}

Set randomSelection (Rng& rng, size_t noteCount)
{
    Set sel;
    for (size_t i = 0; i < noteCount; ++i)
        if (rng.unit() < 0.5)
            sel.push_back (i);

    if (sel.empty() && noteCount > 0)
        sel.push_back (0);

    return sel;
}

bool isSortedUnique (const Set& sel)
{
    for (size_t i = 1; i < sel.size(); ++i)
        if (sel[i] <= sel[i - 1])
            return false;
    return true;
}

//===========================================================================
// 1. The marquee catches what it touches.
//===========================================================================

void testMarqueeIsIntersection()
{
    std::printf ("marquee: touching is enough, and the edges are half open\n");

    Rng rng (0x5eed0001ull);

    int partialCases = 0, containedCases = 0, missCases = 0;

    for (int trial = 0; trial < 40000; ++trial)
    {
        Note n;
        n.start  = rng.between (0.0, 8.0);
        // A zero length note cannot come from any edit in the header, which
        // floors every length at a tick, but it can come from a file. Without
        // one in the generator the branch that handles it is never evaluated
        // and deleting it goes unnoticed, which would leave such a note
        // catchable by no marquee at all and so impossible to select or
        // delete.
        n.length = rng.unit() < 0.08 ? 0.0 : rng.between (0.05, 2.0);
        n.note   = rng.intBetween (0, 127);

        const double noteEnd = n.start + n.length;

        // Rectangles are deliberately drawn so that many of them cut a note
        // rather than surrounding it. Without this the test cannot tell
        // intersection from containment and would pass either.
        const double a = rng.between (n.start - 1.0, noteEnd + 1.0);
        const double b = rng.between (n.start - 1.0, noteEnd + 1.0);
        const int    p = rng.intBetween (0, 127);
        const int    q = rng.intBetween (0, 127);

        const auto r = NoteSelection::Rect::between (a, b, p, q);

        const bool pitchIn = n.note >= r.lowNote && n.note <= r.highNote;
        const bool timeIn  = n.length > 0.0
                               ? (n.start < r.endBeat && noteEnd > r.startBeat)
                               : (n.start >= r.startBeat && n.start <= r.endBeat);
        const bool want    = pitchIn && timeIn;

        check (NoteSelection::caught (n, r) == want, "caught() is overlap, not containment");

        if (want)
        {
            const bool wholly = n.start >= r.startBeat && noteEnd <= r.endBeat;
            if (wholly) ++containedCases; else ++partialCases;
        }
        else
        {
            ++missCases;
        }
    }

    // The generator has to have produced plenty of each kind, or the assertion
    // above was only ever looking at easy cases.
    check (partialCases  > 2000, "the generator produced partial overlaps");
    check (containedCases > 2000, "the generator produced wholly contained notes");
    check (missCases     > 2000, "the generator produced misses");

    // The half open edges, stated directly. No manual publishes these, so
    // they are pinned here as the choice they are.
    Note n;
    n.start = 4.0; n.length = 1.0; n.note = 60;

    check (! NoteSelection::caught (n, NoteSelection::Rect::between (2.0, 4.0, 0, 127)),
           "a rectangle ending exactly at a note's start catches nothing");
    check (! NoteSelection::caught (n, NoteSelection::Rect::between (5.0, 7.0, 0, 127)),
           "a rectangle starting exactly at a note's end catches nothing");
    check (NoteSelection::caught (n, NoteSelection::Rect::between (2.0, 4.001, 0, 127)),
           "a rectangle reaching just past a note's start catches it");

    // A zero length note. Planting them in the generator is not enough on its
    // own: inside a rectangle, the span test and the point test agree, so
    // they only come apart at an exact edge, and random doubles never land on
    // one. These are the cases that distinguish them, and without the branch
    // a zero length note loaded from a file could be caught by no rectangle
    // at all and so could never be selected or deleted.
    Note zero;
    zero.start = 4.0; zero.length = 0.0; zero.note = 60;

    check (NoteSelection::caught (zero, NoteSelection::Rect::between (4.0, 4.0, 60, 60)),
           "a click exactly on a zero length note catches it");
    check (NoteSelection::caught (zero, NoteSelection::Rect::between (4.0, 6.0, 0, 127)),
           "a rectangle starting exactly on a zero length note catches it");
    check (NoteSelection::caught (zero, NoteSelection::Rect::between (2.0, 4.0, 0, 127)),
           "a rectangle ending exactly on a zero length note catches it");
    check (! NoteSelection::caught (zero, NoteSelection::Rect::between (4.5, 6.0, 0, 127)),
           "and a rectangle past it catches nothing");

    // A pitch is a row rather than a span, so a one row rectangle has to work.
    check (NoteSelection::caught (n, NoteSelection::Rect::between (4.5, 4.5, 60, 60)),
           "a click inside a note catches it");
    check (! NoteSelection::caught (n, NoteSelection::Rect::between (4.5, 4.5, 61, 61)),
           "a click one row above a note catches nothing");

    // Two abutting rectangles partition the notes: that is the property the
    // half open choice was made for.
    Rng rng2 (0x5eed0002ull);
    for (int trial = 0; trial < 2000; ++trial)
    {
        const auto notes = randomNotes (rng2, 20, 8.0);
        const double split = rng2.between (0.0, 8.0);

        const auto left  = NoteSelection::inRect (notes, NoteSelection::Rect::between (-1.0, split, 0, 127));
        const auto right = NoteSelection::inRect (notes, NoteSelection::Rect::between (split, 9.0, 0, 127));

        for (size_t i = 0; i < notes.size(); ++i)
        {
            const bool inLeft  = NoteSelection::contains (left, i);
            const bool inRight = NoteSelection::contains (right, i);

            // A note spanning the split is legitimately in both, because it is
            // touched by both. What must not happen is a note in neither.
            check (inLeft || inRight, "abutting rectangles miss no note");

            if (inLeft && inRight)
                check (notes[i].start < split && notes[i].start + notes[i].length > split,
                       "a note in both rectangles really does span the split");
        }
    }
}

//===========================================================================
// 2. The selection as a set.
//===========================================================================

void testSelectionAlgebra()
{
    std::printf ("selection: one Shift adds and removes, and nothing else moves\n");

    Rng rng (0x5eed0003ull);

    for (int trial = 0; trial < 20000; ++trial)
    {
        const size_t count = (size_t) rng.intBetween (1, 24);
        auto sel = randomSelection (rng, count);
        const auto before = sel;
        const size_t index = (size_t) rng.intBetween (0, (int) count - 1);

        NoteSelection::toggle (sel, index);
        check (isSortedUnique (sel), "a toggled selection stays sorted and unique");
        check (NoteSelection::contains (sel, index) != NoteSelection::contains (before, index),
               "toggle changes the clicked note's membership");

        for (size_t i = 0; i < count; ++i)
            if (i != index)
                check (NoteSelection::contains (sel, i) == NoteSelection::contains (before, i),
                       "toggle leaves every other note's membership alone");

        NoteSelection::toggle (sel, index);
        check (sel == before, "toggling the same note twice is the identity");

        // add and remove are idempotent, which is what makes a drag that
        // re-reports the same note harmless.
        NoteSelection::add (sel, index);
        const auto added = sel;
        NoteSelection::add (sel, index);
        check (sel == added, "add is idempotent");
        NoteSelection::remove (sel, index);
        const auto removed = sel;
        NoteSelection::remove (sel, index);
        check (sel == removed, "remove is idempotent");
    }

    // A Shift modified marquee reverses membership inside the rectangle only.
    Rng rng2 (0x5eed0004ull);
    for (int trial = 0; trial < 5000; ++trial)
    {
        const auto notes = randomNotes (rng2, 16, 8.0);
        const auto before = randomSelection (rng2, notes.size());
        const auto r = NoteSelection::Rect::between (rng2.between (0.0, 8.0), rng2.between (0.0, 8.0),
                                                     rng2.intBetween (0, 127), rng2.intBetween (0, 127));

        const auto after = NoteSelection::reverseInRect (before, notes, r);
        check (isSortedUnique (after), "a reversed selection stays sorted and unique");

        for (size_t i = 0; i < notes.size(); ++i)
        {
            const bool inside = NoteSelection::caught (notes[i], r);
            const bool was = NoteSelection::contains (before, i);
            const bool is  = NoteSelection::contains (after, i);
            check (inside ? (is != was) : (is == was),
                   "a Shift marquee reverses inside the rectangle and only there");
        }
    }

    check (NoteSelection::all (5) == Set { 0, 1, 2, 3, 4 }, "select all takes every note");
    check (NoteSelection::all (0).empty(), "select all of nothing is nothing");

    const std::vector<Note> pitched { { 0.0, 1.0, 60, 0.8f }, { 1.0, 1.0, 62, 0.8f },
                                      { 2.0, 1.0, 60, 0.8f } };
    check (NoteSelection::atPitch (pitched, 60) == Set { 0, 2 }, "every note at one pitch");
    check (NoteSelection::atPitch (pitched, 61).empty(), "no note at an unused pitch");
}

//===========================================================================
// 3. Erasing, and indices that still mean what they meant.
//===========================================================================

void testRemapAfterErase()
{
    std::printf ("erase: a surviving index still points at the same note\n");

    Rng rng (0x5eed0005ull);

    for (int trial = 0; trial < 20000; ++trial)
    {
        const size_t count = (size_t) rng.intBetween (1, 20);
        auto notes = randomNotes (rng, count, 16.0);

        // A tag that identifies each note through the erase, so the check is
        // about identity rather than about arithmetic agreeing with itself.
        for (size_t i = 0; i < count; ++i)
            notes[i].start = (double) i;

        const auto doomed = randomSelection (rng, count);
        auto keep = randomSelection (rng, count);

        // Which notes should survive in the selection, by identity.
        std::vector<double> expected;
        for (const auto index : keep)
            if (! NoteSelection::contains (doomed, index))
                expected.push_back (notes[index].start);

        const auto remapped = NoteSelection::remapAfter (keep, doomed);

        std::vector<Note> survivors;
        for (size_t i = 0; i < count; ++i)
            if (! NoteSelection::contains (doomed, i))
                survivors.push_back (notes[i]);

        check (remapped.size() == expected.size(), "remap drops exactly the erased notes");
        check (isSortedUnique (remapped) || remapped.empty(), "a remapped selection stays sorted and unique");

        for (size_t i = 0; i < remapped.size() && i < expected.size(); ++i)
        {
            check (remapped[i] < survivors.size(), "a remapped index is in range");
            if (remapped[i] < survivors.size())
                checkClose (survivors[remapped[i]].start, expected[i], 0.0,
                            "a remapped index points at the note it pointed at before");
        }
    }

    // eraseSelected removes exactly the selection and leaves nothing selected.
    std::vector<Note> notes { { 0.0, 1.0, 60, 0.8f }, { 1.0, 1.0, 61, 0.8f },
                              { 2.0, 1.0, 62, 0.8f }, { 3.0, 1.0, 63, 0.8f } };
    Set sel { 1, 3 };
    NoteSelection::eraseSelected (notes, sel);
    check (notes.size() == 2, "erase removes the selected notes");
    check (notes[0].note == 60 && notes[1].note == 62, "erase keeps the others, in order");
    check (sel.empty(), "nothing is selected after erasing the selection");
}

//===========================================================================
// 4 and 5. A group move does not deform the group, and stays in range.
//===========================================================================

void testGroupMovePreservesShape()
{
    std::printf ("move: the shape survives, the limits hold, and the group stops together\n");

    Rng rng (0x5eed0006ull);

    int limitedByPitch = 0, limitedByTime = 0;

    for (int trial = 0; trial < 60000; ++trial)
    {
        const size_t count = (size_t) rng.intBetween (1, 12);
        auto notes = randomNotes (rng, count, 8.0);

        // Plant notes against the limits on purpose. A generator that stays in
        // the comfortable middle never reaches the clamp branch, and then a
        // per note clamp looks like it works.
        for (auto& n : notes)
        {
            const double which = rng.unit();
            if (which < 0.15) n.note = 127;
            else if (which < 0.30) n.note = 0;
            else if (which < 0.40) n.note = rng.intBetween (120, 127);
            // Outside the range a file is supposed to hold, and before the
            // pattern's origin. Neither can come from an edit here, and both
            // can come from a file, and without them the claim that nothing
            // is clamped per note on the way out is untested: a per note
            // clamp can only be seen through a note that is outside the
            // clamp's own range, and every note the generator made was
            // inside it.
            else if (which < 0.44) n.note = rng.intBetween (128, 140);
            else if (which < 0.48) n.note = rng.intBetween (-12, -1);

            const double where = rng.unit();
            if (where < 0.2) n.start = 0.0;
            else if (where < 0.26) n.start = -rng.between (0.1, 3.0);
        }

        const auto sel = randomSelection (rng, count);
        const auto before = notes;

        NoteSelection::Limits limits;
        limits.minStart = 0.0;
        if (rng.unit() < 0.4)
            limits.maxStart = rng.between (0.0, 10.0);
        if (rng.unit() < 0.3)
        {
            limits.lowNote  = rng.intBetween (0, 40);
            limits.highNote = rng.intBetween (80, 127);
        }

        NoteSelection::Request request;
        request.beats     = rng.between (-40.0, 40.0);
        request.semitones = rng.intBetween (-200, 200);
        request.grid      = rng.unit() < 0.5 ? 0.25 : 1.0 / 3.0;
        // Absolute is what the roll ships with, so it gets the most of this.
        const double snapRoll = rng.unit();
        request.snap = snapRoll < 0.5 ? NoteSelection::Snap::absolute
                     : snapRoll < 0.85 ? NoteSelection::Snap::relative
                                       : NoteSelection::Snap::off;
        request.anchorStart = sel.empty() ? 0.0 : notes[sel.front()].start;

        const auto move = NoteSelection::planMove (notes, sel, request, limits);
        NoteSelection::applyMove (notes, sel, move);

        if (move.pitchLimited) ++limitedByPitch;
        if (move.beatsLimited) ++limitedByTime;

        // The property. Every pairwise difference inside the selection is
        // untouched, so the chord is still the chord and the riff is still the
        // riff. This is the assertion that fails the moment anything clamps a
        // note on its own.
        for (size_t a = 0; a < sel.size(); ++a)
            for (size_t b = a + 1; b < sel.size(); ++b)
            {
                const auto i = sel[a], j = sel[b];
                check (notes[i].note - notes[j].note == before[i].note - before[j].note,
                       "a moved group keeps its intervals");
                checkClose (notes[i].start - notes[j].start,
                            before[i].start - before[j].start, 1e-9,
                            "a moved group keeps its rhythm");
            }

        // A move moves. It does not stretch, and it does not touch velocity.
        // Without this, making applyMove add the delta to the length as well
        // passes everything else here: the starts and the pitches are all
        // still right and the intervals are all still preserved.
        for (const auto index : sel)
        {
            checkClose (notes[index].length, before[index].length, 0.0,
                        "a moved note keeps its length");
            check (notes[index].velocity == before[index].velocity,
                   "a moved note keeps its velocity");
        }

        // Nothing outside the selection moved at all.
        for (size_t i = 0; i < count; ++i)
            if (! NoteSelection::contains (sel, i))
                check (notes[i].start == before[i].start && notes[i].note == before[i].note
                       && notes[i].length == before[i].length
                       && notes[i].velocity == before[i].velocity,
                       "an unselected note is not moved");

        // The flags mean "the limits cut this", which is not the same as "this
        // is not what was asked": snapping changes the delta too. So they are
        // checked against what the same request does with the limits taken
        // away, which is the only thing that tells the two apart. The first
        // version of the time flag compared against the request instead and
        // was wrong on nearly half of all clamped moves, with nothing to say
        // so.
        {
            NoteSelection::Limits none;
            none.minStart = -1e9;
            none.lowNote  = -100000;
            none.highNote = 100000;

            auto freeCopy = before;
            const auto freeMove = NoteSelection::planMove (freeCopy, sel, request, none);

            check (move.pitchLimited == (move.semitones != freeMove.semitones),
                   "pitchLimited says exactly whether the pitch limits cut the move");
            check (move.beatsLimited == (std::abs (move.beats - freeMove.beats) > 1e-12),
                   "beatsLimited says exactly whether the time limits cut the move");
        }

        // And the limits hold, stated as the honest invariant rather than the
        // tidy one. A limit stops a move and never forces one, so the claim
        // is in two halves: a note that was inside ends inside, and a note
        // that somehow started outside is never pushed further out. The
        // second half is what stops the clamp from answering a sideways drag
        // with a transposition, which is what the first version of this did.
        for (const auto index : sel)
        {
            const auto& was = before[index];
            const auto& is  = notes[index];

            if (was.note >= limits.lowNote)
                check (is.note >= limits.lowNote, "a note that was in range is not moved below the lowest pitch");
            else
                check (limits.lowNote - is.note <= limits.lowNote - was.note,
                       "a note already below the lowest pitch is not pushed further down");

            if (was.note <= limits.highNote)
                check (is.note <= limits.highNote, "a note that was in range is not moved above the highest pitch");
            else
                check (is.note - limits.highNote <= was.note - limits.highNote,
                       "a note already above the highest pitch is not pushed further up");

            if (was.start >= limits.minStart)
                check (is.start >= limits.minStart - 1e-9, "a note that was in range is not moved before the start");
            else
                check (is.start >= was.start - 1e-9, "a note already before the start is not pushed further back");

            if (std::isfinite (limits.maxStart))
            {
                if (was.start <= limits.maxStart)
                    check (is.start <= limits.maxStart + 1e-9, "a note that was in range is not moved past the end");
                else
                    check (is.start <= was.start + 1e-9, "a note already past the end is not pushed further on");
            }
        }

        // Standing still is always available, so a limit can cut a request to
        // nothing but can never turn it into a move in the other direction.
        check (move.semitones == 0 || (move.semitones > 0) == (request.semitones > 0)
               || request.semitones == 0,
               "a clamped transpose never reverses direction");
        // Not asserted under absolute snapping, where a reversal is the
        // correct answer rather than a clamp: the nearest grid line to where
        // the pointer has reached can be behind where the note started, so a
        // small nudge forward legitimately pulls the group back onto the
        // previous line. That is the behaviour, not a fault, and it is half
        // of why relative is the default here.
        if (request.snap != NoteSelection::Snap::absolute)
            check (move.beats == 0.0 || (move.beats > 0.0) == (request.beats > 0.0)
                   || request.beats == 0.0,
                   "a clamped move never reverses direction");
    }

    check (limitedByPitch > 1000, "the generator reached the pitch clamp");
    check (limitedByTime  > 1000, "the generator reached the time clamp");
}

void testGroupMoveSaturates()
{
    std::printf ("move: an impossible request stops the group at the limit, not on it\n");

    // Three notes a third apart, asked to go a long way up. The top note ends
    // exactly at the ceiling and the others keep their intervals: that is the
    // difference between stopping and collapsing, and it is the fault this
    // header was written for.
    std::vector<Note> notes { { 0.0, 1.0, 60, 0.8f }, { 0.0, 1.0, 64, 0.8f }, { 0.0, 1.0, 67, 0.8f } };
    Set sel { 0, 1, 2 };

    NoteSelection::Limits limits;
    const auto move = NoteSelection::transpose (notes, sel, 1000, limits);
    NoteSelection::applyMove (notes, sel, move);

    check (move.semitones == 60, "the transpose is cut to what fits");
    check (move.pitchLimited, "and says that it was cut");
    check (notes[2].note == 127, "the top note reaches the ceiling");
    check (notes[1].note == 124 && notes[0].note == 120, "and the chord is still the chord");

    // Downwards, from the other side.
    std::vector<Note> low { { 0.0, 1.0, 3, 0.8f }, { 0.0, 1.0, 10, 0.8f } };
    Set lowSel { 0, 1 };
    const auto down = NoteSelection::transpose (low, lowSel, -50, limits);
    NoteSelection::applyMove (low, lowSel, down);
    check (down.semitones == -3, "a downward transpose is cut by the lowest note");
    check (low[0].note == 0 && low[1].note == 7, "and the interval survives it");

    // A group already touching the ceiling cannot go up at all, and that is
    // the whole group and not just the note that is touching it.
    std::vector<Note> pinned { { 0.0, 1.0, 100, 0.8f }, { 0.0, 1.0, 127, 0.8f } };
    Set pinnedSel { 0, 1 };
    const auto blocked = NoteSelection::transpose (pinned, pinnedSel, 5, limits);
    NoteSelection::applyMove (pinned, pinnedSel, blocked);
    check (blocked.semitones == 0, "one note at the ceiling stops the group");
    check (pinned[0].note == 100 && pinned[1].note == 127, "and nothing moves");
}

//===========================================================================
// 6. Snapping, including what the clamp does to it.
//===========================================================================

void testSnapModes()
{
    std::printf ("snap: relative keeps the offset, absolute lands the anchor, and a clamp keeps the grid\n");

    Rng rng (0x5eed0007ull);

    for (int trial = 0; trial < 40000; ++trial)
    {
        const size_t count = (size_t) rng.intBetween (1, 8);
        auto notes = randomNotes (rng, count, 8.0);

        // Off grid on purpose. A group already on the grid cannot tell the two
        // snap modes apart and cannot detect per note snapping at all, which
        // is the mistake being guarded against.
        const double grid = rng.unit() < 0.5 ? 0.25 : 0.5;
        for (auto& n : notes)
            n.start = (double) rng.intBetween (0, 32) * grid + rng.between (0.01, grid * 0.9);

        const auto sel = randomSelection (rng, count);
        const auto before = notes;
        const size_t anchor = sel[(size_t) rng.intBetween (0, (int) sel.size() - 1)];

        NoteSelection::Limits limits;             // minStart 0, no upper bound
        NoteSelection::Request request;
        request.beats       = rng.between (-6.0, 20.0);
        request.grid        = grid;
        request.anchorStart = notes[anchor].start;

        // Relative: the delta is a whole number of grid steps, so a note that
        // was 7 ticks off the grid is still 7 ticks off it.
        request.snap = NoteSelection::Snap::relative;
        {
            auto copy = notes;
            const auto move = NoteSelection::planMove (copy, sel, request, limits);
            NoteSelection::applyMove (copy, sel, move);

            const double steps = move.beats / grid;
            checkClose (steps, std::round (steps), 1e-9,
                        "a relative group move is a whole number of grid steps");

            for (const auto index : sel)
            {
                const double offsetBefore = before[index].start - std::floor (before[index].start / grid) * grid;
                const double offsetAfter  = copy[index].start   - std::floor (copy[index].start   / grid) * grid;
                checkClose (offsetBefore, offsetAfter, 1e-9,
                            "a relative move keeps each note's distance from the grid");
            }
        }

        // Absolute: the anchor lands on a grid line, unless the clamp stopped
        // the move, and everything else keeps its distance from the anchor.
        request.snap = NoteSelection::Snap::absolute;
        {
            auto copy = notes;
            const auto move = NoteSelection::planMove (copy, sel, request, limits);
            NoteSelection::applyMove (copy, sel, move);

            const double landed = copy[anchor].start / grid;
            checkClose (landed, std::round (landed), 1e-9,
                        "an absolute group move lands the grabbed note on the grid");

            for (size_t a = 0; a < sel.size(); ++a)
                checkClose (copy[sel[a]].start - copy[anchor].start,
                            before[sel[a]].start - before[anchor].start, 1e-9,
                            "and everything else keeps its distance from it");
        }

        // The nearest feasible step, not merely a feasible one. Nothing above
        // pins this: with rounding replaced by flooring, every delta is still
        // on the progression, still in range, still preserves each note's
        // offset and still never reverses, and the only thing wrong is that a
        // group drag lags the pointer by up to a whole grid step and never
        // snaps forward until the pointer crosses the next line, which is the
        // most visible thing snapping does.
        for (const auto mode : { NoteSelection::Snap::relative, NoteSelection::Snap::absolute })
        {
            request.snap = mode;
            auto copy = notes;
            const auto move = NoteSelection::planMove (copy, sel, request, limits);

            double lowestStart = std::numeric_limits<double>::infinity();
            for (const auto index : sel)
                lowestStart = std::min (lowestStart, notes[index].start);
            const double lo = std::min (0.0, limits.minStart - lowestStart);

            // The neighbouring steps either side. Whichever of them is still
            // allowed must not be closer to what was asked for.
            for (const double neighbour : { move.beats - grid, move.beats + grid })
                if (neighbour >= lo - 1e-12)
                    check (std::abs (neighbour - request.beats)
                           >= std::abs (move.beats - request.beats) - 1e-12,
                           "a snapped move takes the nearest step that fits, not just one that fits");
        }

        // Off: the delta arrives untouched, which is what a snap defeat
        // modifier is for.
        request.snap = NoteSelection::Snap::off;
        {
            auto copy = notes;
            const auto move = NoteSelection::planMove (copy, sel, request, limits);
            if (! move.beatsLimited)
                checkClose (move.beats, request.beats, 1e-12,
                            "with snap off the delta is used as it arrives");
        }
    }

    // The order of operations, stated on its own, because getting it the wrong
    // way round is invisible until a group is dragged against the start of the
    // pattern: clamping a raw delta and snapping afterwards would be fine, but
    // snapping and then clamping without re-snapping leaves the group off the
    // grid for good.
    std::vector<Note> notes { { 1.0, 1.0, 60, 0.8f }, { 2.0, 1.0, 64, 0.8f } };
    Set sel { 0, 1 };
    NoteSelection::Limits limits;
    NoteSelection::Request request;
    request.beats = -5.0;           // far further left than the group can go
    request.grid  = 0.25;
    request.snap  = NoteSelection::Snap::relative;
    request.anchorStart = 1.0;

    const auto move = NoteSelection::planMove (notes, sel, request, limits);
    NoteSelection::applyMove (notes, sel, move);

    checkClose (move.beats, -1.0, 1e-12, "a clamped move goes as far as it can");
    checkClose (move.beats / 0.25, std::round (move.beats / 0.25), 1e-12,
                "and a clamped move is still a whole number of grid steps");
    checkClose (notes[0].start, 0.0, 1e-12, "the earliest note reaches the start");
    checkClose (notes[1].start, 1.0, 1e-12, "and the group kept its rhythm");

    // The same from an off grid start: the group can only move by whole steps,
    // so it stops short of the edge rather than snapping onto it.
    std::vector<Note> offGrid { { 0.6, 1.0, 60, 0.8f } };
    Set one { 0 };
    NoteSelection::Request leftward;
    leftward.beats = -3.0;
    leftward.grid  = 0.25;
    leftward.snap  = NoteSelection::Snap::relative;
    leftward.anchorStart = 0.6;

    const auto stopped = NoteSelection::planMove (offGrid, one, leftward, limits);
    NoteSelection::applyMove (offGrid, one, stopped);
    checkClose (stopped.beats, -0.5, 1e-12, "a relative move stops at the last whole step that fits");
    checkClose (offGrid[0].start, 0.1, 1e-12, "which leaves the note short of the edge, still off the grid");
}

//===========================================================================
// 7. Velocity on a group.
//===========================================================================

void testVelocityOnAGroup()
{
    std::printf ("velocity: additive, the group stops together, and the override really overrides\n");

    Rng rng (0x5eed0008ull);

    for (int trial = 0; trial < 40000; ++trial)
    {
        const size_t count = (size_t) rng.intBetween (1, 10);
        auto notes = randomNotes (rng, count, 8.0);

        // Plant notes at the rails, or the group-stops branch is never taken,
        // and below them, or the half of the invariant that says a limit
        // never FORCES an edit is never exercised. A velocity under the floor
        // cannot come from any edit here, only from a file.
        for (auto& n : notes)
        {
            const double which = rng.unit();
            if (which < 0.15) n.velocity = NoteSelection::kMaxVelocity;
            else if (which < 0.30) n.velocity = NoteSelection::kMinVelocity;
            else if (which < 0.36) n.velocity = NoteSelection::kMinVelocity * 0.25f;
        }

        const auto sel = randomSelection (rng, count);
        const auto before = notes;
        const double amount = rng.between (-1.5, 1.5);

        auto grouped = notes;
        NoteSelection::applyVelocity (grouped, sel, amount, NoteSelection::VelocityMode::group);

        // Differences preserved. This is what separates an offset from a
        // scale: a scale changes every difference it touches.
        for (size_t a = 0; a < sel.size(); ++a)
            for (size_t b = a + 1; b < sel.size(); ++b)
            {
                const auto i = sel[a], j = sel[b];
                checkClose ((double) grouped[i].velocity - grouped[j].velocity,
                            (double) before[i].velocity - before[j].velocity, 1e-6,
                            "a group velocity edit keeps the differences");
            }

        for (const auto index : sel)
        {
            const bool wasAboveFloor = before[index].velocity >= NoteSelection::kMinVelocity;

            // The floor is checked by what a MIDI file can carry rather than
            // against the constant itself, so that setting the constant to
            // zero fails here. A velocity of zero is a note off, which is the
            // whole reason the floor is not zero, and a check written as
            // "at least kMinVelocity" passes happily when kMinVelocity is
            // nothing.
            // Against MidiTimebase's own floor rather than against this
            // header's constant, so that setting the constant to zero fails
            // here. Written the other way, "at least kMinVelocity" passes
            // happily when kMinVelocity is nothing, and then an edit could
            // write a velocity of zero, which every studio reads as a note
            // off: the note would vanish on the way through a file.
            if (wasAboveFloor)
                check (grouped[index].velocity >= midiVelocityFromByte (1) - 1e-7,
                       "no velocity is driven below the lowest a file can carry");
            else
                check (grouped[index].velocity >= before[index].velocity - 1e-7,
                       "a velocity already under the floor is not pushed further down");

            check (grouped[index].velocity <= NoteSelection::kMaxVelocity + 1e-7,
                   "no velocity goes over the top");
        }

        // A limit stops an edit and never forces one, here as everywhere else.
        // A note arriving from a file below the floor must not turn a request
        // to quieten the group into one that makes it louder.
        if (amount <= 0.0)
            for (const auto index : sel)
                check (grouped[index].velocity <= before[index].velocity + 1e-7,
                       "a request to quieten never makes anything louder");
        if (amount >= 0.0)
            for (const auto index : sel)
                check (grouped[index].velocity >= before[index].velocity - 1e-7,
                       "and a request to lift never makes anything quieter");

        for (size_t i = 0; i < count; ++i)
            if (! NoteSelection::contains (sel, i))
                check (grouped[i].velocity == before[i].velocity,
                       "an unselected note's velocity is left alone");
    }

    // Apple's rule, stated directly: one note at the top stops all of them.
    std::vector<Note> notes { { 0.0, 1.0, 60, 0.5f }, { 0.0, 1.0, 64, 1.0f } };
    Set sel { 0, 1 };
    auto stopped = notes;
    NoteSelection::applyVelocity (stopped, sel, 0.3, NoteSelection::VelocityMode::group);
    check (stopped[0].velocity == notes[0].velocity && stopped[1].velocity == notes[1].velocity,
           "one note at full velocity stops the group");

    // And the override, which Apple documents as altering the relationship on
    // purpose. The assertion is that the relationship really does change: an
    // override that quietly did nothing would be a dead branch nothing else
    // in this file would notice.
    auto overridden = notes;
    NoteSelection::applyVelocity (overridden, sel, 0.3, NoteSelection::VelocityMode::perNote);
    checkClose (overridden[0].velocity, 0.8, 1e-6, "with the override the lower note keeps moving");
    check (overridden[1].velocity == 1.0f, "while the one at the top pins");
    check (std::abs ((double) overridden[0].velocity - overridden[1].velocity)
           < std::abs ((double) notes[0].velocity - notes[1].velocity) - 1e-9,
           "so the override does alter the relationship, which is what it is for");

    // Absolute: every note to one value, which is a different gesture.
    auto flattened = notes;
    NoteSelection::applyVelocity (flattened, sel, 0.42, NoteSelection::VelocityMode::absolute);
    check (flattened[0].velocity == flattened[1].velocity, "absolute sets one value for all");
    checkClose (flattened[0].velocity, 0.42, 1e-6, "and it is the value asked for");

    // And absolute is clamped too. It is the one mode where the value comes
    // straight from a caller rather than from a note that was already in
    // range, so it is the most likely of the three to be handed something
    // impossible.
    for (const double asked : { -4.0, 0.0, 1.0, 9.0 })
    {
        auto pinned = notes;
        NoteSelection::applyVelocity (pinned, sel, asked, NoteSelection::VelocityMode::absolute);
        for (const auto& n : pinned)
        {
            check (n.velocity >= NoteSelection::kMinVelocity && n.velocity <= NoteSelection::kMaxVelocity,
                   "an absolute velocity outside the range is brought into it");
            check (midiVelocityToByte (n.velocity) >= 1,
                   "and never to what a file reads as a note off");
        }
    }

    // The floor is the one a file can carry, not zero: a velocity of zero is a
    // note off, so an edit must not be able to write one.
    auto floored = notes;
    NoteSelection::applyVelocity (floored, sel, -5.0, NoteSelection::VelocityMode::perNote);
    for (const auto& n : floored)
        check (n.velocity >= NoteSelection::kMinVelocity,
               "no edit can drive a velocity to a note off");
}

//===========================================================================
// 8. Length on a group.
//===========================================================================

void testLengthOnAGroup()
{
    std::printf ("length: relative differences kept, and the two absolute modes are different modes\n");

    Rng rng (0x5eed0009ull);

    for (int trial = 0; trial < 40000; ++trial)
    {
        const size_t count = (size_t) rng.intBetween (1, 10);
        auto notes = randomNotes (rng, count, 8.0);

        // At the floor, so the group-stops branch is taken, and under it, so
        // the half of the invariant that says a limit never FORCES an edit is
        // exercised. A length under a tick cannot come from any edit here,
        // only from a file.
        const double plant = rng.unit();
        if (plant < 0.3)
            notes[(size_t) rng.intBetween (0, (int) count - 1)].length = NoteSelection::kMinLengthBeats;
        else if (plant < 0.4)
            notes[(size_t) rng.intBetween (0, (int) count - 1)].length = NoteSelection::kMinLengthBeats * 0.1;

        const auto sel = randomSelection (rng, count);
        const auto before = notes;
        const double amount = rng.between (-4.0, 4.0);

        auto grouped = notes;
        NoteSelection::applyLength (grouped, sel, amount, NoteSelection::LengthMode::group);

        for (size_t a = 0; a < sel.size(); ++a)
            for (size_t b = a + 1; b < sel.size(); ++b)
            {
                const auto i = sel[a], j = sel[b];
                checkClose (grouped[i].length - grouped[j].length,
                            before[i].length - before[j].length, 1e-9,
                            "a group length edit keeps the differences");
            }

        for (const auto index : sel)
        {
            if (before[index].length >= NoteSelection::kMinLengthBeats)
                check (grouped[index].length >= NoteSelection::kMinLengthBeats - 1e-12,
                       "a note that was long enough is not shortened past a tick");
            else
                check (grouped[index].length >= before[index].length - 1e-12,
                       "a note already shorter than a tick is not shortened further");
        }

        // And the limit never forces the group the other way. A note arriving
        // from a file shorter than a tick must not make a request to SHORTEN
        // the group lengthen it, which is what the first version did: at the
        // first instant of a resize drag, where the requested change is
        // nothing, every selected note grew before the mouse had moved.
        if (amount <= 0.0)
            for (const auto index : sel)
                check (grouped[index].length <= before[index].length + 1e-12,
                       "a request to shorten never lengthens anything");

        for (size_t i = 0; i < count; ++i)
            if (! NoteSelection::contains (sel, i))
                check (grouped[i].length == before[i].length, "an unselected note's length is left alone");
    }

    // The two absolute modes have to be distinguishable, or one of them is
    // dead code wearing a name. Notes with different starts are what tells
    // them apart, which is exactly why Apple publishes both.
    std::vector<Note> notes { { 0.0, 1.0, 60, 0.8f }, { 1.5, 0.5, 64, 0.8f } };
    Set sel { 0, 1 };

    auto sameLength = notes;
    NoteSelection::applyLength (sameLength, sel, 2.0, NoteSelection::LengthMode::sameLength);
    checkClose (sameLength[0].length, 2.0, 1e-12, "same length gives both notes that length");
    checkClose (sameLength[1].length, 2.0, 1e-12, "same length gives both notes that length");

    auto sameEnd = notes;
    NoteSelection::applyLength (sameEnd, sel, 4.0, NoteSelection::LengthMode::sameEnd);
    checkClose (sameEnd[0].start + sameEnd[0].length, 4.0, 1e-12, "same end gives both notes that end");
    checkClose (sameEnd[1].start + sameEnd[1].length, 4.0, 1e-12, "same end gives both notes that end");
    check (std::abs (sameEnd[0].length - sameEnd[1].length) > 1e-9,
           "so same end and same length are genuinely different operations");

    // Both absolute modes have the floor, not just same end. Asked for
    // nothing, or for less than nothing, they give a note rather than an
    // event a file cannot hold.
    for (const double asked : { -1.0, 0.0, NoteSelection::kMinLengthBeats * 0.1 })
    {
        auto tiny = notes;
        NoteSelection::applyLength (tiny, sel, asked, NoteSelection::LengthMode::sameLength);
        for (const auto& n : tiny)
            check (n.length >= NoteSelection::kMinLengthBeats,
                   "same length never writes a note shorter than a tick");

        auto tinyEnd = notes;
        NoteSelection::applyLength (tinyEnd, sel, asked, NoteSelection::LengthMode::sameEnd);
        for (const auto& n : tinyEnd)
            check (n.length >= NoteSelection::kMinLengthBeats,
                   "same end never writes a note shorter than a tick");
    }

    // A note that would be left behind by a same-end edit still gets a note,
    // not a zero.
    auto behind = notes;
    NoteSelection::applyLength (behind, sel, 0.5, NoteSelection::LengthMode::sameEnd);
    for (const auto& n : behind)
        check (n.length >= NoteSelection::kMinLengthBeats, "same end never writes a zero length note");

    // The group stop, in length: the shortest note decides how far everything
    // can shrink.
    std::vector<Note> mixed { { 0.0, 1.0, 60, 0.8f }, { 0.0, 0.25, 64, 0.8f } };
    Set mixedSel { 0, 1 };
    NoteSelection::applyLength (mixed, mixedSel, -2.0, NoteSelection::LengthMode::group);
    checkClose (mixed[1].length, NoteSelection::kMinLengthBeats, 1e-12,
                "the shortest note reaches the floor");
    checkClose (mixed[0].length - mixed[1].length, 0.75, 1e-9,
                "and the longer note keeps its difference from it");
}

//===========================================================================
// 9. Duplicating.
//===========================================================================

void testDuplicate()
{
    std::printf ("duplicate: one selection length later, and the copy becomes the selection\n");

    std::vector<Note> notes { { 0.0, 0.5, 60, 0.8f }, { 0.5, 0.5, 64, 0.6f } };
    Set sel { 0, 1 };

    checkClose (NoteSelection::extentBeats (notes, sel), 1.0, 1e-12,
                "the extent runs from the earliest start to the latest end");

    const auto copies = NoteSelection::duplicate (notes, sel);

    check (notes.size() == 4, "duplicating adds one note per selected note");
    check (copies == Set { 2, 3 }, "and hands back the copies, which become the selection");
    checkClose (notes[2].start, 1.0, 1e-12, "the copy lands one selection length later");
    checkClose (notes[3].start, 1.5, 1e-12, "the copy lands one selection length later");
    check (notes[2].note == 60 && notes[3].note == 64, "pitches are untouched");
    check (notes[2].velocity == notes[0].velocity, "and so are velocities");

    // Three in a row lay three copies end to end, with no gap and no pile.
    // That is what the copy becoming the selection buys, and it is the half of
    // Studio One's sentence that is easy to drop.
    std::vector<Note> chain { { 0.0, 1.0, 60, 0.8f } };
    Set chainSel { 0 };
    for (int i = 0; i < 3; ++i)
        chainSel = NoteSelection::duplicate (chain, chainSel);

    check (chain.size() == 4, "three duplicates make three copies");
    for (size_t i = 0; i < chain.size(); ++i)
        checkClose (chain[i].start, (double) i, 1e-12, "laid end to end rather than piled up");
    check (chainSel == Set { 3 }, "and only the last copy is selected");

    // An empty selection duplicates nothing rather than everything.
    std::vector<Note> untouched { { 0.0, 1.0, 60, 0.8f } };
    Set none;
    const auto nothing = NoteSelection::duplicate (untouched, none);
    check (nothing.empty() && untouched.size() == 1, "duplicating nothing does nothing");

    // A stale index must not come alive as the list grows. The loop appends
    // as it goes, so testing an index against the list's CURRENT size would
    // let an index past the end become valid the moment the first copy
    // landed, and the copies would start being copied.
    {
        std::vector<Note> three { { 0.0, 1.0, 60, 0.8f }, { 1.0, 1.0, 62, 0.8f },
                                  { 2.0, 1.0, 64, 0.8f } };
        Set stale { 0, 1, 2, 3, 4 };
        const auto copies = NoteSelection::duplicate (three, stale);
        check (three.size() == 6, "only the notes that existed are duplicated");
        check (copies.size() == 3, "and only those are handed back");
    }

    // A chord, where every note starts together: the extent is the longest
    // note, not the sum of them.
    std::vector<Note> chord { { 2.0, 1.0, 60, 0.8f }, { 2.0, 2.0, 64, 0.8f }, { 2.0, 1.5, 67, 0.8f } };
    Set chordSel { 0, 1, 2 };
    checkClose (NoteSelection::extentBeats (chord, chordSel), 2.0, 1e-12,
                "a chord's extent is its longest note");
}

//===========================================================================
// 10. What a moved note does to what it lands on.
//===========================================================================

void testOverlapResolution()
{
    std::printf ("overlap: the moved note wins, the covered start dies, the covered tail is cut\n");

    // Covering the start: the existing note is overwritten, per Ableton.
    {
        std::vector<Note> notes { { 1.0, 1.0, 60, 0.8f },     // the mover, selected
                                  { 1.5, 1.0, 60, 0.4f } };   // its start is covered
        Set sel { 0 };
        const auto report = NoteSelection::resolveOverlaps (notes, sel);
        check (report.deleted == 1 && report.trimmed == 0, "a covered start is a deletion");
        check (notes.size() == 1 && notes[0].velocity == 0.8f, "and the mover is what is left");
        check (sel == Set { 0 }, "the selection still points at the mover");
    }

    // Covering only the tail: the existing note is shortened to where the
    // mover begins.
    {
        std::vector<Note> notes { { 2.0, 1.0, 60, 0.8f },     // the mover, selected
                                  { 1.0, 2.0, 60, 0.4f } };   // only its tail is covered
        Set sel { 0 };
        const auto report = NoteSelection::resolveOverlaps (notes, sel);
        check (report.deleted == 0 && report.trimmed == 1, "a covered tail is a trim");
        check (notes.size() == 2, "and nothing is deleted");
        checkClose (notes[1].length, 1.0, 1e-12, "the trim ends where the mover begins");
        check (notes[1].length > 0.0, "and leaves a note rather than a stub");
    }

    // Covered entirely: deleted.
    {
        std::vector<Note> notes { { 0.0, 4.0, 60, 0.8f }, { 1.0, 1.0, 60, 0.4f } };
        Set sel { 0 };
        const auto report = NoteSelection::resolveOverlaps (notes, sel);
        check (report.deleted == 1, "a note covered entirely is deleted");
        check (notes.size() == 1 && notes[0].length == 4.0, "and the mover survives whole");
    }

    // Exactly abutting: a note starting where the mover stops survives, which
    // is the same half open convention the marquee uses and is what lets two
    // notes sit back to back.
    {
        std::vector<Note> notes { { 0.0, 1.0, 60, 0.8f }, { 1.0, 1.0, 60, 0.4f } };
        Set sel { 0 };
        const auto report = NoteSelection::resolveOverlaps (notes, sel);
        check (report.deleted == 0 && report.trimmed == 0, "back to back notes are left alone");
        check (notes.size() == 2, "and both survive");
    }

    // Another pitch is another note. This is the one the obvious
    // implementation gets wrong, by looping over every note it can see.
    {
        std::vector<Note> notes { { 1.0, 2.0, 60, 0.8f }, { 1.0, 2.0, 61, 0.4f } };
        Set sel { 0 };
        const auto report = NoteSelection::resolveOverlaps (notes, sel);
        check (report.deleted == 0 && report.trimmed == 0, "a note a semitone away is not touched");
        check (notes.size() == 2 && notes[1].length == 2.0, "and keeps its length");
    }

    // Inside the selection is inside the selection: resolving there would eat
    // a chord's own voicing, and anything overlapping in the group was put
    // there deliberately.
    {
        std::vector<Note> notes { { 0.0, 2.0, 60, 0.8f }, { 1.0, 2.0, 60, 0.4f } };
        Set sel { 0, 1 };
        const auto report = NoteSelection::resolveOverlaps (notes, sel);
        check (report.deleted == 0 && report.trimmed == 0, "overlaps inside the selection are left alone");
        check (notes.size() == 2, "and nothing in the group is lost");
    }

    // A trim that would leave less than a note behind is a deletion instead,
    // because a one tick stub is a note nobody can see and something will
    // still play.
    {
        const double tick = NoteSelection::kMinLengthBeats;
        std::vector<Note> notes { { 1.0 + tick * 0.5, 1.0, 60, 0.8f },
                                  { 1.0, 2.0, 60, 0.4f } };
        Set sel { 0 };
        const auto report = NoteSelection::resolveOverlaps (notes, sel);
        check (report.deleted == 1 && report.trimmed == 0, "a stub is deleted rather than kept");
        check (notes.size() == 1, "and only the mover is left");
    }

    // The selection is remapped across the deletions, by identity. A stale
    // index here would have the next edit move a note nobody picked.
    {
        std::vector<Note> notes { { 0.0, 1.0, 40, 0.1f },     // untouched, index 0
                                  { 5.0, 1.0, 60, 0.9f },     // the mover, selected
                                  { 5.2, 1.0, 60, 0.4f },     // covered start, deleted
                                  { 9.0, 1.0, 50, 0.2f } };   // untouched
        Set sel { 1 };
        const auto report = NoteSelection::resolveOverlaps (notes, sel);
        check (report.deleted == 1, "the covered note goes");
        check (notes.size() == 3, "leaving three");
        check (sel.size() == 1, "and one note still selected");
        if (sel.size() == 1)
            check (notes[sel[0]].velocity == 0.9f, "which is still the mover, not whatever slid into the gap");
    }

    // Two selected notes starting inside one unselected note: the earliest of
    // them decides the cut, because a later one is already inside the part
    // being taken away. Trimming to the later one instead leaves the note
    // still overlapping the earlier one, which is the state the whole rule
    // exists to remove, and every other check in this file passes while it
    // does.
    {
        std::vector<Note> notes { { 1.0, 0.5, 60, 0.8f },     // selected
                                  { 2.0, 0.5, 60, 0.8f },     // selected, later
                                  { 0.0, 4.0, 60, 0.4f } };   // the victim
        Set sel { 0, 1 };
        const auto report = NoteSelection::resolveOverlaps (notes, sel);
        check (report.trimmed == 1 && report.deleted == 0, "the victim is trimmed once");
        checkClose (notes[2].length, 1.0, 1e-12, "and cut at the earliest mover, not the latest");
    }

    // Randomised: resolution only ever touches unselected notes at a pitch
    // some selected note is on, and never lengthens anything.
    Rng rng (0x5eed000aull);
    int sawDeletion = 0, sawTrim = 0, sawCoincident = 0;

    for (int trial = 0; trial < 20000; ++trial)
    {
        const size_t count = (size_t) rng.intBetween (2, 12);
        auto notes = randomNotes (rng, count, 6.0);

        // A narrow pitch range, or overlaps at the same pitch almost never
        // happen and the test quietly checks nothing.
        for (auto& n : notes)
            n.note = rng.intBetween (60, 63);

        // And notes that start together, which a generator drawing starts
        // from a continuous range never produces. Two notes at one pitch
        // sharing a start is precisely the case MidiTimebase.h calls a drop,
        // so it is the one the rule most needs to catch, and relaxing the
        // covered-start test from "at or after" to "after" goes unnoticed
        // without it.
        if (rng.unit() < 0.5)
        {
            const size_t a = (size_t) rng.intBetween (0, (int) count - 1);
            const size_t b = (size_t) rng.intBetween (0, (int) count - 1);
            notes[b].start = notes[a].start;
            notes[b].note  = notes[a].note;
            ++sawCoincident;
        }

        const auto sel = randomSelection (rng, count);
        auto working = notes;
        auto workingSel = sel;

        const auto report = NoteSelection::resolveOverlaps (working, workingSel);

        if (report.deleted > 0) ++sawDeletion;
        if (report.trimmed > 0) ++sawTrim;

        check (working.size() + (size_t) report.deleted == count, "the deletion count is the truth");

        // Every selected note is exactly as it was: the rule is about what a
        // mover lands on, never about the mover.
        check (workingSel.size() == sel.size(), "no selected note is deleted");
        for (size_t i = 0; i < workingSel.size() && i < sel.size(); ++i)
        {
            const auto& was = notes[sel[i]];
            const auto& is  = working[workingSel[i]];
            check (is.start == was.start && is.note == was.note && is.length == was.length
                   && is.velocity == was.velocity,
                   "a selected note is never changed by resolution");
        }

        // And nothing was lengthened, nor given a length below a tick.
        for (const auto& n : working)
            check (n.length >= NoteSelection::kMinLengthBeats - 1e-12,
                   "resolution never leaves a note shorter than a tick");

        // The post-condition, which is the thing this function exists to
        // produce and which nothing above actually asserted: afterwards, no
        // unselected note overlaps a selected one at the same pitch. Counting
        // the deletions and checking the selection survived says the function
        // did something; this says it did the right thing, and it is what
        // catches a covered start going unresolved or a trim stopping at the
        // wrong mover.
        for (size_t u = 0; u < working.size(); ++u)
        {
            if (NoteSelection::contains (workingSel, u))
                continue;

            const auto& victim = working[u];
            const double victimEnd = victim.start + victim.length;

            for (const auto m : workingSel)
            {
                const auto& mover = working[m];
                if (mover.note != victim.note)
                    continue;

                const double moverEnd = mover.start + mover.length;
                check (! (mover.start < victimEnd && victim.start < moverEnd),
                       "no unselected note is left overlapping a selected one at the same pitch");
            }
        }
    }

    check (sawDeletion   > 500, "the generator produced deletions");
    check (sawTrim       > 500, "the generator produced trims");
    check (sawCoincident > 500, "the generator produced notes that start together");
}

//===========================================================================
// Nothing in here trusts a stale index.
//===========================================================================

void testStaleIndicesAreIgnored()
{
    std::printf ("robustness: an out of range index is ignored rather than read\n");

    std::vector<Note> notes { { 0.0, 1.0, 60, 0.8f } };
    Set sel { 0, 99 };

    NoteSelection::Limits limits;
    NoteSelection::Request request;
    request.semitones = 2;
    request.snap = NoteSelection::Snap::off;

    const auto move = NoteSelection::planMove (notes, sel, request, limits);
    NoteSelection::applyMove (notes, sel, move);
    check (notes.size() == 1 && notes[0].note == 62, "a stale index does not stop the real one moving");

    NoteSelection::applyVelocity (notes, sel, 0.1, NoteSelection::VelocityMode::group);
    checkClose (notes[0].velocity, 0.9, 1e-6, "nor a velocity edit");

    NoteSelection::applyLength (notes, sel, 0.5, NoteSelection::LengthMode::group);
    checkClose (notes[0].length, 1.5, 1e-9, "nor a length edit");

    Set empty;
    const auto nothing = NoteSelection::planMove (notes, empty, request, limits);
    check (nothing.semitones == 0 && nothing.beats == 0.0, "an empty selection plans no move");
}

} // namespace

int main()
{
    std::printf ("NoteSelection\n\n");

    testMarqueeIsIntersection();
    testSelectionAlgebra();
    testRemapAfterErase();
    testGroupMovePreservesShape();
    testGroupMoveSaturates();
    testSnapModes();
    testVelocityOnAGroup();
    testLengthOnAGroup();
    testDuplicate();
    testOverlapResolution();
    testStaleIndicesAreIgnored();

    std::printf ("\n");

    if (failures != 0)
    {
        std::printf ("%d check(s) failed.\n", failures);
        return 1;
    }

    std::printf ("All checks passed.\n");
    return 0;
}
