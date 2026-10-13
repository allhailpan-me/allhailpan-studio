#pragma once
#include <algorithm>
#include <cmath>

//==============================================================================
/** How the pan control turns a position into a pair of gains.

    The studio had one behaviour here and no choice about it: a centred signal
    passed untouched and moving off centre attenuated the opposite side in
    proportion. That is a balance control, and as a balance control it is
    right. It is also exactly what Ardour's `stereobalance` panner does, so it
    was never a guess.

    What was missing is the other control. A guitar in one socket of an
    interface is a mono source, which `InputSource.h` centres, so both halves
    of the insert's buffer carry the same signal. Balance that hard left and
    the right half is multiplied by zero, so the power halves and the guitar
    drops 3 dB the moment it is moved off centre. The fader then has to be
    ridden back up, and every mono track in the session has the same problem,
    which for somebody recording guitar and vocals is every track carrying a
    performance.

    Every studio looked at treats these as two controls chosen by what is on
    the track: Logic gives a mono strip a Pan knob and a stereo strip a Balance
    knob, Ardour ships a separate panner for each and lets it be picked from
    the panner's own context menu, Cubase picks from the channel's input and
    output configuration, and Reaper names its default mode after both halves
    at once. So the control is chosen here too, per insert, and balance stays
    the default: it is what the studio has always done, so no existing mix
    moves.

    Kept free of JUCE so the arithmetic can be tested. It is worth testing for
    the reason `LatencyGraph.h` is: nothing here can announce that it is wrong.
    A pan law that is off by a decibel does not error, does not crackle and
    does not look wrong on a meter. It just means a mix balanced in this studio
    is not balanced anywhere else.
*/
namespace PanLaw
{
    /** Which of the two controls an insert's pan knob is.

        `balance` is what the studio has always done and stays the default.
        `pan` places a mono source, and is the one the project's law applies
        to. Logic draws the same line: Balance "differs from Stereo Pan mode in
        that it controls the relative levels of two signals (Left and Right) at
        their outputs", and a law that changed the relative levels of a stereo
        recording would move its image rather than its position.
    */
    enum class Mode
    {
        balance,
        pan
    };

    /** The pan laws Logic's General Audio project settings offer, named the
        way it names them: by the gain each side gets at centre.

        "Compensated" is Logic's word and Logic's definition. A plain -3 dB law
        routes a centred signal "to the left and right output channels with a
        -3 dB level decrease" and the extremes pass "without any level change";
        -3 dB Compensated routes a centred signal at "the same level" and gives
        the extremes "a +3 dB level increase". So the two differ only in where
        unity sits, at the ends or in the middle.
    */
    enum class Law
    {
        zero,
        minus3,
        minus3Compensated,
        minus4point5,
        minus4point5Compensated,
        minus6,
        minus6Compensated
    };

    /** Balance, because it is what every project saved so far was mixed
        through. Changing this would quietly remix every session on disk.
    */
    inline constexpr Mode defaultMode = Mode::balance;

    /** Derived rather than copied, because the studios disagree about
        defaults and the reasoning is short.

        -3 dB Compensated is the only one of the seven that is both flat in
        power across the sweep, so moving a track does not change how loud it
        is, and unity at centre, so switching an insert from balance to pan
        while it sits centred changes nothing at all and therefore cannot
        click. Every other option gives up one of those two.
    */
    inline constexpr Law defaultLaw = Law::minus3Compensated;

    struct Gains
    {
        double left  = 1.0;
        double right = 1.0;
    };

    /** The curve underneath a law, with the compensation taken off.

        Three of these are the three laws Oland and Dannenberg set out in
        their pan law reading for Carnegie Mellon's Introduction to Computer
        Music, and the fourth is what the studio already had.
    */
    enum class Curve
    {
        flat,            /**< full level on both sides at centre, which is Logic's 0 dB law */
        constantPower,   /**< sine and cosine: total power flat at every position */
        compromise,      /**< the square root of the product of the other two */
        linear           /**< straight line: total amplitude flat, so a mono fold keeps level */
    };

    inline Curve curveOf (Law law) noexcept
    {
        switch (law)
        {
            case Law::zero:                     return Curve::flat;
            case Law::minus3:
            case Law::minus3Compensated:        return Curve::constantPower;
            case Law::minus4point5:
            case Law::minus4point5Compensated:  return Curve::compromise;
            case Law::minus6:
            case Law::minus6Compensated:        return Curve::linear;
        }

        // Unreachable: the switch above is exhaustive over Law. Written out
        // rather than calling back into this function so a value from a
        // corrupted file cannot turn a fallback into a recursion.
        return Curve::constantPower;
    }

    inline bool isCompensated (Law law) noexcept
    {
        return law == Law::minus3Compensated
            || law == Law::minus4point5Compensated
            || law == Law::minus6Compensated;
    }

    /** The gain one side gets when the source sits `t` of the way towards it,
        with `t` running 0 at the far extreme to 1 at this one.

        One function for both sides, read at `t` and at `1 - t`, which is what
        makes the two sides exact mirrors of each other rather than mirrors to
        within a rounding error. It also lands the far side on a hard zero: the
        same curve written as a cosine leaves 6e-17 there instead, which is
        inaudible but is the kind of thing that makes a test state an
        approximate fact where it could state an exact one.

        The three real curves are the published ones. Oland and Dannenberg give
        the linear law as `R = theta(2/pi)`, the constant power law as
        `R = sin(theta)` with theta running to pi/2, and the -4.5 dB
        compromise as "the square root of the product of the linear and
        constant power laws". `flat` is not from there: it is the formula the
        studio already used, and it is Logic's 0 dB law written out.
    */
    inline double sideGain (Curve curve, double t) noexcept
    {
        constexpr double halfPi = 1.57079632679489661923;

        switch (curve)
        {
            // The old `min (1, 1 + p)`, with p rewritten as t. Every step of
            // that rewriting is exact in doubles, so a project mixed through
            // the old formula gets the same numbers out of this one.
            case Curve::flat:          return std::min (1.0, 2.0 * t);
            case Curve::constantPower: return std::sin (t * halfPi);
            case Curve::compromise:    return std::sqrt (t * std::sin (t * halfPi));
            case Curve::linear:        return t;
        }

        return std::sin (t * halfPi);
    }

    /** What one side gets at centre, which is the figure a law is named by.

        Read off the curve itself rather than written down, so a compensated
        law divides a value by itself at centre and lands on exactly one.
    */
    inline double centreGain (Curve curve) noexcept
    {
        return sideGain (curve, 0.5);
    }

    /** The two gains for a position, with -1 hard left and 1 hard right.

        A position outside that range is clamped and a position that is not a
        number is read as centre, because this is reached from a saved project
        and from automation. Neither can be trusted to have stayed in range,
        and a gain of NaN does not go quiet: it silences the whole mix from
        that insert onward and gives no clue where it came from.
    */
    inline Gains gains (Mode mode, Law law, double position) noexcept
    {
        if (std::isnan (position))
            position = 0.0;

        position = std::clamp (position, -1.0, 1.0);

        // A balance control takes no law. Logic's pan law applies to its Pan
        // knob, not to its Balance knob, for the reason given on Mode above.
        const Curve curve = mode == Mode::balance ? Curve::flat : curveOf (law);

        // Both sides are worked out the same way from the position, rather
        // than one of them as one minus the other. That is what makes a
        // mirrored position give the mirrored pair down to the last bit: for a
        // position that is not exactly representable, `1 - (1 + p) / 2` and
        // `(1 - p) / 2` round differently, and a panner that is a rounding
        // error off centre at centre is the sort of fault nobody ever finds.
        const double towardsLeft  = (1.0 - position) * 0.5;
        const double towardsRight = (1.0 + position) * 0.5;

        // The mode test is belt and braces: balance mode has already forced
        // the flat curve above, and the flat curve is unity at centre, so
        // dividing by its centre gain would do nothing anyway. It is written
        // out because the condition it states is the one that matters, and a
        // reader should not have to work out that the two agree.
        const double scale = mode == Mode::pan && isCompensated (law) ? centreGain (curve) : 1.0;

        return { sideGain (curve, towardsLeft)  / scale,
                 sideGain (curve, towardsRight) / scale };
    }

    /** What this mode and law leave of a centred signal, in decibels.

        Not always the number in the law's name, and the difference is the
        whole point of the two halves of that name: a compensated law reports
        zero, because unity at centre is what compensated means, and a balance
        control reports zero whatever law is passed, because a balance control
        leaves a centred signal alone. An uncompensated law in pan mode is the
        only case that reports the figure it is named by.

        Worked out rather than written down so the names and the arithmetic
        cannot drift apart.
    */
    inline double centreGainDb (Mode mode, Law law) noexcept
    {
        const auto g = gains (mode, law, 0.0);
        return 20.0 * std::log10 (g.left);
    }

    inline const char* name (Mode mode) noexcept
    {
        return mode == Mode::balance ? "Balance (stereo source)" : "Pan (mono source)";
    }

    inline const char* name (Law law) noexcept
    {
        switch (law)
        {
            case Law::zero:                     return "0 dB";
            case Law::minus3:                   return "-3 dB";
            case Law::minus3Compensated:        return "-3 dB compensated";
            case Law::minus4point5:             return "-4.5 dB";
            case Law::minus4point5Compensated:  return "-4.5 dB compensated";
            case Law::minus6:                   return "-6 dB";
            case Law::minus6Compensated:        return "-6 dB compensated";
        }

        return "-3 dB compensated";
    }

    inline constexpr int numLaws = 7;

    inline Law lawAt (int index) noexcept
    {
        switch (index)
        {
            case 0:  return Law::zero;
            case 1:  return Law::minus3;
            case 2:  return Law::minus3Compensated;
            case 3:  return Law::minus4point5;
            case 4:  return Law::minus4point5Compensated;
            case 5:  return Law::minus6;
            case 6:  return Law::minus6Compensated;
            default: break;
        }

        return defaultLaw;
    }

    // ---- what goes in the project file ----
    //
    // Written as a number rather than a name because the rest of the mixer is.
    // Read back so that only the values actually written can come out: the law
    // through a range check against the list, the mode by naming the one value
    // that means pan. Anything else is a project written by a later version or
    // a corrupted one, and landing on the default leaves a readable session
    // rather than a silent insert.

    inline int modeToStored (Mode mode) noexcept
    {
        return mode == Mode::pan ? 1 : 0;
    }

    inline Mode modeFromStored (int stored) noexcept
    {
        return stored == 1 ? Mode::pan : Mode::balance;
    }

    inline int lawToStored (Law law) noexcept
    {
        switch (law)
        {
            case Law::zero:                     return 0;
            case Law::minus3:                   return 1;
            case Law::minus3Compensated:        return 2;
            case Law::minus4point5:             return 3;
            case Law::minus4point5Compensated:  return 4;
            case Law::minus6:                   return 5;
            case Law::minus6Compensated:        return 6;
        }

        return 2;   // unreachable, and the index of the default if it were not
    }

    inline Law lawFromStored (int stored) noexcept
    {
        if (stored < 0 || stored >= numLaws)
            return defaultLaw;

        return lawAt (stored);
    }
}
