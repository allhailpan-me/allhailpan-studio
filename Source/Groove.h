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
    double velocity = 0.0;    // how much offbeats are softened, 0 to 1
    double random   = 0.0;    // humanising, in fractions of a step
    bool   enabled  = false;

    bool operator== (const Groove&) const = default;

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
        const auto   whole = (std::int64_t) std::llround (index);

        // Only notes sitting on the grid are moved. Anything already played
        // off the grid is left where the performance put it, which is what
        // Base means in the studios this follows.
        if (std::abs (index - (double) whole) > 0.02)
            return beatFromOrigin;

        double shifted = beatFromOrigin;

        if (byFeel)
        {
            // The deviation the donor played, added to where this note
            // already is. Which of the two models that is, and why this one,
            // is in GrooveTemplate.h. A slot the donor never played
            // contributes nothing: giving it one would be inventing a feel
            // for a position there is no evidence about.
            const auto slot = feel.slotFor (whole);

            if (slot.filled)
                shifted += slot.timing * step * amount;
        }
        else if ((whole & 1) != 0)
        {
            // The pair spans two steps, so the straight offbeat is at one half
            // of it and the swung offbeat is at the swing fraction of it.
            const double pair = step * 2.0;
            shifted += (swing * pair - step) * amount;
        }

        if (random > 0.0)
        {
            std::uint32_t h = (std::uint32_t) (whole * 2654435761u) ^ (std::uint32_t) (note * 40503u);
            h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
            const double jitter = (h / 4294967296.0) * 2.0 - 1.0;
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
        const auto   whole = (std::int64_t) std::llround (index);

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
