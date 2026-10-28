#pragma once
#include <algorithm>
#include <cmath>

//==============================================================================
/** What a mixer fader's travel means: the position to gain mapping, its exact
    inverse, and the translation of positions saved under the law this replaces.

    The studio had a law here and nobody chose it. `processInsert` computed
    `(v / 0.8)^2` from a position stored in 0 .. 1.25, which has three faults,
    and the first thing worth saying is which one is not among them.

    **The taper was fine.** Three nights of the backlog recorded "the taper is
    wrong" as the reason to replace this, and measured it is not: `(v / 0.8)^2`
    gives the -20 dB to -6 dB region 25.07 per cent of the travel, against
    28.17 for Ardour and 22.50 for the law below. A squared position is a
    perfectly reasonable fader shape. What was wrong:

    - **Unity sat at 0.640 of the travel**, where every reference puts it
      between 0.75 and 0.782. A third of the fader was spent above 0 dB.
    - **The ceiling was +7.75 dB**, which is 1.25 divided by 0.8 and then
      squared. Not a number anybody picked, and not a number anybody could
      have told you without doing that arithmetic.
    - **There was no floor.** The bottom of the travel reached gain 0 as a
      consequence of the formula rather than as a decision, and no point on
      the scale was written down, so the figure drawn next to the fader could
      not be checked against anything.

    ## The law

    The Behringer X32's: piecewise linear in decibels, in four segments.

        position 0.5    .. 1.0     dB = 40p  - 30      -10 .. +10
        position 0.25   .. 0.5     dB = 80p  - 50      -30 .. -10
        position 0.0625 .. 0.25    dB = 160p - 70      -60 .. -30
        position 0      .. 0.0625  dB = 480p - 90      -90 .. -60
        position 0                 gain 0 exactly      -inf

    Unity is at exactly 0.75 of the travel and the ceiling is +10 dB.

    **Behringer does not publish the law**, and that is said plainly here
    because this project keeps a record of what a present, plausible and
    unsourced number costs. What Behringer does publish is the printed scale
    on the channel strip of the X32 operation manual: +10, +5, 0, -5, -10,
    -20, -30, -40, -50, -60, -inf, on a 100 mm motorised fader.

    The evidence in that is **where those ticks sit along the fader**, and the
    distinction is worth being careful about because the obvious reading of it
    is vacuous. That +10, +5, 0, -5 and -10 are 5 dB apart is a fact about
    which numbers somebody chose to print and says nothing whatever about the
    mapping: any law at all can have those five labels. What a piecewise linear
    in decibels law predicts, and what no smooth law gives, is that they are
    **equally spaced in travel** within a segment and that the spacing changes
    between segments. That is what the artwork shows and it is what
    `FaderLawTest.cpp` asserts.

    So three of the four segments are derived from the manufacturer's own
    artwork. **The fourth is not, and that has to be said plainly.** The
    printed scale runs -50, -60, -inf, so the bottom segment has its upper end
    pinned by the -60 tick and nothing at the other end: -inf gives no slope.
    Its 480 decibels per travel, and `floorDb` being -90 rather than any other
    figure, come only from the two independent open source implementations,
    `jtsage/x32_osc_state` and `bitfocus/companion-module-behringer-x32`, which
    agree to the digit on all four segments. That matters more than it sounds,
    because `floorDb` is the number the decision below about the step down to
    silence rests on, and it is the one constant here with no manufacturer
    document behind it.

    Behringer's own MIDI table corroborates the unity point from a third
    direction, listing "Faders - Value 95 = 0db", where 0.75 * 127 = 95.25.

    ## Why this law and not Ardour's

    Ardour's is `p = ((6 log2(g) + 192) / 198)^8`, in `libs/pbd/pbd/control_math.h`
    rather than in `utils.cc` where it is usually said to live. It is a fine
    law and it was rejected for three specific reasons.

    **Its inverse is a matched pair of transcendentals.** `pow(x, 8)` against
    `pow(x, 1/8)`, which measures a maximum position round trip error of
    1.33e-15 against 1.11e-16 for the law below. Within a segment this law is
    affine in decibels, so it inverts to a floating point unit by
    construction. That matters more here than it looks: a position round trips
    through the project file and through every automation curve, and the
    migration below has to convert an old gain into a new position exactly.

    **Its unity point cannot be drawn.** `(32/33)^8 = 0.7817867535191608`.
    Every tick of the law below lands on a dyadic fraction of the travel: 1/16,
    1/8, 3/16, 1/4, 3/8, 1/2, 5/8, 3/4, 7/8, 1.

    **Its unity point does not survive a different ceiling.** Ardour's ceiling
    is the config file only variable `max-gain`, and
    `gain_to_slider_position_with_max` rescales the **gain**, so asking for
    +10 dB moves unity from 0.782 down to 0.6616. The one virtue it has over
    this law is an accident of its +6.02 dB ceiling.

    And on the authority of any of it, from Ardour's own author: Paul Davis, on
    linux-audio-dev in May 2009, calls Ardour's mapping "rather arbitrary but
    definitely comfortable" and notes that "the specs for various standard
    meters and for various standard faders do not use the same scale".
    **Nothing standardises a fader law.** IEC 60268-17 is the standard volume
    indicator and IEC TR 60268-18 is the digital peak programme meter; neither
    is a fader, and anybody who goes looking for a standard here will find a
    meter and implement the wrong thing.

    Which is not hypothetical. `iec_scale`, the most widely copied function in
    amateur audio code, is a **meter deflection** function: it is `log_meter`
    in Ardour's `libs/ardour/ardour/logmeter.h`, sitting next to
    `meter_deflect_vu`, `_ppm`, `_din`, `_nordic` and `_k`, and it puts unity
    at 100/115 = 0.8696 of the scale with a hard floor returning 0 below
    -70 dB. Used as a fader law it draws a VU needle's travel. It appears
    nowhere in this repository and it should stay that way.

    ## Two things in here that are decisions rather than details

    **The slope is discontinuous at the three breakpoints**, jumping from 40 to
    80 to 160 to 480 decibels per full travel. That is the real console, and it
    is why Behringer's own printed ticks change spacing at -10 and -30. It does
    mean an automation ramp crossing a breakpoint has a kink in its fade rate.
    Written down because the next person to read this will see the kink and
    take it for a bug worth smoothing out, and smoothing it out would be
    leaving the law.

    **The bottom of the travel steps -90 dB to silence.** The continuous part
    reaches -90 dB and position 0 is exact zero, so there is a jump at one
    point. Both references do this: Ardour special cases `pos == 0.0` to return
    0.0 while its continuous part only reaches -192.66 dB, and Behringer prints
    -inf below a scale that stops at -60. The argument for it rather than
    against it is that the step is **about one least significant bit of a 16 bit
    file**: -90 dB is an amplitude of 3.1623e-5, the least significant bit of a
    16 bit file is 1/32768 = 3.0518e-5, which is -90.31 dB, so the step is 1.036
    of one. Not smaller than it. An earlier version of this comment claimed
    smaller, which is the sentence this whole decision rests on and was wrong by
    0.31 dB in the direction that flattered it, found by the test refusing to
    assert it. One bit at the bottom of a 16 bit file is below what any
    converter resolves, so the argument survives being stated correctly, which
    is the only form it is worth having. The
    alternative, bending the bottom segment to approach -inf, was rejected
    because the gain would then never reach zero, and a mixer summing sixteen
    inserts at -70 dB each is not silent. A fader pulled all the way down has
    to give nothing.

    Kept free of JUCE so the whole law and the whole migration can be compiled
    and run under sanitizers. Worth it for the reason `PanLaw.h` and
    `LatencyGraph.h` are: nothing here can announce that it is wrong. A fader
    law a decibel out does not error, does not crackle and looks entirely
    normal on a meter, and a migration that is wrong moves every fader in
    every project somebody has ever saved.

    See Tests/FaderLawTest.cpp.
*/
namespace FaderLaw
{
    /** Unity. Exactly three quarters of the travel, which is the whole reason
        this law was picked over one whose unity point is `(32/33)^8`. */
    inline constexpr double unityPosition = 0.75;

    /** The ends of the travel. A position is a fraction of the fader, so the
        top is 1 rather than the 1.25 the old stored range used. */
    inline constexpr double minPosition = 0.0;
    inline constexpr double maxPosition = 1.0;

    /** The ceiling, +10 dB, and the lowest decibel figure the continuous part
        of the law reaches. Named rather than written as literals at the call
        sites because the interface draws the scale and the tests assert it.

        `floorDb` is where the bottom of the travel sits, not where silence is:
        silence is position 0 and nothing else. */
    inline constexpr double ceilingDb = 10.0;
    inline constexpr double floorDb   = -90.0;

    /** The decibel figure a position means, ignoring silence.

        Split out from `positionToGain` because the interface wants the number
        to print and the test wants to assert the eleven ticks Behringer
        printed, and neither should have to go through a gain and back.

        At position 0 this returns `floorDb` rather than minus infinity: the
        -inf belongs to the gain, where it is exact, and a decibel reading of
        minus infinity is not a thing that can be clamped or drawn. Callers
        that want to show "-inf" should ask whether the gain is zero.
    */
    inline double positionToDb (double position) noexcept
    {
        // A position that is not finite comes from a corrupt project file or
        // from a control that has gone wrong, and the quiet answer is the safe
        // one: a fader that has lost its position should not be louder than
        // the one next to it.
        //
        // This is where a position and a gain are treated differently on a
        // non-finite input, so the rule is worth stating once. **A position is
        // bounded and a gain is not.** The fader has two physical ends, so a
        // position outside them carries no meaning at all and is read as
        // corruption. A gain or a decibel figure has a ceiling here but no
        // upper bound as a quantity, so an infinite one is a limiting case of
        // "louder than the ceiling" and saturates at the top, which is what
        // `dbToPosition` and `gainToPosition` both do.
        //
        // So `positionToDb` of an infinity is the floor while `dbToPosition`
        // of an infinity is the top, and those two are not inverses there.
        // That is deliberate. The alternative, reading an infinite position as
        // the top because the clamp below would have taken it there, means a
        // corrupt project file opens with a strip at full.
        if (! std::isfinite (position))
            return floorDb;

        const double p = std::clamp (position, minPosition, maxPosition);

        // Ordered from the top down because that is where a fader spends its
        // life, and each test is the lower edge of the segment above. The
        // forward direction branches on position and the inverse below
        // branches on decibels, which is the asymmetry worth being careful
        // about: a boundary made inclusive on one side and exclusive on the
        // other is silent everywhere except at the breakpoint itself.
        if (p >= 0.5)    return p * 40.0  - 30.0;
        if (p >= 0.25)   return p * 80.0  - 50.0;
        if (p >= 0.0625) return p * 160.0 - 70.0;
        return p * 480.0 - 90.0;
    }

    /** The position a decibel figure sits at. The exact inverse of
        `positionToDb` over `floorDb` .. `ceilingDb`.

        Anything at or below `floorDb` returns 0, which is the bottom of the
        travel and therefore silence. That is deliberate rather than a clamp
        falling out: asking for -120 dB on a fader whose scale stops at -90 is
        asking for nothing, and the alternative is a position that reads as
        silent and is not.
    */
    inline double dbToPosition (double db) noexcept
    {
        if (! std::isfinite (db))
            return db > 0.0 ? maxPosition : minPosition;

        if (db <= floorDb)   return minPosition;
        if (db >= ceilingDb) return maxPosition;

        if (db >= -10.0) return (db + 30.0) / 40.0;
        if (db >= -30.0) return (db + 50.0) / 80.0;
        if (db >= -60.0) return (db + 70.0) / 160.0;
        return (db + 90.0) / 480.0;
    }

    /** What the audio is multiplied by. This is the one the engine calls.

        Position 0 is exact silence, by its own branch and not by arithmetic.
        See the note above on why the -90 dB step there is the right trade.
    */
    inline double positionToGain (double position) noexcept
    {
        if (! std::isfinite (position) || position <= minPosition)
            return 0.0;

        return std::pow (10.0, positionToDb (position) / 20.0);
    }

    /** Where a gain sits on the travel. The inverse of `positionToGain`.

        A gain of zero, or anything below the floor, is the bottom of the
        fader. A gain above the ceiling saturates rather than extrapolating:
        the fader has a top and a number that cannot be reached on it has to
        come back as the top rather than as something off the end of it.

        An infinite gain is therefore the top, which is worth saying because
        this function used to send it to the bottom. It guarded on
        `! std::isfinite` and so lumped an infinity in with a NaN, which left
        the two entry points to this inverse disagreeing about the same
        request: `dbToPosition` sent plus infinity to the top and this sent an
        infinite gain to the bottom, and neither the comment above nor the rest
        of the header said why. They agree now, and they agree by composition
        rather than by a second special case, because 20 log10 of an infinity
        is an infinity and `dbToPosition` already saturates. A NaN is a
        different thing and still goes quiet: it means the gain carries no
        information, and a control that has lost its value should not come back
        louder than the one next to it.
    */
    inline double gainToPosition (double gain) noexcept
    {
        // A NaN is checked before the logarithm rather than after it. Not
        // because log10 would produce something worse, since dbToPosition
        // handles a NaN itself and gives the same answer, but because the
        // intent is worth being able to read here: a gain that is not a number
        // is the bottom of the fader by decision.
        if (std::isnan (gain) || gain <= 0.0)
            return minPosition;

        return dbToPosition (20.0 * std::log10 (gain));
    }

    //==========================================================================
    // The migration.
    //
    // Everything below exists to translate what is already saved. It is here
    // rather than in ProjectIO so that it can be tested without JUCE, because
    // it is the half of this change that can do real damage: the law above
    // being wrong makes a fader feel odd, and the migration being wrong moves
    // every fader in every project the owner has ever saved.

    /** The travel the old law was stored over, and where unity sat on it.

        Kept as named constants rather than left as literals in the migration,
        because they are the only place either number should still appear and
        because a reader needs to see that 0.8 over 1.25 is the 0.640 the
        header complains about.
    */
    inline constexpr double legacyUnityPosition = 0.8;
    inline constexpr double legacyMaxPosition   = 1.25;

    /** The gain the old law gave a stored position: `(v / 0.8)^2`.

        Preserved exactly as it was, including that it reaches zero at zero and
        that its ceiling is `(1.25 / 0.8)^2 = 2.4414`, which is +7.75 dB. Every
        old gain therefore fits inside the new +10 dB ceiling and no existing
        project loses anything at the top.
    */
    inline double legacyPositionToGain (double legacyPosition) noexcept
    {
        if (! std::isfinite (legacyPosition) || legacyPosition <= 0.0)
            return 0.0;

        const double v = std::min (legacyPosition, legacyMaxPosition) / legacyUnityPosition;
        return v * v;
    }

    /** A position saved under the old law, as a position under this one.

        **Through the gain, never through the fraction of the travel.** That is
        the whole content of this function and it is worth the emphasis,
        because going through the travel fraction looks right and is a silent
        disaster: old unity is 0.640 of the travel and new unity is 0.75, so a
        fader saved at 0 dB would come back at position 0.640, which is -4 dB.
        Every track of every existing project would come back quieter, by an
        amount depending on where its fader happened to be, and it would
        present as "my old mixes all sound wrong and the balance has moved"
        with nothing anywhere in the audio path to blame for it.

        Silence maps to silence exactly, rather than through a logarithm of
        zero.

        One consequence worth stating rather than discovering. The old law had
        no floor, so it could store a gain below -90 dB, and this law's floor
        is -90 dB. An old position below 0.0044987, which is the bottom 0.36
        per cent of the old travel, therefore comes back as exact silence
        instead of as the inaudible figure it was. That is the same trade as
        the step at the bottom of the travel, for the same reason: -90 dB is
        below the least significant bit of a 16 bit file. Above the floor the
        translation is exact, measuring 7.11e-15 dB of error over half a
        million randomised positions.
    */
    inline double legacyToPosition (double legacyPosition) noexcept
    {
        if (! std::isfinite (legacyPosition) || legacyPosition <= 0.0)
            return minPosition;

        return gainToPosition (legacyPositionToGain (legacyPosition));
    }

    /** An automation curve value saved under the old law, as a curve value
        under this one.

        A curve is stored 0 .. 1 and was multiplied by the old travel of 1.25
        to reach a fader position, so an old curve reading of 0.64 meant unity.
        The new travel is 0 .. 1, so a curve value and a position are now the
        same number, and translating the fader without translating the curves
        would leave every drawn fade pointing somewhere else: a curve that sat
        at unity would come back at -4 dB, the same fault as above and harder
        to see because it only sounds wrong where the automation is playing.

        **What this cannot translate is the line between two points, and that
        is a real limitation rather than a rounding.** The two laws interpolate
        differently: a straight legacy segment was linear in position under
        `(v / 0.8)^2`, and a straight segment now is linear in decibels, which
        is the shape a fade is supposed to have and is the better of the two.
        So a migrated fade keeps both of its ends exactly and changes shape in
        between, by an amount that depends on how far it travels:

            0 dB to -3 dB      0.065 dB at worst
            0 dB to -10 dB     0.716 dB
            0 dB to -20 dB     0.570 dB
            0 dB to -60 dB     13.600 dB, worst at 87 per cent through
            0 dB to silence    7.959 dB at the midpoint, 15.918 at three quarters

        A short fade is therefore untouched and a long fade out is noticeably
        quicker to drop away. `point.bend` is carried across untranslated for
        the same reason, so a bent legacy segment is reshaped twice.

        Subdividing a legacy segment on load would hold the old path, and it
        was not done. It would mean turning somebody's two point fade into a
        nine or seventeen point one, so the gesture they drew stops being
        editable as the gesture they drew, and it needs its own care around the
        tail of a fade to silence, where the old path runs to minus infinity
        and the new one stops at -90 dB. That is a night's work with its own
        test, not a line inside a loader, and it is recorded in the backlog as
        such. What is not acceptable is leaving it unsaid, which is why it is
        here, in the backlog and in the release notes: a fade that changed
        shape with nothing to say so is exactly the fault this header is
        otherwise built to avoid. `FaderLawTest.cpp` asserts the figures above
        so they cannot drift.
    */
    inline double legacyCurveValueToPosition (double legacyCurveValue) noexcept
    {
        if (! std::isfinite (legacyCurveValue) || legacyCurveValue <= 0.0)
            return minPosition;

        return legacyToPosition (std::min (legacyCurveValue, 1.0) * legacyMaxPosition);
    }
}
