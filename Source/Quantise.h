#pragma once
#include "MidiTimebase.h"
#include <algorithm>
#include <cmath>
#include <vector>

// ---------------------------------------------------------------------------
// Quantise: moving notes that were played onto the grid.
//
// This is the destructive half of the studio's timing. `Groove.h` is the other
// half and the two do opposite jobs: a groove swings notes that are already on
// the grid, at playback, and leaves anything played off the grid exactly where
// the performance put it. That is the right behaviour for a feel and no help
// at all to somebody who played a part slightly early, so without this there
// was no way to tidy a performance.
//
// No JUCE, so all of it is tested: see Tests/QuantiseTest.cpp. The numbers
// here are a claim about agreement with what every other studio means by a
// strength and a swing percentage, and that is the kind of claim that has to
// be measured rather than eyeballed.
//
// --- What is published, and what is not -----------------------------------
//
// Worth stating before any of the arithmetic, because this project keeps
// having to fix numbers that were present, plausible and unsourced: **no
// manufacturer publishes an equation for quantise.** Four publish the
// behaviour in words precisely enough to pin it down, two open source projects
// publish a numeric definition that agrees with them, and everything beyond
// that is a decision made here and labelled as one.
//
// Strength is linear interpolation from where the note was played toward the
// grid. The sources describe exactly that and none writes it down:
//
//   Avid, Pro Tools help, Additional Grid Quantize Options: notes are "moved a
//   percentage toward the Quantize Grid", "lower percentages preserve the
//   original feel", range 0 to 100 per cent.
//
//   Apple, Logic Pro User Guide, Quantize parameters: Q-Strength "determines
//   how far a note or transient marker is shifted toward the nearest grid
//   position"; 100 per cent "results in full quantization, and 0% leaves the
//   note or transient marker at its recorded position".
//
//   Steinberg, Cubase Plug-in Reference, Quantizer: Strength "determines how
//   close the notes should be moved to the quantize grid", and at 100 per cent
//   "all notes are forced to the closest grid position".
//
//   Rosegarden's manual is the most explicit of any source: below full, each
//   note "will only be moved that percentage of the distance toward its
//   nearest grid line". Ardour's says "as a percentage of the nominal
//   distance".
//
// Ableton is the one wording that does not fit: Live's manual says Amount
// moves notes "only by a percentage of the set quantization value", which read
// literally is a percentage of the step rather than of the distance and would
// overshoot the grid. Taken here as loose prose for the same thing, because
// the literal reading is not a quantiser.
//
// Steinberg's iterative quantize documents a property that interpolation gives
// for free and that nothing else does: "Repeated use moves them progressively
// closer to the grid." Two passes at strength s leave exactly (1 - s)^2 of the
// original offset, which is the single best check that this is interpolation
// and not a "snap if close enough" shortcut wearing a percentage.
// ---------------------------------------------------------------------------

namespace Quantise
{

/** The smallest length a quantised note is allowed to come out with: one
    export tick. `MidiTimebase.h` already holds the reason, that "a note on and
    a note off at the same tick is a note that some studios draw and none of
    them play", and a both-ends quantise is the operation most likely to ask
    for one, since any note shorter than the grid can have both of its ends
    land on the same point. */
inline constexpr double kMinLengthBeats = 1.0 / (double) kMidiExportPpq;

/** Swing is the offbeat's position inside its pair, which is the scale
    `Groove.h` already uses, so an edit and a playback setting cannot disagree
    about what a swing number means.

    Apple's Q-Swing is on this scale and is the source for it: it "alters the
    position of every second point in the current quantization grid", and
    "values over 50% delay the beats; values under 50% pre-delay (advance)
    them". Apple's named grid presets give six exact points, 50, 54, 58, 62, 66
    and 71 per cent.

    Pro Tools and Rosegarden publish the other scale, where 0 per cent is
    straight and 100 per cent is "a triplet feel" (Avid) or the offbeat placed
    "two-thirds of the distance between the previous and next" grid lines
    (Rosegarden), with Avid's maximum of 300 per cent putting the offbeat on
    the next grid point entirely. The two convert exactly, and the conversion
    is pinned at three published points rather than one, so a number read off
    an Avid dialog can be brought across rather than being four times out:

        thisScale = 0.5 + avidPercent / 600
        avidPercent = 600 * (thisScale - 0.5)

    Cubase's swing is also a percentage and Steinberg never states what any
    value of it means, so it cannot be matched from the documentation at all.

    The clamp is a decision, and the number it takes comes from Apple, which is
    not the same as Apple setting a limit: Apple says only that "the most
    practical settings are between 50% and 75%, which give a swing feel to
    strictly quantized (or tightly played) regions", and gives Q-Swing no
    minimum or maximum at all. The bottom is made symmetric about straight so
    that the pre-delay Apple documents has the same room as the delay. The
    clamp also keeps the grid usable: at 0 or 1 the offbeat lands on a
    downbeat, the grid has half as many distinct points, and "the nearest grid
    point" becomes a coin toss between two identical positions. Apple's whole
    preset table, 50 to 71, is inside it. */
inline constexpr double kMinSwing = 0.25;
inline constexpr double kMaxSwing = 0.75;

/** Converts a Pro Tools or Rosegarden swing percentage to this scale.

    Avid's range runs to 300 per cent, which is 1.0 here, so everything above
    150 per cent lands on `kMaxSwing` once `clampedSwing` has had it. That is
    not a conversion error: it is the clamp above, and what it refuses is the
    half of Avid's range that pushes the offbeat from half a step away from the
    following downbeat to sitting on top of it. */
inline double swingFromAvidPercent (double percent) noexcept
{
    return 0.5 + percent / 600.0;
}

/** What is allowed to move. Avid's rule, which is the clearest published
    statement of the note length question and the one the other manuals agree
    with structurally.

    Avid's Note On and Note Off are separate choices, and "Preserve Note
    Duration" is **unavailable** when both are selected, because the two
    requests contradict each other: a note cannot keep its length and have both
    of its ends on the grid. Avid removes the option rather than silently
    picking one, which is what `preserveLengthApplies` copies. Cubase keeps the
    three apart the same way, as three differently named commands: Quantize
    "quantizes audio or MIDI event starts", Quantize MIDI Event Ends "moves the
    ends of MIDI events to the nearest grid positions", and Quantize MIDI Event
    Lengths is a trim to a length rather than to a position. Ableton says what
    end quantise costs: it "will stretch the note". Logic's Smart Quantize is
    described by what it refuses to break, keeping "the relative position of
    note on, note off, and CC events".

    So starts, with the length carried along, is the default everywhere, and
    anything else is an explicit choice. */
enum class Target
{
    starts = 0,   ///< Note ons move. The default.
    ends,         ///< Note offs move; starts stay where they were played.
    both          ///< Both ends land on the grid, and the length is whatever that leaves.
};

/** Which notes the grid is allowed to touch.

    This is the control every manual warns about by the way it is built. Apple
    signs it: Q-Range of 0 means "every note or transient marker is quantized",
    and "negative Q-Range values move only notes or transient markers that fall
    outside the set range to ideal quantization grid positions, while those
    closer to an ideal position are not quantized". Apple documents only the
    negative case, which is also the musically useful one, so a naive reading
    of the sign gets it backwards and leaves a control that visibly does
    something to the wrong half of the notes.

    Avid splits the same thing into two separately named options precisely so
    the sign cannot be misread, Include Within and Exclude Within, and Cubase
    does the same as Catch Range and Non-Quantize. Naming them is what this
    header does, for that reason. */
enum class Gate
{
    off = 0,   ///< Every note is quantised. Apple's Q-Range of zero.
    include,   ///< Only notes already within the window move. Avid's Include Within, Cubase's Catch Range.
    exclude    ///< Only notes outside the window move: the stragglers are tidied and the good notes left alone. Avid's Exclude Within, Cubase's Non-Quantize, Apple's negative Q-Range.
};

struct Options
{
    /** The grid, in beats. A quarter is 1.0, a sixteenth is 0.25. Triplets are
        an ordinary value on this scale rather than a separate mode. Avid
        publishes the arithmetic for an irregular grouping: "if a sixteenth
        note equals 240 ticks, quintuplet sixteenth notes with 5 in time of 4
        would yield a Grid size of 192 ticks or (240 ticks/5)*4". So five in
        the time of four divides the note value by five and multiplies by four,
        which for a sixteenth is 0.25 * 4 / 5 = 0.2 beats, and reads the same
        way round as Avid's own worked answer rather than the reciprocal of
        it. */
    double grid = 0.25;

    /** How much of the distance to the grid is travelled, 0 to 1. */
    double strength = 1.0;

    /** The offbeat's position inside its pair. 0.5 is straight. */
    double swing = 0.5;

    Target target = Target::starts;

    /** Only consulted for Target::starts: see `preserveLengthApplies`. With it
        set, the note keeps its length and its end travels with its start; with
        it clear the end stays where it was played, which is Avid's "when
        deselected with the Note On option selected, note end points are not
        moved". */
    bool preserveLength = true;

    Gate gate = Gate::off;

    /** The region of the pattern the clip actually plays, in beats, if the
        caller knows it. A note whose onset is inside it is never moved out of
        it: the engine and the export both take only the notes whose onset is
        inside the clip's window, so a note pushed past either edge is still
        in the pattern, still drawn in the roll, and silent.

        That is a decision, and the alternative was tried first and was worse.
        Growing the clip to cover the note sounds tidier and changes the
        arrangement: a clip is a window onto its pattern and is often
        deliberately shorter than it, which is how one bar of a longer pattern
        is looped, so growing it un-hides everything the trim was hiding. A
        note that stays where it was played is a note that still sounds, and
        the only cost is that it is not on the grid, which is visible.

        Both edges matter. The back edge is the common one: the last sixteenth
        of a bar quantised to a beat lands on the bar line and falls out of the
        clip. The front edge is reachable too, after the clip's left edge has
        been dragged, since the window then starts part way into the pattern
        and a note near it can quantise to a point before it.

        `windowEnd <= windowStart` means the caller does not know or does not
        care, and nothing is held back. */
    double windowStart = 0.0;
    double windowEnd = 0.0;

    /** The gate's window, as a fraction of the step, 0 to 1.

        The width comes from Avid's two worked examples, which are the only
        quantified definition of such a gate in any primary source: a quarter
        note grid with Include Within at 50 per cent gives "an eighth note area
        around each beat", and at 25 per cent gives "a sixteenth note area". So
        the window is range * grid wide, centred on the grid point, and the
        tolerance either side is half of that.

        At 1.0 the window is a whole step wide, which on a straight grid
        reaches every note, because nothing there is more than half a step from
        a grid point. Under swing it does not: the gap from a downbeat to a
        swung offbeat is up to one and a half steps, so a note in the middle of
        that gap is three quarters of a step from either end and stays outside
        a full window. That is what Avid's definition says rather than
        something to correct, and the test pins it. */
    double range = 0.5;
};

/** True when `preserveLength` means anything.

    Avid makes the option unavailable rather than ignoring it, because a
    request to keep the length while putting both ends on the grid is a
    contradiction and silently picking one of the two is how a studio ends up
    doing something the dialog did not say. Callers should grey the control out
    on a false here, and `apply` refuses it either way. */
inline bool preserveLengthApplies (const Options& o) noexcept
{
    return o.target == Target::starts;
}

/** The grid's beats per step, with anything unusable replaced by a sixteenth
    rather than left to divide by zero. */
inline double stepBeats (const Options& o) noexcept
{
    if (! std::isfinite (o.grid) || o.grid <= 0.0)
        return 0.25;
    return o.grid;
}

inline double clampedSwing (const Options& o) noexcept
{
    if (! std::isfinite (o.swing))
        return 0.5;
    return std::clamp (o.swing, kMinSwing, kMaxSwing);
}

inline double clampedStrength (const Options& o) noexcept
{
    if (! std::isfinite (o.strength))
        return 0.0;
    return std::clamp (o.strength, 0.0, 1.0);
}

inline double clampedRange (const Options& o) noexcept
{
    if (! std::isfinite (o.range))
        return 0.0;
    return std::clamp (o.range, 0.0, 1.0);
}

/** Where the grid's `index`th point sits, counting from the clip's own origin.

    Every second point is an offbeat and moves with the swing; the others never
    move, for any swing value, which is what Apple, Steinberg, Avid and
    Rosegarden all say in four different wordings: "every second point in the
    current quantization grid", "every second position in the grid", "every
    other Grid boundary", "alternate beats".

    Pairs are counted from the clip's origin. No manufacturer says whether they
    count from the bar, the clip or the project, so this agrees with the one
    other place in the studio that has already decided, which is `Groove.h`. */
inline double gridPoint (long long index, double step, double swing) noexcept
{
    const auto pairIndex = (index >= 0 ? index / 2 : -((-index + 1) / 2));
    const double pairStart = (double) (pairIndex * 2) * step;

    if ((index - pairIndex * 2) == 0)
        return pairStart;

    // The pair spans two steps, so the straight offbeat is at one half of it
    // and the swung offbeat is at the swing fraction of it.
    return pairStart + swing * (step * 2.0);
}

/** How many pairs of grid steps from the origin this arithmetic will work.

    The grid search counts pairs in signed integers, and that overflows long
    before a double runs out of range, which is undefined behaviour rather than
    a wrong answer. The quantity that overflows is the beat divided by the
    pair, so the guard is on that rather than on the beat alone: a position
    inside any sane range still overflows it if the grid is small enough, and a
    guard on the beat reads as complete while leaving that open.

    A note out there is a fault upstream rather than a position. `ProjectIO`
    will read whatever a file claims, and `MidiTimebase.h` caps its own tick
    arithmetic at `kMidiMaxTick` for the same reason, which is `kMaxBeats`
    here: past that a note is beyond anything a file could hold. */
inline constexpr double kMaxBeats = (double) kMidiMaxTick / (double) kMidiExportPpq;
inline constexpr double kMaxPairIndex = 4.0e18;

/** The grid point nearest a beat, with ties going to the earlier point.

    A beat that is not a number, or is past `kMaxBeats`, is returned unchanged,
    so a caller that is placing notes leaves it where it is.

    Only the two pairs either side of the beat can hold the nearest point, so
    the search is six candidates rather than a loop. Doing it by candidate
    rather than by arithmetic is deliberate: with swing on, the gaps between
    points are unequal, so rounding the beat to a multiple of anything is
    wrong, and that is the shape of mistake this is written to avoid. */
inline double nearestGridBeat (double beat, double step, double swing) noexcept
{
    if (! std::isfinite (beat) || std::abs (beat) > kMaxBeats)
        return beat;

    const double pair = step * 2.0;

    if (! (std::abs (beat / pair) < kMaxPairIndex))
        return beat;

    const auto pairIndex = (long long) std::floor (beat / pair);

    double best = 0.0;
    double bestDistance = -1.0;

    for (long long p = pairIndex - 1; p <= pairIndex + 1; ++p)
    {
        for (long long member = 0; member < 2; ++member)
        {
            const double candidate = gridPoint (p * 2 + member, step, swing);
            const double distance = std::abs (beat - candidate);

            // Strictly less, so of two equally near points the earlier one
            // wins and the choice does not depend on the order of the search.
            if (bestDistance < 0.0 || distance < bestDistance)
            {
                bestDistance = distance;
                best = candidate;
            }
        }
    }

    return best;
}

/** Whether the gate lets this beat be quantised. */
inline bool passesGate (double beat, double target, const Options& o) noexcept
{
    if (o.gate == Gate::off)
        return true;

    // Avid's worked examples: the window is range * grid wide and centred on
    // the grid point, so the tolerance either side is half of it.
    const double tolerance = clampedRange (o) * stepBeats (o) * 0.5;
    const double distance = std::abs (beat - target);

    return o.gate == Gate::include ? distance <= tolerance
                                   : distance > tolerance;
}

/** Where one beat ends up.

    `new = old + strength * (grid - old)`: the equation nobody publishes and
    everybody describes. At strength 0 the beat is returned bit for bit, which
    matters because Apple, Steinberg and Avid all state that endpoint and
    because a quantise of nothing should be able to leave a performance
    untouched rather than nearly untouched. */
inline double place (double beat, const Options& o) noexcept
{
    if (! std::isfinite (beat))
        return beat;

    const double step = stepBeats (o);
    const double swing = clampedSwing (o);
    const double target = nearestGridBeat (beat, step, swing);

    if (! passesGate (beat, target, o))
        return beat;

    const double strength = clampedStrength (o);

    if (strength <= 0.0)
        return beat;
    if (strength >= 1.0)
        return target;

    return beat + strength * (target - beat);
}

/** What a pass did, so the caller can say so rather than leaving somebody to
    wonder whether anything happened.

    These are what the pass actually did and not what it attempted, which is
    not the same thing: an end pushed onto the grid and then trimmed straight
    back to where it already was has not moved a note, and counting the attempt
    had the piano roll reporting work on a pattern it had left exactly as it
    found it. So `moved` is the number of notes that came out somewhere other
    than where they went in, and `trimmed` is the subset of those that were
    shortened by a collision. */
struct Result
{
    int moved   = 0;   ///< Notes that came out at a different position or length.
    int trimmed = 0;   ///< Of those, the ones cut back to where a later note of the same pitch starts.
    int dropped = 0;   ///< Notes lost because another of the same pitch landed on the same onset.
    int held    = 0;   ///< Notes left where they were played because the grid would have pushed them out of the clip.
};

/** Whether two notes already overlapped before the pass, by index into the
    before picture. Half open, so two notes that merely abut did not overlap.

    An index the before picture does not have answers no, which means a caller
    that passes a shorter picture than it has notes gets everything resolved.
    That is the useful answer for a caller resolving a set it has just built. */
inline bool overlappedBefore (const std::vector<TimebaseNote>& before,
                              size_t a, size_t b) noexcept
{
    if (a >= before.size() || b >= before.size())
        return false;

    const auto& first = before[a];
    const auto& second = before[b];

    return first.start < second.start + second.length
        && second.start < first.start + first.length;
}

/** Whether two notes were already struck together, within one export tick,
    before the pass. Same convention for an index that is not there. */
inline bool struckTogetherBefore (const std::vector<TimebaseNote>& before,
                                  size_t a, size_t b) noexcept
{
    if (a >= before.size() || b >= before.size())
        return false;

    return std::abs (before[a].start - before[b].start) < kMinLengthBeats;
}

/** Puts a quantised set of notes into a shape the studio and a MIDI file can
    both hold, following the rule `MidiTimebase.h` already established for the
    file round trip, and counts what that cost.

    Quantise is the operation that causes this: two notes of the same pitch a
    sixteenth apart, quantised to a quarter grid, become one. One MIDI channel
    has one note on and one note off per pitch, so an overlap cannot be
    described, and the existing rule is that the earlier note is shortened to
    where the later one starts, while of two beginning together the longer is
    kept and the other is counted as lost.

    `before` is the pattern as it stood before the pass, index aligned with
    `notes` as they came in, and a pair is resolved **only if it did not
    already overlap**. That restriction is the whole point and the first
    version of this got it wrong by asking a weaker question, whether either
    note had moved at all. A piano roll will happily draw two notes of one
    pitch on top of each other, and a held note re-struck before it is released
    is ordinary material: sustained piano, a re-triggered pad, a roll on one
    drum. Under the weaker rule, nudging either of them by a thousandth of a
    beat cost the earlier note everything past the later one's onset, and the
    note that lost its tail had not necessarily moved at all. An overlap that
    was already there is left alone, and the export's own rule deals with it
    at the only point where it genuinely cannot be held, which is where it was
    being dealt with before this feature existed.

    Two onsets within one export tick of each other count as one onset, and of
    the two the one kept is the one that was audible, which is the longer, and
    of two equally long the earlier. That is a decision rather than a standard,
    and worth stating as one: a tick is the finest position this project keeps,
    two strikes of one pitch half a millisecond apart are one strike musically,
    and the alternative is a note nobody can see or select. It is not a claim
    about the file, because a pair that straddles a tick boundary would export
    as two notes.

    Keeping the longer rather than whichever happened to be sorted later is the
    whole of what makes this safe, and an audit found what the other way costs.
    A sub-tick artefact struck a third of a tick before a two beat bass note is
    ordinary material from a double triggered pad or a noisy input, and under
    the earlier rule the bass note was the one deleted: the pass kept a
    sixty-fourth of a beat at velocity 0.10 and threw away two beats at 0.95.
    It also put this file at odds with the export, which resolves the identical
    pair by `MidiTimebase.h`'s rule and keeps the longer, so the edit and the
    bounce disagreed about which of the two was the note. The rule here is that
    same rule, which is what the paragraph above promises.

    It fires only on onsets this pass brought together, which is the double
    triggered pad collapsed onto one grid point, and it fires there even when
    the pair was already overlapping, because by then the two are one note:
    leaving both would hand the export a pair it has to throw one away from,
    and a bounce that is not the pattern that was played is the worst thing
    this studio can do. A pair that was already struck together is left as it
    was drawn, since nothing this pass did made it that way.

    No manufacturer documents any of this, which the backlog says plainly. The
    reason to follow this project's own rule rather than invent one is that it
    makes quantise leave a pattern the export does not have to repair, and the
    test measures exactly that: it starts from material the export was happy
    with and asks `midiNotesToTicks` how many notes it had to throw away
    afterwards, and the answer has to be none. */
inline void resolveOverlaps (std::vector<TimebaseNote>& notes,
                             const std::vector<TimebaseNote>& before,
                             Result& result)
{
    struct Entry
    {
        TimebaseNote note;
        size_t index = 0;
        bool trimmed = false;
    };

    std::vector<Entry> entries;
    entries.reserve (notes.size());
    for (size_t i = 0; i < notes.size(); ++i)
        entries.push_back ({ notes[i], i, false });

    // By pitch, then by start, then longest first, so that of two notes
    // beginning together the one kept is the one that was audible.
    std::stable_sort (entries.begin(), entries.end(), [] (const Entry& a, const Entry& b)
    {
        if (a.note.note != b.note.note)   return a.note.note < b.note.note;
        if (a.note.start != b.note.start) return a.note.start < b.note.start;
        return a.note.length > b.note.length;
    });

    std::vector<Entry> kept;
    kept.reserve (entries.size());

    for (const auto& e : entries)
    {
        bool lost = false;

        // Every note of this pitch already kept, newest first, rather than
        // only the one before. Looking at neighbours is enough while every
        // pair gets resolved, because then each resolved pair leaves the one
        // before it unable to reach the one after. It is not enough once a
        // pair can be skipped: an inherited overlap is left in place, so it
        // can sit between two notes that this pass has just brought into
        // collision, and that pair would never be compared. Notes of one pitch
        // are a contiguous run at the back of what is kept, because the sort
        // puts pitch first, so the scan stops at the first other pitch.
        for (auto it = kept.rbegin(); it != kept.rend() && it->note.note == e.note.note; ++it)
        {
            // Onsets the pass has brought together are one onset, whatever the
            // two notes were doing before. That includes a pair that was
            // already overlapping: two notes of one pitch put on the same
            // point are the same note, and leaving both would hand the export
            // a pair it has to throw one away from, which is a bounce that is
            // not the pattern that was played.
            if (e.note.start - it->note.start < kMinLengthBeats
                && ! struckTogetherBefore (before, it->index, e.index))
            {
                // Which of the two is lost is decided by length and not by
                // which was sorted later, because the one worth keeping is the
                // one that was audible. The sort puts the longer first only
                // where the two starts are exactly equal, and this branch
                // fires on starts that are unequal and under a tick apart, so
                // without this the note kept would be whichever was played
                // first however short it was. `MidiTimebase.h` keeps the
                // longer, so deciding it any other way would leave the edit
                // and the export disagreeing about the same pair.
                //
                // Strictly greater, so that of two equally long notes the one
                // kept is the earlier. That is the same tie `MidiTimebase.h`
                // breaks the same way, by sorting longest first and keeping
                // the first of equals.
                if (e.note.length > it->note.length)
                {
                    // The later note is the audible one, so it takes the place
                    // of what is in hand rather than being thrown away. Its
                    // own onset comes with it: the two are within a tick, so
                    // the difference is finer than anything downstream keeps,
                    // and leaving the audible note exactly where the grid put
                    // it is easier to reason about than moving it back onto an
                    // onset that belonged to the note being dropped.
                    //
                    // Clearing the trim flag cannot currently change an
                    // answer, and is kept because it would be true of the
                    // note being put in hand if it ever could. A trim of the
                    // note being replaced needs another note at least a tick
                    // after it and processed before this one, and the entries
                    // are in ascending order of start: that note would have to
                    // start no later than this one, which is under a tick
                    // after. So nothing reaching here has been trimmed, which
                    // is why a mutation that drops this line survives.
                    it->note = e.note;
                    it->index = e.index;
                    it->trimmed = false;
                }

                lost = true;
                break;
            }

            // A trim, on the other hand, is only ever for a collision this
            // pass created. An overlap somebody drew keeps its full length.
            //
            // And a trim never leaves less than a tick. It cannot normally:
            // two onsets closer than that have already been dealt with above.
            // It can when they were struck together before, which is the pair
            // the rule above deliberately leaves alone, and trimming one of
            // those to the other would make a note nothing plays out of a note
            // somebody drew.
            if (! overlappedBefore (before, it->index, e.index)
                && e.note.start - it->note.start >= kMinLengthBeats
                && it->note.start + it->note.length > e.note.start)
            {
                it->note.length = e.note.start - it->note.start;
                it->trimmed = true;
            }
        }

        if (lost)
        {
            ++result.dropped;
            continue;
        }

        kept.push_back (e);
    }

    // Back into playing order, which is the order a piano roll and a file both
    // want them in.
    std::stable_sort (kept.begin(), kept.end(), [] (const Entry& a, const Entry& b)
    {
        if (a.note.start != b.note.start) return a.note.start < b.note.start;
        return a.note.note < b.note.note;
    });

    result.moved = 0;
    result.trimmed = 0;

    notes.clear();
    notes.reserve (kept.size());

    for (const auto& e : kept)
    {
        const bool differs = e.index >= before.size()
                          || e.note.start != before[e.index].start
                          || e.note.length != before[e.index].length;

        if (differs)
        {
            ++result.moved;
            if (e.trimmed)
                ++result.trimmed;
        }

        notes.push_back (e.note);
    }
}

/** Quantises a whole set of notes in place.

    Starts and ends are placed by the same grid, the same strength and the same
    gate, which is what Avid does: the options say which of the two is allowed
    to move, not which one gets a different treatment. One consequence of that
    is worth stating rather than leaving to be discovered: with both ends
    moving, the gate is asked about each end separately, so a note can have one
    end quantised and the other left where it was played. That follows from
    Avid treating a note on and a note off as two separately selectable
    events, and it is what the gate means, but it does surprise people.

    Two properties were measured rather than assumed, and the second is the one
    to know before relying on it.

    Quantising starts is idempotent: a second pass at full strength moves
    nothing, which is what anybody would expect of a tidy up.

    Quantising **ends** is not, and it is not a fault. An end put on the grid
    and then trimmed back to where the next note of its pitch begins is no
    longer on the grid, because the trim answers to a different constraint, so
    a second pass has something to do to it. One further pass settles it, and
    that is measured rather than argued: six thousand randomised patterns
    across six grids, six swings, both length settings, both of the targets
    that move an end, and with the gate off and on. That matters because
    somebody who presses Quantise twice to be sure should not get a third
    pattern.

    At full strength, that is. Below it, pressing Quantise again moves
    everything closer to the grid again and is meant to: that is the Steinberg
    property quoted at the top of this file, `(1 - s)^n` of the offset left
    after n passes, which never reaches nothing. The piano roll offers 80 per
    cent as the setting that keeps the feel, so that is a setting somebody will
    be on, and a second press there is a second helping rather than a mistake.

    It is worth knowing which rule bought that. An earlier version resolved any
    pair of notes where either had moved, and under it a gated pass could
    cascade: a trim moved an end closer to the grid, which brought it inside an
    include window it had been outside of, which trimmed the note before it,
    one note per pass, and an audit found a line of twenty four notes that took
    twenty five passes to settle. Resolving only the collisions the pass itself
    created ended the cascade as well as the deletions it was fixed for. The
    case the audit found is kept in Tests/QuantiseTest.cpp as a regression.

    Lengths run down rather than up while that settles, except in two cases
    where growing is the right answer: with both ends moving a note can grow
    because the note after it moved out of the way, and an inherited overlap is
    never resolved, so nothing trims an end the grid has pushed outward. Nobody
    documents any of this corner. */
inline Result apply (std::vector<TimebaseNote>& notes, const Options& o)
{
    Result result;

    if (notes.empty())
        return result;

    // The pass needs to know what it inherited, so that it only resolves the
    // collisions it caused and so that the counts are about what changed. One
    // copy of the notes: this runs on an edit, not in the device callback.
    const std::vector<TimebaseNote> before = notes;

    const bool moveStarts = (o.target != Target::ends);
    const bool moveEnds   = (o.target != Target::starts);

    // Refused rather than ignored: with both ends going to the grid there is
    // no length left to preserve, and Avid makes the option unavailable in
    // exactly this case rather than quietly honouring one of the two requests.
    const bool keepLength = o.preserveLength && preserveLengthApplies (o);

    // Whether the caller told us which part of the pattern its clip plays.
    const bool hasWindow = o.windowEnd > o.windowStart;

    bool anyChanged = false;

    for (auto& n : notes)
    {
        // A note with a non-finite position is a fault somewhere else, but it
        // must not become one here: the comparisons the overlap pass sorts by
        // are not an ordering once a NaN is in the vector. Repairing one is a
        // change like any other and is counted as one.
        if (! std::isfinite (n.start))
        {
            n.start = 0.0;
            anyChanged = true;
        }

        if (! std::isfinite (n.length) || n.length <= 0.0)
        {
            n.length = kMinLengthBeats;
            anyChanged = true;
        }

        const double oldStart = n.start;
        const double oldEnd = n.start + n.length;

        double newStart = moveStarts ? place (oldStart, o) : oldStart;
        const double newEnd = moveEnds ? place (oldEnd, o) : oldEnd;

        // Held where it was played rather than pushed out of the clip, where
        // it would be in the pattern, drawn in the roll, and silent. Only a
        // note that was audible is protected: one already outside the window
        // is hidden whatever happens to it.
        if (hasWindow && newStart != oldStart
            && oldStart >= o.windowStart && oldStart < o.windowEnd
            && (newStart < o.windowStart || newStart >= o.windowEnd))
        {
            newStart = oldStart;
            ++result.held;
        }

        // A note cannot start before the pattern does. The clip's own front
        // edge is `windowStart` and is a different thing, handled above.
        if (newStart < 0.0)
            newStart = 0.0;

        // Nothing moved, so nothing is written. Recomputing the length as the
        // difference of two beats that did not change is not the identity in
        // floating point, and a quantise at strength zero has to leave a
        // performance untouched rather than nearly untouched: Apple,
        // Steinberg and Avid all publish that endpoint.
        if (newStart == oldStart && newEnd == oldEnd)
            continue;

        // A kept length is carried across rather than recovered from the two
        // ends, so it is exact. The other way round it cannot be: a note is
        // stored as a start and a length, so holding the end still means
        // storing `newEnd - newStart`, and adding that difference back to the
        // start is not guaranteed to land on the same double. So Avid's "note
        // end points are not moved", quoted above as an exact rule, is exact
        // to within an ulp or two here rather than to the bit. Said rather
        // than left to be discovered, since every other exactness claim in
        // this header is exact.
        const double newLength = keepLength ? n.length : newEnd - newStart;

        n.start  = newStart;
        n.length = std::max (kMinLengthBeats, newLength);
        anyChanged = true;
    }

    // Nothing moved, so nothing is resolved and nothing is even reordered. A
    // quantise that did not move a note has to be indistinguishable from not
    // having been asked for.
    if (! anyChanged)
        return result;

    resolveOverlaps (notes, before, result);
    return result;
}

} // namespace Quantise
