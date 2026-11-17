#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "GrooveTemplate.h"

//=============================================================================
// Groove.
//
// Modelled on what hardware samplers and the better studios actually mean by
// these words, rather than on a single "swing" knob.
//
// Swing is the position of the offbeat within its pair, as a percentage: at
// 50 the pair is even and the feel is straight, at 66.67 the offbeat lands on
// the third triplet, which is a full shuffle. That is the definition the MPC
// established and the one other studios and drummers mean by a swing number,
// so a pattern moved between them keeps its feel. Most records sit between
// 52 and 62.
//
// Swing alone is not groove, though. Ableton's grooves carry a resolution the
// groove is measured at, an amount to apply, a velocity effect and a random
// amount, and that combination is what turns a grid into a performance. The
// same four are here.
//
// The other half of it is a feel taken out of a part somebody played, rather
// than described by a percentage. That lives in GrooveTemplate.h, which
// carries the reasoning and the sources; when one is loaded it supplies the
// timing and the accents in place of the swing number, and the amount, the
// velocity and the random knobs go on meaning what they meant. A part has one
// feel, the way a clip in Live has one groove, so the two are not summed.
//
// Nothing below changes the stored notes. Groove is applied when the
// arrangement is handed to the engine, so it can be dialled while playing and
// turned off without having lost anything.
//
// No JUCE, so the arithmetic can be tested on its own: see
// Tests/GrooveTest.cpp. The numbers this produces are a claim about agreement
// with every other studio that reports a swing percentage, which is the kind
// of claim that has to be checked rather than eyeballed.
//=============================================================================

struct Groove
{
    // The note value the groove is measured against. Pairs are formed at this
    // resolution, so a 1/16 groove swings sixteenths and leaves eighths alone.
    enum class Base { eighth = 0, sixteenth, thirtySecond };

    Base   base     = Base::sixteenth;

    /** A feel read out of a performance. Empty unless one has been extracted,
        and when it is not empty it replaces the swing. It is a value rather
        than a pointer so that `Project::capture` keeps it in the undo
        history, `clearAll` resets it and the equality check below sees it,
        all of which come for free from Project holding a Groove by value. */
    GrooveTemplate feel {};

    double swing    = 0.5;    // 0.5 straight, 0.6667 a triplet shuffle
    double amount   = 1.0;    // how much of the timing shift to apply
    // Without a feel loaded, how much the offbeats are softened. With one,
    // how much of the donor's accents is passed on. Different operations, so
    // the one number means two things depending on `feel`, and at zero the
    // donor's accents are not passed on at all.
    double velocity = 0.0;

    // Humanising, in fractions of a step, which is a fraction of the feel's
    // own slot when one is loaded and of `base` when one is not. The two
    // coincide only while the feel was read at the resolution `base` names.
    double random   = 0.0;
    bool   enabled  = false;

    bool operator== (const Groove&) const = default;

    /** The swing settings the interface offers, straight first.

        Here rather than in the channel rack because three places need them
        and two of those are decisions rather than display: which swing a
        choice means, which choice a stored swing is shown as, and whether
        the groove is on at all once a feel is cleared. They were a table
        written out twice, and the third use got a different rule, which is
        how the groove ended up switched on with the box reading "Straight"
        and no control able to turn it off.

        A stored swing need not be one of these: `ProjectIO` admits anything
        from 0.3 to 0.8, so a file can carry 0.6, which the box can only show
        as Straight. `nearestOffered` is what lets a caller put the groove
        into a state the controls can represent, and therefore undo. */
    static constexpr int kNumOfferedSwings = 5;

    static double offeredSwing (int index) noexcept
    {
        constexpr double offered[kNumOfferedSwings] { 0.5, 0.54, 0.58, 0.62, 2.0 / 3.0 };
        return offered[index < 0 ? 0 : index >= kNumOfferedSwings ? kNumOfferedSwings - 1 : index];
    }

    /** Which offered swing a number is closest to. Index 0 is straight, so a
        caller can read "is this a swing at all" off the result. */
    static int nearestOfferedSwing (double swing) noexcept
    {
        if (! std::isfinite (swing))
            return 0;

        int best = 0;

        for (int i = 1; i < kNumOfferedSwings; ++i)
            if (std::abs (swing - offeredSwing (i)) < std::abs (swing - offeredSwing (best)))
                best = i;

        return best;
    }

    /** Beats per step at this groove's resolution. */
    double stepBeats() const noexcept
    {
        switch (base)
        {
            case Base::eighth:       return 0.5;
            case Base::sixteenth:    return 0.25;
            case Base::thirtySecond: return 0.125;
        }
        return 0.25;
    }

    /** Where a note sits once the groove is applied.

        Steps are numbered from the clip's own origin, and every second one is
        an offbeat. An offbeat's position inside its pair is the swing
        fraction, so the shift is the difference between that and the even
        halfway point.

        The random part is derived from the note's own position rather than
        drawn fresh, so a pattern plays the same way twice and an export
        matches what was heard. A groove that wandered on every pass would be
        unusable.
    */
    double place (double beatFromOrigin, int note) const
    {
        if (! enabled)
            return beatFromOrigin;

        // A loaded feel brings its own resolution, which is the one it was
        // read at. Reading it against whatever `base` says now would make a
        // pattern's feel change when somebody moved the channel rack from
        // sixteenths to thirty-seconds, which is the reason GrooveTemplate.h
        // stores the spacing rather than deriving it.
        const bool   byFeel = ! feel.isEmpty();
        const double step = byFeel ? feel.slotBeats() : stepBeats();
        const double index = beatFromOrigin / step;

        // The same guard `GrooveTemplate::extract` carries for the same
        // call. `llround` is undefined past the range of its return type,
        // and a note position is loaded from a project file with no bound on
        // it, so a damaged file reaches this with whatever it likes. Beyond
        // 1e15 steps a double cannot tell one step from the next anyway, so
        // nothing is given up by leaving such a note where it is.
        if (! std::isfinite (index) || std::abs (index) > 1.0e15)
            return beatFromOrigin;

        const auto whole = (std::int64_t) std::llround (index);

        // Only notes sitting on the grid are moved. Anything already played
        // off the grid is left where the performance put it, which is what
        // Base means in the studios this follows.
        //
        // This is Cakewalk's window at its narrowest and the MPC's Window,
        // both of which leave a note outside it alone and both of which
        // expose the width. It is not exposed here, and at 2 per cent of a
        // step it is a test for "this note is on the grid" rather than
        // anything musical. Two things follow, and the second is the reason
        // it has not been widened.
        //
        // It is what makes a song-wide groove safe. The groove is one setting
        // for the whole project rather than one per clip the way Live's is,
        // so a feel read off a part somebody played would otherwise be
        // applied to that same part again, on top of the timing it already
        // has. A narrow window means a feel reaches programmed material,
        // which is what it is for, and leaves performances alone.
        //
        // And it is a step rather than a fade: it decides whether to apply,
        // and what is applied is the whole deviation. A played note that
        // happens to land inside it takes the full shove while its
        // neighbours take none, which on a part played by hand is a few notes
        // in a bar moving and the rest not. The answer is the width as a
        // control with the strength scaling the move, which is what Cakewalk
        // and the MPC both ship and which wants an interface of its own.
        // Recorded in Research/BACKLOG.md rather than half done here.
        if (std::abs (index - (double) whole) > 0.02)
            return beatFromOrigin;

        double shifted = beatFromOrigin;

        // The humanising draw, in fractions of a step, derived from the note's
        // own position rather than drawn fresh so that a pattern plays the
        // same way twice and an export matches what was heard. A groove that
        // wandered on every pass would be unusable.
        //
        // Drawn under the same condition it is spent under, so that a groove
        // with the humanise knob down does exactly what it did before any of
        // this existed, and the hash is not evaluated on every note of every
        // pattern to be multiplied by nothing.
        //
        // The index is widened to unsigned before the multiply because
        // `whole` is signed and the constant does not fit in an int, so the
        // product was signed and overflowed once the index reached 3.5e9.
        // That is undefined behaviour rather than a wrong answer, and a fine
        // enough spacing out of a project file reaches it at an ordinary
        // position. Unsigned wrapping is defined and keeps the same low bits
        // for every index that did not overflow, so nothing that played one
        // way plays another.
        double jitter = 0.0;

        if (random > 0.0)
        {
            std::uint32_t h = (std::uint32_t) ((std::uint64_t) whole * 2654435761ull)
                            ^ (std::uint32_t) (note * 40503u);
            h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
            jitter = (h / 4294967296.0) * 2.0 - 1.0;
        }

        if (byFeel)
        {
            // The deviation the donor played, added to where this note
            // already is, rather than the note being moved onto the donor's
            // position. Which of the two models that is, and why this one, is
            // in GrooveTemplate.h. A slot the donor never played contributes
            // nothing: giving it one would be inventing a feel for a position
            // there is no evidence about.
            //
            // Be clear about how much of that choice is audible here. For a
            // note exactly on the grid the two models give the same answer,
            // because adding the donor's offset to a note at the grid point
            // lands it on the donor's position. They can differ only for a
            // note off the grid, and the window above lets through nothing
            // further off than 2 per cent of a step. So the difference is
            // bounded at a couple of milliseconds, and the model is the shape
            // of the arithmetic rather than something anybody can hear today.
            // The test pins it with a recipient inside the window, which is
            // the one case that separates the two: before that check existed
            // a full target model passed the whole suite.
            const auto   slot      = feel.slotFor (whole);
            const double deviation = slot.filled ? slot.timing * amount : 0.0;

            // The feel and the humanising are bounded together rather than
            // one after the other, which is what keeps two notes on this
            // groove's own grid from crossing each other.
            //
            // A deviation is already up to half a slot either way, which is
            // the whole of the distance between two neighbours: a note pushed
            // to the back of its slot and the next one pulled to the front of
            // its own land on the same beat, which is allowed. Humanising
            // then has nothing left to spend, so added on top it walked one
            // note past the other. Measured before this clamp existed, a note
            // could land 0.42 of a slot behind the note in front of it.
            //
            // What it does not promise, and never could from in here, is that
            // a note cannot pass a note that is not on this grid. Anything
            // more than 0.02 of a step off the grid is left exactly where the
            // performance put it, by the test above, so a note that is moved
            // can overtake a neighbour that is held still. That is true of
            // the swing half as well and always was: `place` sees one note at
            // a time and has nothing to measure a gap against. The guarantee
            // is the one the suite asserts, which is over the notes the
            // groove is actually acting on.
            //
            // The swing half is not bounded this way. At the shuffle the
            // interface offers it moves the odd step of a pair by 0.33 of a
            // step and the humanising fits in what is left, so a pair can
            // meet and not cross. That argument is about what the interface
            // offers: `ProjectIO` admits a swing up to 0.8 from a file, where
            // a pair can cross by a tenth of a step. The crossing is
            // `2 * swing - 1.5` steps, nothing at 0.75 and 0.1 at 0.8, and
            // 0.75 is the last value `GrooveTest` sweeps, so that check
            // cannot see it however many placements it makes. It predates this
            // and
            // is recorded in the backlog rather than changed here, because
            // narrowing what a saved project may contain is not this
            // feature's business.
            //
            // Half a slot is also exactly the bound the test already asserts
            // for a feel on its own, so this makes that true with the
            // humanise knob up rather than only with it at zero.
            shifted += std::clamp (deviation + jitter * random * 0.25, -0.5, 0.5) * step;
        }
        else
        {
            if ((whole & 1) != 0)
            {
                // The pair spans two steps, so the straight offbeat is at one
                // half of it and the swung offbeat is at the swing fraction
                // of it.
                const double pair = step * 2.0;
                shifted += (swing * pair - step) * amount;
            }

            if (random > 0.0)
                shifted += jitter * random * step * 0.25;
        }

        return std::max (0.0, shifted);
    }

    /** What a note is played at once the groove has had its say.

        Without a feel loaded: offbeats are played a little softer, which is
        most of what makes a swung pattern sound played rather than
        programmed.

        With one: the accent the donor put on this slot, measured against the
        donor's own average, added to what the note already had. Measured
        against the average rather than taken whole so that a part played at
        one steady loudness changes nothing, which is the same thing the
        timing does with a donor who played exactly on the grid.

        Live's velocity slider runs to -100 as well, which reverses the
        accents so that the donor's loud hits come out quiet. It is not here,
        because nothing in the interface would reach it and a control nothing
        reaches is the fault this project keeps meeting. */
    float shade (double beatFromOrigin, float noteVelocity) const
    {
        if (! enabled || velocity <= 0.0)
            return noteVelocity;

        const bool   byFeel = ! feel.isEmpty();
        const double step = byFeel ? feel.slotBeats() : stepBeats();
        const double index = beatFromOrigin / step;

        // As in `place` above, and for the same reason.
        if (! std::isfinite (index) || std::abs (index) > 1.0e15)
            return noteVelocity;

        const auto whole = (std::int64_t) std::llround (index);

        if (std::abs (index - (double) whole) > 0.02)
            return noteVelocity;

        if (byFeel)
        {
            const auto slot = feel.slotFor (whole);

            if (! slot.filled)
                return noteVelocity;

            const double accent = (double) slot.velocity - feel.velocityReference();
            return (float) std::clamp ((double) noteVelocity + velocity * accent, 0.02, 1.0);
        }

        if ((whole & 1) == 0)
            return noteVelocity;

        return (float) std::clamp (noteVelocity * (1.0 - velocity * 0.4), 0.02, 1.0);
    }
};
