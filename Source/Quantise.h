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

    The clamp is partly published and partly a decision. Apple states the top of
    it: "The most practical settings are between 50% and 75%, which give a
    swing feel to strictly quantized (or tightly played) regions." The bottom
    is this project's, made symmetric about straight so that the pre-delay
    Apple documents has the same room as the delay, and it also keeps the grid
    usable: at 0 or 1 the offbeat lands on a downbeat, the grid has half as many
    distinct points, and "the nearest grid point" becomes a coin toss between
    two identical positions. Apple's whole preset table, 50 to 71, is inside
    it. */
inline constexpr double kMinSwing = 0.25;
inline constexpr double kMaxSwing = 0.75;

/** Converts a Pro Tools or Rosegarden swing percentage to this scale.

    Avid's range runs to 300 per cent, which is 1.0 here, so everything above
    150 per cent lands on `kMaxSwing` once `clampedSwing` has had it. That is
    not a conversion error: it is the clamp above, and the part of Avid's range
    it refuses puts the offbeat within a quarter step of a downbeat. */
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

/** How far from the clip's origin this arithmetic will work.

    A note out there is a fault upstream rather than a position: `ProjectIO`
    will read whatever a file claims, and `MidiTimebase.h` caps its own tick
    arithmetic at `kMidiMaxTick` for exactly this reason. The grid search
    counts pairs in signed integers, and a beat divided by a step can overflow
    that long before a double runs out of range, which is undefined behaviour
    rather than a wrong answer. So a note past the end of what a file could
    hold is left exactly where it is. */
inline constexpr double kMaxBeats = (double) kMidiMaxTick / (double) kMidiExportPpq;

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
    int trimmed = 0;   ///< Of those, the ones shortened because a later note of the same pitch landed inside them.
    int dropped = 0;   ///< Notes lost because another of the same pitch landed on the same onset.
};

/** Whether two notes already overlapped before the pass, by index into the
    before picture. Half open, so two notes that merely abut did not overlap. */
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

    Two onsets that the pass has brought within one export tick of each other
    count as one onset, and the later note wins. That is a decision rather than
    a standard, and worth stating as one: a tick is the finest position this
    project keeps, two strikes of one pitch half a millisecond apart are one
    strike musically, and the alternative is a note nobody can see or select.
    It is not a claim about the file, because a pair that straddles a tick
    boundary would export as two notes. With the restriction above it can only
    fire on a coincidence the quantise itself created, which is the double
    triggered pad collapsed onto one grid point.

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
        if (! kept.empty() && kept.back().note.note == e.note.note
            && ! overlappedBefore (before, kept.back().index, e.index))
        {
            auto& previous = kept.back();

            if (e.note.start - previous.note.start < kMinLengthBeats)
            {
                ++result.dropped;
                continue;
            }

            if (previous.note.start + previous.note.length > e.note.start)
            {
                previous.note.length = e.note.start - previous.note.start;
                previous.trimmed = true;
            }
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
    somebody who presses Quantise twice to be sure must not get a third
    pattern.

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

        // A note cannot start before the clip does.
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
        // ends, so it is exact. The other way round it cannot be: holding the
        // end still means storing `newEnd - newStart`, and adding that back to
        // the start does not always land on `newEnd`. Measured over three
        // million single note cases the end drifts on about one in twelve
        // hundred, by at most 1.8e-15 of a beat, which is a ten thousandth of
        // the finest position this project keeps. Said here because Avid's
        // "note end points are not moved" is quoted above as an exact rule and
        // it is exact to within an ulp rather than to the bit.
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
