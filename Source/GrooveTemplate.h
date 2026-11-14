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
        "First" is earliest, with the lower pitch winning a tie, so the answer
        does not depend on the order the notes arrive in either.
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

        std::stable_sort (notes.begin(), notes.end(),
                          [] (const SourceNote& a, const SourceNote& b)
                          {
                              if (a.beat != b.beat)
                                  return a.beat < b.beat;
                              return a.pitch < b.pitch;
                          });

        for (const auto& note : notes)
        {
            if (! std::isfinite (note.beat))
                continue;

            const double u = note.beat / slotBeats;

            // Beyond this a double cannot tell one slot from the next, and
            // llround is undefined past the range of its return type.
            if (std::abs (u) > 1.0e15)
                continue;

            const auto raw = (std::int64_t) std::llround (u);
            const int  k   = wrapIndex (raw, count);

            Slot& slot = built[(std::size_t) k];

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
            // Everything reading a slot already asks `filled` first, and this
            // is what makes forgetting to ask harmless rather than a note
            // moved by a number nobody set.
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
        if (hits == 0 || ! (spacing > 0.0) || ! std::isfinite (spacing))
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
