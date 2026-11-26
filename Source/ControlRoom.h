#pragma once

#include <algorithm>
#include <cmath>

/*
    The control room: what the speakers do with the finished mix, as opposed to
    what the mix is.

    A mono fold for checking mono compatibility, a dim, a cut per leg and a
    polarity invert per leg. It sits on the speaker feed and on nothing else:
    not on the bounce, not on the loudness and true peak measurement, not on a
    take. That is the whole contract and the engine applies this in exactly one
    place so there is one place to check it.

    It is called the control room rather than the monitor section, which is
    what Ardour and SSL call it, because Steinberg's name for it is Control
    Room and `Monitor` is already taken in this engine for input monitoring.
    Those are different features: one decides whether a player hears
    themselves while tracking, this one decides what the speakers do with the
    mix. Two things called Monitor in one engine is a collision waiting to
    confuse somebody reading `mixMonitorInput` next to a mono fold.

    Why it exists at all: there was no way to hear this mix in mono, and a
    mono fold is the check that finds a polarity problem between two mics on a
    cabinet or on a doubled vocal. The difference listen that falls out of a
    per leg invert is the louder half of the same measurement. The fold tells
    you what survives; the difference tells you what is being destroyed, and
    the second is much easier to hear, because a wide pad that half
    disappears in a fold is faint there and obvious in the difference.

    THE FOLD IS 0.5 PER LEG. `out = 0.5 * (L + R)`, written identically to
    both legs.

    That is what the one readable reference implementation does: Ardour's
    `MonitorProcessor::run` scales by `1/chn`, which is exactly 0.5 for a
    stereo path, with no compensation of any kind and no option to change it
    (monitor_processor.cc:325-361, read for behaviour at HEAD dd32a199e5;
    Ardour is GPL-2.0-or-later and nothing here is copied from it). The
    Digidesign C|24 Guide, chapter 7, documents its Mono switch as lowering
    the monitor output by a default of 6 dB, which is the same number from a
    shipping console. The SSL AWS 900+ SE and Duality Delta manuals, the
    PreSonus Monitor Station V2 manual and Magix Sequoia all describe the sum
    and publish no coefficient, analogue sums having none to publish.

    Three reasons it is the right number and not only the common one:

    Centred content comes through at exactly the same level, so what changes
    when the fold goes in is only what actually changed. That is what the
    button is for: judging centred material against panned material.

    It cannot overflow. `|0.5 * (L + R)| <= 1` whenever both legs are inside
    one, which is the overload ATSC A/52:2018 section 7.8.2 writes its own
    factor of 1/2 into the standard to prevent.

    With a polarity invert on one leg it gives exactly `0.5 * (L - R)`, so
    the two buttons together produce the textbook mid and side signals rather
    than something off by a factor. The convention here is therefore the
    sum and difference halves one, `M = (L + R) / 2` and `S = (L - R) / 2`,
    whose inverse is `L = M + S` and `R = M - S`. Named explicitly because
    there is a second convention in circulation, the orthonormal
    `M = (L + R) / sqrt(2)`, which is energy preserving and self inverse and
    puts correlated content up 3.01 dB.

    WHERE 0.7071 COMES FROM, since it is the number somebody would reach for
    from memory and it is wrong here. ITU-R BS.775-3, Annex 4, Section 1,
    Table 2 gives a mono row over (L, R, C, LS, RS) of
    (0.7071, 0.7071, 1.0, 0.5, 0.5). That is a 3/2 delivery downmix, and its
    normalisation is chosen for power across mostly decorrelated channels
    rather than for level continuity on a correlated centre image. BS.775
    contains no two channel stereo to mono equation at all. Used on a stereo
    speaker feed it puts a correlated full scale mix 3.01 dB into clipping,
    which is a control room button that distorts the thing it is there to
    let you judge.

    THE PER LEG CONTROLS GO BEFORE THE FOLD, which is a real decision and not
    an implementation detail. Ardour applies cut, dim, polarity and solo in
    its per channel stage and folds after it (monitor_processor.cc:301-323
    then 325-361), so cut left plus mono gives `0.5 * R` on both legs rather
    than `R` on both. It is invisible from the UI, so the test pins it. The
    other order is defensible and is Sequoia's idiom, where LL sends the left
    input to both outputs; Ardour's order is the one that makes mono mean
    "the sum of what is currently going to the speakers", which is the
    question the button is asked.

    THE WHOLE SECTION IS LINEAR, so it resolves to four coefficients and the
    engine applies a matrix rather than a sequence of cases. That is not
    only tidier: it means the thing the audio thread does per sample is four
    multiplies whose values can be slewed, so a button is not a
    discontinuity. Ardour's fold is the one piece of its behaviour not copied
    here: it switches instantly at monitor_processor.cc:325-361 while the per
    channel stage beside it ramps at line 319, so pressing Mono in Ardour
    clicks.

    THE DIM AMOUNT IS A CHOICE BETWEEN SOURCES, NOT A SPECIFICATION, and this
    is where that is admitted rather than implied. Every source makes dim
    adjustable and no two agree on the number: Ardour defaults to -12 dB over
    a -20 to 0 dB range, stored as a linear coefficient
    (monitor_processor.cc:58-61); the Digidesign C|24 Guide chapter 7
    documents a 20 dB reduction; UAD Console defaults to -17 dB over seven
    discrete steps from -9 to -60 dB; the PreSonus Monitor Station V2 trims
    -30 to -6 dB and fixes its talkback cue dim at -20 dB; the Grace Design
    m908 covers -30 to 0 dB in half decibel steps. Nobody ships a fixed dim
    on the main speaker path. So: adjustable, stored in decibels, default -20
    dB as the majority of the console side, over -30 to 0 dB, which covers
    the console side of that spread. UAD's lowest steps go below it and are
    deliberately out of reach, for the reason given at `dimFloorDb`.

    Stored in decibels rather than as a coefficient, which is where Ardour
    keeps it, because the number a person chooses is a number of decibels and
    a round trip through a float coefficient and back is not exactly the
    figure they asked for.
*/
namespace ControlRoom
{
    /** The adjustable range of the dim.

        It covers the console side of the sources the header comment collects:
        -30 dB is where both the Grace Design m908's range and the PreSonus
        Monitor Station V2's speaker attenuation bottom out, and Ardour's own
        range stops at -20. UAD Console's discrete steps go further, to -43,
        -51 and -60 dB, and those are deliberately not reachable here: this is
        a dim, which is a check you talk over and then come back from, and a
        60 dB dim is a mute with extra steps. So the floor is a choice and
        this is where that is said, rather than a claim to cover everything
        published. */
    inline constexpr double dimFloorDb   = -30.0;
    inline constexpr double dimCeilingDb =   0.0;

    /** The majority of the console side. See the header comment for the
        spread, which is real: Ardour ships -12 and the C|24 ships -20. */
    inline constexpr double defaultDimDb = -20.0;

    /** What the fold multiplies each leg by. Not a tunable: see the header
        comment for why this one is 0.5 and why the 0.7071 in circulation is a
        3/2 downmix row rather than a stereo law. */
    inline constexpr double foldPerLeg = 0.5;

    /** What the buttons are in, which is what the message thread sets.

        `dim` and `dimDb` are separate because they are different kinds of
        thing: whether the dim is engaged is a momentary check and is not
        saved, and how far it dims is calibration and is. The engine reads
        both together, so a dim amount changed while the dim is engaged takes
        effect without having to be re-engaged. */
    struct State
    {
        bool   mono        = false;
        bool   dim         = false;
        bool   cutLeft     = false;
        bool   cutRight    = false;
        bool   invertLeft  = false;
        bool   invertRight = false;
        double dimDb       = defaultDimDb;
    };

    /** The four coefficients the state resolves to.

        `out_L = leftFromLeft * L + leftFromRight * R`, and likewise for the
        right. Named rather than indexed for the reason `Synth/ModMatrix.h`
        gives about ids: four numbers in a row whose order is a convention is
        four chances to transpose two of them, and a transposed control room
        matrix swaps the speakers, which sounds like a mix decision. */
    struct Matrix
    {
        double leftFromLeft   = 1.0;
        double leftFromRight  = 0.0;
        double rightFromLeft  = 0.0;
        double rightFromRight = 1.0;
    };

    /** The matrix that passes the mix through untouched, which is what the
        speaker feed is whenever nothing is engaged. */
    inline Matrix passThrough() noexcept { return {}; }

    /** A stored dim amount brought inside the range.

        Guarded against a value that is not a number at all, which is not
        paranoia here: this comes out of a settings file that a previous
        version wrote, or has not been read yet, and `std::min` and
        `std::max` propagate a NaN rather than reject it. A NaN dim amount
        becomes a NaN coefficient, which does not go quiet: it silences the
        speaker feed and gives no clue where it came from. */
    inline double clampDimDb (double db) noexcept
    {
        if (! std::isfinite (db))
            return defaultDimDb;

        return std::clamp (db, dimFloorDb, dimCeilingDb);
    }

    /** What the dim multiplies both legs by, which is one when it is not
        engaged. */
    inline double dimGain (const State& s) noexcept
    {
        if (! s.dim)
            return 1.0;

        return std::pow (10.0, clampDimDb (s.dimDb) / 20.0);
    }

    /** What one leg is multiplied by before the fold: its cut, its polarity,
        and the dim that applies to both. */
    inline double legGain (bool cut, bool invert, double dim) noexcept
    {
        if (cut)
            return 0.0;

        return invert ? -dim : dim;
    }

    inline Matrix matrixFor (const State& s) noexcept
    {
        const double dim = dimGain (s);
        const double l   = legGain (s.cutLeft,  s.invertLeft,  dim);
        const double r   = legGain (s.cutRight, s.invertRight, dim);

        // The fold is after the per leg stage, which is Ardour's order and is
        // what makes mono mean the sum of what is currently reaching the
        // speakers rather than the sum of the mix.
        if (s.mono)
            return { foldPerLeg * l, foldPerLeg * r, foldPerLeg * l, foldPerLeg * r };

        return { l, 0.0, 0.0, r };
    }

    /** Whether the speaker feed is the mix.

        Defined on the signal rather than on the buttons, deliberately: a dim
        engaged at 0 dB does nothing to the sound, and an indicator that
        lights when nothing is happening to what you hear teaches people to
        ignore it. The test asserts the equivalence both ways, so this cannot
        drift from `matrixFor` the way a second opinion about the same state
        usually does.

        This is the mitigation Ardour reaches for and does not use. It
        computes a `monitor_active()` flag over exactly this question
        (monitor_processor.cc:483-504) and then its own monitor section blinks
        only solo and audition (monitor_section.cc:1144-1151), so nothing
        ever tells the user the path is not neutral. Here the latches are not
        saved and this indicator is on the master strip, which are the two
        halves of the same answer. */
    inline bool isNeutral (const State& s) noexcept
    {
        const auto m = matrixFor (s);

        return m.leftFromLeft == 1.0 && m.leftFromRight  == 0.0
            && m.rightFromLeft == 0.0 && m.rightFromRight == 1.0;
    }

    /** How far the coefficients can be from the mix, which is what bounds the
        speaker feed.

        `|out_L| <= |leftFromLeft| + |leftFromRight|` for input inside full
        scale, and that sum is at most one for every reachable state and, the
        test checks, at every point along a slew between any two of them. So
        the control room cannot clip material that was not already clipping,
        which is the property the 0.5 fold was chosen for. */
    inline double leftBound (const Matrix& m) noexcept
    {
        return std::abs (m.leftFromLeft) + std::abs (m.leftFromRight);
    }

    inline double rightBound (const Matrix& m) noexcept
    {
        return std::abs (m.rightFromLeft) + std::abs (m.rightFromRight);
    }

    /** What a device with one output channel gets.

        Written down as its own figure rather than left as a branch in the
        engine, because the alternatives are all wrong in a way that would not
        announce itself. A one channel feed has one leg, so the leg's own cut,
        polarity and dim apply and the fold has nothing to fold: taking the
        missing leg as silence would make the fold a 6 dB drop for nothing,
        and taking it as a copy of the leg that is there would make a cut of
        the absent leg a 6 dB drop instead. The controls that are about the
        relationship between two legs simply have no second leg to act on,
        which is the honest reading, and a dim still dims.

        Rare rather than hypothetical: the engine treats a missing right
        channel as a real case everywhere else in the block, so this is the
        case it already has. */
    inline double oneChannelGain (const State& s) noexcept
    {
        return legGain (s.cutLeft, s.invertLeft, dimGain (s));
    }

    /** What the row of buttons says it is doing, for the one line on the
        master strip. Built here rather than at the place that shows it, so
        the words cannot describe a state the matrix does not have. */
    inline const char* describe (const State& s) noexcept
    {
        if (isNeutral (s))
            return "control room: the speakers are hearing the mix";

        if (s.cutLeft && s.cutRight)
            return "control room: both legs cut, the speakers are silent";

        // A cut leg is described before the difference is claimed, because a
        // difference listen needs both legs. With one leg cut and the other
        // inverted the feed is half of the surviving leg with its polarity
        // flipped, which cancels nothing that a fold destroys: a centred
        // signal comes out at half level rather than at zero. Saying
        // "difference" there would be the fault this project keeps meeting,
        // present and plausible and wrong, in a sentence rather than in the
        // signal, and in the one place whose whole justification is that the
        // words cannot describe a state the matrix does not have. The first
        // version of this said it, reachable in four button presses, and the
        // test that pins the difference reading against the actual
        // cancellation is what found it.
        if (s.mono && s.cutLeft)      return "control room: mono, right leg only";
        if (s.mono && s.cutRight)     return "control room: mono, left leg only";

        if (s.mono && (s.invertLeft != s.invertRight))
            return "control room: difference, only what a mono fold destroys";
        if (s.mono)                   return "control room: mono";
        if (s.cutLeft)                return "control room: left leg cut";
        if (s.cutRight)               return "control room: right leg cut";
        if (s.invertLeft != s.invertRight)
            return "control room: one leg inverted";
        if (s.invertLeft && s.invertRight)
            return "control room: both legs inverted";

        return "control room: dimmed";
    }
}
