#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

//=============================================================================
// A groove taken from a performance.
//
// `Groove.h` is the parametric half: a swing percentage, a resolution, and an
// amount. It can make a programmed pattern shuffle. What it cannot do is make
// one sit the way a part somebody actually played sits, because the thing that
// makes a performance sound played is not a single number. This is the other
// half: read the timing and the accents out of one part, and put them on
// another.
//
// Modelled on Ableton's Groove Pool, which is the reference implementation of
// the idea, with the places it departs from Live written down below. No JUCE,
// so all of it is tested: see Tests/GrooveTemplateTest.cpp.
//
//-----------------------------------------------------------------------------
// The one question the studios answer differently
//
// Applying a groove can mean two things, and which one it means is the whole
// feature rather than a detail.
//
// Pro Tools, Cubase, Logic and Cakewalk are target based. The template is a
// grid of positions, the note is moved onto it, and the percentage is how far
// along that journey it travels. Cakewalk states the arithmetic outright: "a
// strength of 50 percent means that all notes are moved half-way towards the
// desired position".
//
// Ableton is deviation based. Each groove note is first reduced to its offset
// from its own nearest grid point, and that offset is added to where the clip
// note already is. The sentence in the Live 12 manual that settles it is not
// the one about moving notes "proportionally towards the positions of the
// groove notes", which reads as target based; it is the one after it: "Notes
// in the groove that fall exactly on the grid aren't moved at all, so the
// corresponding notes in your clips will also not be moved." Under a target
// model a clip note sitting off the grid, with a groove note sitting on the
// grid, would be pulled onto the grid. Live says it stays where it is, which
// is only true if what the groove contributes is a deviation, zero in that
// case.
//
// Deviation is what this file does. A target model tightens a part that was
// played onto the donor's timing and throws away its own, which is a quantiser
// with a crooked grid rather than a groove, and it does that on exactly the
// material the feature exists for. The two are not exclusive and nothing is
// given up by choosing: a straight quantise to the grid followed by the full
// deviation lands the note on the template's position, which is the target
// model exactly. That is why Live carries a separate Quantize amount and says
// it is "applied before the groove is applied". This studio's quantiser is
// Quantise.h, and asking for it is a separate decision rather than a second
// thing in here that straightens notes.
//
// How much of that choice is audible, said plainly because the paragraphs
// above read as though a great deal turns on it: for a note exactly on the
// grid the two models are identical, since adding the donor's offset to a
// note at the grid point puts it on the donor's position. They differ only
// for a note that is itself off the grid, and `Groove::place` applies a feel
// only to notes within 2 per cent of a step of the grid, for the reasons
// written there. So in this studio today the difference is bounded at a
// couple of milliseconds. It is written down, and pinned by a test that can
// fail, because it is what the feature would turn on if that window ever
// became the control Cakewalk and the MPC both expose, and because a choice
// nobody recorded is one somebody silently reverses. It was not pinned at
// first, and a full target model passed every check in the suite.
//
//-----------------------------------------------------------------------------
// What is stored, and why it is stored that way
//
// A slot per grid position: whether the donor played anything there, how far
// off the grid it was, and how hard it was hit.
//
// The timing is a deviation rather than a position. Every published format
// that says what it holds holds offsets: the DNA templates that Logic, Cubase
// and Pro Tools all read store "offsets from the quantize points", and Live
// derives its offsets from the Base setting at the moment it applies them. A
// deviation also carries no tempo and no absolute time, so a template survives
// a tempo change without meaning something different afterwards.
//
// The unit is a fraction of a slot, not ticks. Every tick figure in the field
// belongs to one host and no two agree: 192 per quarter for DNA, 480 for
// Cubase, 960 for the Feel Injector templates, 3840 to a 4/4 bar for Logic.
// They exist because those hosts were integer sequencers. This one is beat
// based doubles throughout, the way WarpMap.h and CompModel.h are, so a
// fraction of a slot is both exact and the only unit that needs no conversion
// to use.
//
// The slot spacing is stored with the slots rather than read from whatever the
// groove's Base says now. This is a deliberate departure from Live, which
// stores the groove as note positions and re-measures them against the current
// Base, so that changing Base re-reads the same groove at a different
// resolution. That is a nice property and it is not safe here: this studio's
// base follows the channel rack's step resolution, so re-measuring would mean
// the feel of a pattern changing silently when somebody switched the rack from
// sixteenths to thirty-seconds. Extracting again is one click, and a template
// that quietly means something else after an unrelated setting moves is the
// shape of fault this project keeps meeting.
//
// Velocity is stored absolute and applied as a deviation from the template's
// own mean, which is Live's model. The symmetry with the timing is exact and
// worth keeping: a groove note on the grid moves nothing, and a slot hit at
// the template's own average loudness accents nothing. Storing the absolute
// rather than the deviation keeps the information, so the mean can be
// recomputed and the template can be read back as a part.
//
// Note lengths are not stored. Pro Tools and Cubase carry them, Logic carries
// them only through a separate mechanism, Live does not carry them at all, and
// the DNA documentation says outright that its templates do not. It is the
// least agreed of the dimensions and the least audible.
//
//-----------------------------------------------------------------------------
// The invariant
//
// `reference` is the mean velocity of the filled slots, and the application
// arithmetic subtracts it on every note. It is held rather than recomputed
// because it is read once per note, and a held value derived from other
// members is a value that can go stale. So the slots are private and the only
// two ways to set them, extracting and parsing, both go through `setSlots`,
// which is the one place that computes it. That is the same discipline
// WarpMap.h applies to its markers, and for the same reason. The test asserts
// the invariant directly rather than trusting the comment.
//=============================================================================

class GrooveTemplate
{
public:
    /** One grid position of the donor performance. */
    struct Slot
    {
        bool   filled   = false;  // whether the donor played anything here
        double timing   = 0.0;    // deviation from the slot, in fractions of a slot, [-0.5, 0.5]
        float  velocity = 0.0f;   // 0 to 1, absolute, and meaningless unless filled

        bool operator== (const Slot&) const = default;
    };

    /** A note of the part being read. Positions are beats from the part's own
        origin, which is where Groove::place counts from too. */
    struct SourceNote
    {
        double beat     = 0.0;
        float  velocity = 0.0f;
        int    pitch    = 0;
    };

    /** No published standard fixes a maximum: the DNA templates are two bars,
        and Cakewalk says a groove pattern "can be as short or long as you
        like". This cap is here so that a corrupt or hostile project file
        cannot ask for an unbounded allocation. 512 slots is eight bars of
        thirty-seconds in 4/4, which is longer than anybody plays a feel. */
    static constexpr int kMaxSlots = 512;

    /** Reads the feel out of a part.

        `lengthBeats` is the length of the part rather than the position of its
        last note, and the caller passes it because the caller is what knows
        where the bar lines are. It matters: a template whose length is taken
        from the last note of a pattern that ends with a rest is short by that
        rest, so it wraps early and the feel walks forward by a slot every
        repetition. The channel rack passes the pattern's length, which is
        already a whole number of bars.

        Where two notes land in the same slot, the first one wins and the rest
        are ignored. That is Logic's rule, stated for notes at the same
        position: "only the first will be evaluated for the groove template".
        A slot is wider than a position, so the rule is extended to the slot
        here. The reason it is needed at all is that without it the slot's
        timing depends on which voice of a spread chord the loop happened to
        reach last, which is a template that is not a function of its input.
        "First" is earliest, then lowest in pitch, then quietest, which orders
        any two notes that are not identical in all three and so makes the
        answer independent of the order the notes arrive in. Pitch alone was
        not enough: two notes at one beat and one pitch kept their input
        order under a stable sort, so pasting a note onto itself or importing
        a MIDI file with a doubled note gave one of two templates depending
        on which copy came first. Notes identical in all three are
        indistinguishable, so either one is the same answer.
    */
    static GrooveTemplate extract (std::vector<SourceNote> notes,
                                   double lengthBeats,
                                   double slotBeats)
    {
        GrooveTemplate t;

        if (! (slotBeats > 0.0) || ! (lengthBeats > 0.0)
            || ! std::isfinite (slotBeats) || ! std::isfinite (lengthBeats))
            return t;

        const double wanted = std::floor (lengthBeats / slotBeats + 0.5);
        if (! (wanted >= 1.0))
            return t;

        const int count = (int) std::min (wanted, (double) kMaxSlots);

        std::vector<Slot> built ((std::size_t) count);

        // Dropped before the sort rather than after it. A comparator that
        // sees a NaN reports it neither less nor greater, so NaN compares
        // equivalent to everything while the real positions stay ordered,
        // and equivalence that is not transitive makes `std::stable_sort`
        // undefined rather than merely unhelpful. Nothing went wrong with it
        // in practice and the suite fed it two of them.
        notes.erase (std::remove_if (notes.begin(), notes.end(),
                                     [slotBeats] (const SourceNote& n)
                                     {
                                         return ! std::isfinite (n.beat)
                                             || std::abs (n.beat / slotBeats) > 1.0e15;
                                     }),
                     notes.end());

        std::stable_sort (notes.begin(), notes.end(),
                          [] (const SourceNote& a, const SourceNote& b)
                          {
                              if (a.beat != b.beat)
                                  return a.beat < b.beat;
                              if (a.pitch != b.pitch)
                                  return a.pitch < b.pitch;
                              return a.velocity < b.velocity;
                          });

        for (const auto& note : notes)
        {
            // Both guards are above, before the sort. Beyond 1e15 slots a
            // double cannot tell one slot from the next anyway, and llround
            // is undefined past the range of its return type.
            const double u = note.beat / slotBeats;
            const auto raw = (std::int64_t) std::llround (u);

            // A note outside the template is dropped rather than wrapped
            // round into it, and this is load bearing rather than tidiness.
            //
            // `count` is capped at kMaxSlots, so a donor longer than that is
            // read only as far as the cap reaches. Wrapping instead took the
            // far end of the performance and laid it into the slots the near
            // end had left empty, which is a feel assembled out of two
            // different places in the part and offered as one. Measured on a
            // 24 bar donor read at thirty-seconds, slot 2 came back carrying
            // bar 17's deviation and bar 17's accent while bars 1 to 16 never
            // played that slot at all, with nothing on screen to say so. A
            // recorded take longer than the cap is ordinary, and reading the
            // feel out of a recorded take is the whole feature.
            //
            // So the template is the first `count` slots of the donor, which
            // is a feel somebody really played.
            //
            // `extract` and `fromString` still differ about an oversized
            // input on purpose, and only about what to keep: extraction reads
            // a long donor as far as the cap reaches, because a take longer
            // than the cap is ordinary and the near end of it is a real feel,
            // while a file claiming more slots than the cap is refused
            // outright, because its slot count is part of what it is saying
            // and a template read at the wrong length is not that template.
            // What they agree about, now, is that neither folds one part of a
            // performance into another.
            if (raw < 0 || raw >= (std::int64_t) count)
                continue;

            Slot& slot = built[(std::size_t) raw];

            if (slot.filled)
                continue;

            slot.filled   = true;
            slot.timing   = u - (double) raw;
            slot.velocity = std::isfinite (note.velocity)
                          ? std::clamp (note.velocity, 0.0f, 1.0f) : 0.0f;
        }

        t.setSlots (slotBeats, std::move (built));
        return t;
    }

    /** Reads a feel out of the stretch of a part that one clip plays.

        This is here, with the rest of the arithmetic and the tests, rather
        than in the channel rack where it was first written. It is the one
        calculation that decides whether the donor's grid and the recipient's
        are the same grid, and a wrong answer from it does not announce
        itself: the feel comes back looking perfectly reasonable and measured
        against something nobody played to. `RecordAlign.h` exists for the
        same reason, and `CLAUDE.md` records what the version of that figure
        which lived in the window cost: "the fourth thing wrong with it is
        why: the figure was in the window, where nothing tests it".

        `notes` are the part's own notes, positioned from the part's origin.
        `clipOffset` is how far into the part this clip starts and
        `clipLength` is how much of it the clip plays, which is what a clip
        carries.

        The donor is measured on the part's own grid, with no anchor taken off
        it, and that is the whole of why the two grids agree: the slot a note
        lands in here is `round (beat / slot)` folded by the template's
        period, which is character for character the slot `Groove::place`
        consults for a recipient note at the same position. Anything else has
        to be kept in step with `place` by hand.

        An earlier version subtracted the clip's offset, which put the two
        grids a fraction apart whenever the offset was not a whole number of
        slots: a part played late off a clip split at an unsnapped beat came
        back carrying the split's fraction rather than the performance's feel.
        Rounding that offset to the nearest slot does not fix it either, which
        is what the test for this found: the donor's deviation is its distance
        from the part's grid, and shifting the measurement by a whole number
        of slots cannot change that distance, so an offset of 0.62 beats still
        read a part played 0.08 of a slot late as one played 0.44 early. The
        fraction has to come out of the arithmetic, not be rounded away.

        Only the stretch the clip plays is read, which is Ableton's rule for
        extraction: a groove "will only consider the material in the playing
        portion of the clip". The template is as long as the clip rather than
        as its last note, so a part that ends with a rest still repeats on
        the bar instead of walking forward by that rest every time round.
    */
    static GrooveTemplate fromClip (std::vector<SourceNote> notes,
                                    double clipOffset,
                                    double clipLength,
                                    double slotBeats)
    {
        if (! (slotBeats > 0.0) || ! (clipLength > 0.0)
            || ! std::isfinite (slotBeats) || ! std::isfinite (clipLength)
            || ! std::isfinite (clipOffset))
            return {};

        const double wanted = std::floor (clipLength / slotBeats + 0.5);

        if (! (wanted >= 1.0))
            return {};

        const int count = (int) std::min (wanted, (double) kMaxSlots);

        // A clip needing more slots than the cap is read only as far as the
        // cap reaches, by shortening the window rather than by folding what
        // is past it into the template. Folding laid the far end of the
        // performance into slots the near end had left empty: a 24 bar take
        // read at thirty-seconds came back with bar 17's deviation and accent
        // in a slot bars 1 to 16 never played, offered as one feel.
        const double span  = (double) count * slotBeats;
        const double until = clipOffset + std::min (clipLength, span);

        std::vector<SourceNote> kept;
        kept.reserve (notes.size());

        for (auto n : notes)
        {
            if (! std::isfinite (n.beat) || n.beat < clipOffset || n.beat >= until)
                continue;

            const double u = n.beat / slotBeats;

            if (std::abs (u) > 1.0e15)
                continue;

            const auto idx = (std::int64_t) std::llround (u);

            // Folded by the template's own period, which is what makes a
            // template shorter than the part repeat across it, and is the
            // same fold `slotFor` applies when the feel is read back. The
            // deviation is untouched by the fold, because a whole number of
            // slots is what is taken off.
            n.beat = ((double) wrapIndex (idx, count) + (u - (double) idx)) * slotBeats;

            kept.push_back (n);
        }

        return extract (std::move (kept), span, slotBeats);
    }

    bool   isEmpty()           const noexcept { return slots.empty(); }
    int    numSlots()          const noexcept { return (int) slots.size(); }
    double slotBeats()         const noexcept { return spacing; }
    double lengthBeats()       const noexcept { return spacing * (double) slots.size(); }

    /** What a slot's velocity is measured against: the mean of the filled
        slots. A template whose hits are all the same loudness has no accent
        pattern in it, and subtracting the mean is what makes that come out as
        no change rather than as a level shift. */
    double velocityReference() const noexcept { return reference; }

    /** The slot a grid position falls in, wrapping so that the template
        repeats. The index is the note's own grid step counted from the part's
        origin, which can be negative for a note dragged in front of it, and
        C++ leaves `%` negative for a negative left operand, so the wrap is
        written out rather than assumed. */
    Slot slotFor (std::int64_t gridStep) const noexcept
    {
        if (slots.empty())
            return {};

        return slots[(std::size_t) wrapIndex (gridStep, (int) slots.size())];
    }

    /** The template as text, for the project file.

        Integers throughout, in millionths. Not because the precision is
        needed, a millionth of a sixteenth note is a few nanoseconds, but
        because the alternative is printing and parsing doubles, and the C
        library does that through the locale: a studio running in a locale
        that writes a decimal comma would save a file that every other locale
        reads as zero. Integers have no such hole, and the rounding is far
        below anything the arithmetic cares about.

        Empty slots are written as a dash rather than as a zero deviation,
        because the two mean different things: the donor played nothing there,
        which is not the same as having played it exactly on the grid. */
    std::string toString() const
    {
        if (slots.empty())
            return {};

        std::string out = std::to_string ((long long) std::llround (spacing * kScale));

        for (const auto& s : slots)
        {
            out += ' ';

            if (! s.filled)
            {
                out += '-';
                continue;
            }

            out += std::to_string ((long long) std::llround (s.timing * kScale));
            out += ':';
            out += std::to_string ((long long) std::llround ((double) s.velocity * kScale));
        }

        return out;
    }

    /** Reads back what `toString` wrote. Anything malformed gives an empty
        template rather than a half read one: a feel that is partly somebody
        else's is worse than no feel, because nothing about it looks wrong. */
    static GrooveTemplate fromString (const std::string& text)
    {
        GrooveTemplate t;

        std::size_t i = 0;
        long long   spacingMicros = 0;

        skipSpaces (text, i);

        if (! readInt (text, i, spacingMicros) || spacingMicros <= 0)
            return t;

        std::vector<Slot> built;

        while (true)
        {
            skipSpaces (text, i);

            if (i >= text.size())
                break;

            if ((int) built.size() >= kMaxSlots)
                return {};

            Slot s;

            if (text[i] == '-' && (i + 1 >= text.size() || text[i + 1] == ' '))
            {
                ++i;
            }
            else
            {
                long long timingMicros = 0, velocityMicros = 0;

                if (! readInt (text, i, timingMicros))
                    return {};

                if (i >= text.size() || text[i] != ':')
                    return {};

                ++i;

                if (! readInt (text, i, velocityMicros))
                    return {};

                s.filled   = true;
                s.timing   = (double) timingMicros / kScale;
                s.velocity = (float) ((double) velocityMicros / kScale);
            }

            built.push_back (s);
        }

        if (built.empty())
            return t;

        t.setSlots ((double) spacingMicros / kScale, std::move (built));
        return t;
    }

    bool operator== (const GrooveTemplate&) const = default;

private:
    static constexpr double kScale = 1.0e6;

public:
    /** What a slot spacing is allowed to be, because a project file is text on
        a disk and `fromString` will otherwise read whatever is in it.

        The channel rack produces 0.125, 0.25 or 0.5 beats, an eighth to a
        thirty-second, and these bounds are deliberately wider than that so a
        resolution the rack grows later is not refused: a 256th note at the
        bottom and a 4/4 bar at the top. Outside them the number is not a
        musical grid at all, and both ends did real damage.

        What the bounds are for is that a slot has to be a grid somebody
        could play to, because half of one is how far this groove is then
        licensed to move a note. At ten beats a slot it was licensed to move
        a note five beats, which is not a groove by any definition: it threw
        the downbeat of a bar past the next four notes. At a millionth of a
        beat the slots are below the resolution anything else here works in.

        What the bounds are **not** is the thing that stops a pattern
        reordering itself, and it is worth being plain about that because an
        earlier version of this comment claimed it was. The bound in
        `Groove::place` is half a slot whatever the slot is, so two notes on
        this groove's own grid cannot cross at any spacing in or out of this
        range, and a note that is moved can pass a neighbour that is held
        still off the grid at every spacing including the rack's own. That is
        a property of applying a groove one note at a time, it is true of the
        swing half as well, and `Groove::place` says so where it happens.

        Out of range is refused rather than clamped, which is `fromString`'s
        stance everywhere else: a feel read at the wrong resolution is worse
        than no feel, because nothing about it looks wrong. */
    static constexpr double kMinSlotBeats = 1.0 / 64.0;
    static constexpr double kMaxSlotBeats = 4.0;

private:

    /** The only way the slots are ever set, so that `reference` cannot
        disagree with them. */
    void setSlots (double spacingBeats, std::vector<Slot> s)
    {
        spacing = spacingBeats;
        slots   = std::move (s);

        double sum = 0.0;
        int    hits = 0;

        for (auto& slot : slots)
        {
            // A deviation beyond half a slot means the note belonged to the
            // next slot along, so it cannot arise from extraction and can only
            // arrive from a file. Clamped rather than rejected: the worst a
            // clamped deviation does is move a note to the slot boundary.
            //
            // This is the one gate for both numbers, which is why `extract`
            // and `fromString` can both hand over whatever they read and why
            // the bounds are asserted of a parsed template and not only of an
            // extracted one.
            slot.timing = std::isfinite (slot.timing) ? std::clamp (slot.timing, -0.5, 0.5) : 0.0;
            slot.velocity = std::isfinite (slot.velocity) ? std::clamp (slot.velocity, 0.0f, 1.0f) : 0.0f;

            // An empty slot carries no deviation and no accent, so that
            // "the donor played nothing here" is one state rather than two.
            //
            // No path can currently hand over an unfilled slot with anything
            // in it: `extract` default constructs its vector and writes
            // `timing` only where it writes `filled`, and `fromString`'s dash
            // branch default constructs a `Slot`. So this is defence in depth
            // and not a guard that fires, and deleting it changes nothing
            // today. It is here so that the invariant is a property of this
            // function rather than of `Slot`'s default member initialisers,
            // which is what a later path that writes one field and forgets
            // the other would otherwise depend on.
            if (! slot.filled)
            {
                slot.timing   = 0.0;
                slot.velocity = 0.0f;
            }

            if (slot.filled)
            {
                sum += (double) slot.velocity;
                ++hits;
            }
        }

        // A template with nothing in it is not a template. Emptying it here
        // means "is there a feel loaded" is one question rather than two, and
        // nothing downstream has to cope with a template that is present and
        // inert.
        if (hits == 0 || ! (spacing >= kMinSlotBeats) || ! (spacing <= kMaxSlotBeats)
            || ! std::isfinite (spacing))
        {
            slots.clear();
            spacing   = 0.25;
            reference = 0.0;
            return;
        }

        reference = sum / (double) hits;
    }

    static int wrapIndex (std::int64_t index, int count) noexcept
    {
        const auto n = (std::int64_t) count;
        return (int) (((index % n) + n) % n);
    }

    static void skipSpaces (const std::string& s, std::size_t& i) noexcept
    {
        while (i < s.size() && s[i] == ' ')
            ++i;
    }

    /** Written out rather than using strtoll so that nothing here can be
        changed by the locale, and so that a malformed file is reported rather
        than throwing out of a project load. */
    static bool readInt (const std::string& s, std::size_t& i, long long& out) noexcept
    {
        bool negative = false;

        if (i < s.size() && (s[i] == '-' || s[i] == '+'))
        {
            negative = s[i] == '-';
            ++i;
        }

        const std::size_t start = i;
        long long value = 0;

        while (i < s.size() && s[i] >= '0' && s[i] <= '9')
        {
            if (value > 1'000'000'000'000LL)
                return false;

            value = value * 10 + (long long) (s[i] - '0');
            ++i;
        }

        if (i == start)
            return false;

        out = negative ? -value : value;
        return true;
    }

    double            spacing   = 0.25;
    std::vector<Slot> slots;
    double            reference = 0.0;
};
