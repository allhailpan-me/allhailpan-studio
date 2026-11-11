// ---------------------------------------------------------------------------
// Checks the time signature: what a bar is, where the clicks fall, what the
// position readout counts, and what goes into a MIDI file.
//
// The fault this is mostly here to prevent is the one the research says every
// implementation of this is at risk of: putting the denominator into a
// conversion to seconds. A bar in 6/8 at 120 bpm is 1.5 seconds, because the
// tempo counts quarter notes whatever the signature says. Get that wrong and
// the studio plays a 6/8 song at two thirds speed, which is loud and obvious,
// or it moves the notes of an existing project when the signature is changed,
// which is not obvious at all.
//
// So the properties asserted here, in the order of how much each one catches:
//
//  1. A bar is exactly `numerator * 4 / denominator` quarters, as an exact
//     rational, for every signature the studio accepts.
//  2. Signatures with the same bar length in quarters are interchangeable:
//     4/4, 8/8 and 2/2 put their bar lines in exactly the same places. Any
//     stray multiplication or division by the denominator fails this.
//  3. The bar|beat readout round trips exactly, counts display beats, and
//     never leaves the range 1 .. numerator.
//  4. Exactly one accented click per bar, on the bar line, at negative
//     positions as well as positive ones, since the count-in runs before the
//     start of the song.
//  5. The MIDI meta event bytes, against the specification's own worked
//     example and against the reader that has to get the signature back.
//  6. Rounding up to a bar line lands on a bar line, never before the content
//     and never a whole bar after it, and actually moves with the signature.
//  7. The map's own invariants, asserted now so that the night the signature
//     can change mid song arrives into a test that already describes it.
// ---------------------------------------------------------------------------

#include "Meter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <vector>

static int failures = 0;

static void check (bool ok, const std::string& what)
{
    if (! ok)
    {
        std::printf ("  FAIL  %s\n", what.c_str());
        ++failures;
    }
}

static void checkEqual (double a, double b, const std::string& what)
{
    check (a == b, what + "  (" + std::to_string (a) + " vs " + std::to_string (b) + ")");
}

static void checkEqualInt (long long a, long long b, const std::string& what)
{
    check (a == b, what + "  (" + std::to_string (a) + " vs " + std::to_string (b) + ")");
}

static void checkNear (double a, double b, double tolerance, const std::string& what)
{
    check (std::abs (a - b) <= tolerance,
           what + "  (" + std::to_string (a) + " vs " + std::to_string (b) + ")");
}

static std::string name (const Meter::Signature& s)
{
    return std::to_string (s.numerator) + "/" + std::to_string (s.denominator);
}

/** Every signature the studio accepts. Small enough to sweep exhaustively,
    which is better than sampling it: the faults in here are at the edges, and
    4/1 and 99/32 are both edges.
*/
static std::vector<Meter::Signature> everySignature()
{
    std::vector<Meter::Signature> all;
    for (int d : { 1, 2, 4, 8, 16, 32 })
        for (int n = Meter::kMinNumerator; n <= Meter::kMaxNumerator; ++n)
            all.push_back ({ n, d });
    return all;
}

int main()
{
    const auto all = everySignature();
    std::printf ("Sweeping %zu accepted signatures\n\n", all.size());

    // -----------------------------------------------------------------------
    // 1. A bar is numerator * 4 / denominator quarters, exactly.
    //
    // Computed here as an exact rational in long long and then divided once,
    // rather than by repeating the header's own expression, so that this is a
    // check and not a copy. Every accepted denominator is a power of two, so
    // the division is exact and `==` is the right comparison.
    // -----------------------------------------------------------------------
    for (const auto& s : all)
    {
        const long long numerator = (long long) s.numerator * 4;
        checkEqual (Meter::barLengthQuarters (s),
                    (double) numerator / (double) s.denominator,
                    name (s) + ": a bar is n*4/d quarters");

        checkEqual (Meter::beatLengthQuarters (s) * (double) s.numerator,
                    Meter::barLengthQuarters (s),
                    name (s) + ": numerator display beats make a bar");
    }

    // The literals the research pins, which are the numbers another studio
    // would agree with. At 120 bpm a quarter is half a second, so the seconds
    // column is the quarters column halved: 4/4 is 2.0 s, 6/8 is 1.5 s, and
    // 6/8 being 3.0 s is the whole fault this file exists to catch.
    checkEqual (Meter::barLengthQuarters ({ 4, 4 }),  4.0,   "4/4 is four quarters");
    checkEqual (Meter::barLengthQuarters ({ 3, 4 }),  3.0,   "3/4 is three quarters");
    checkEqual (Meter::barLengthQuarters ({ 5, 4 }),  5.0,   "5/4 is five quarters");
    checkEqual (Meter::barLengthQuarters ({ 6, 8 }),  3.0,   "6/8 is three quarters, not six");
    checkEqual (Meter::barLengthQuarters ({ 7, 8 }),  3.5,   "7/8 is three and a half quarters");
    checkEqual (Meter::barLengthQuarters ({ 12, 8 }), 6.0,   "12/8 is six quarters");
    checkEqual (Meter::barLengthQuarters ({ 2, 2 }),  4.0,   "2/2 is four quarters");
    checkEqual (Meter::barLengthQuarters ({ 4, 1 }),  16.0,  "4/1 is sixteen quarters");
    checkEqual (Meter::barLengthQuarters ({ 27, 32 }), 27.0 / 8.0, "27/32 is 27/8 quarters");

    // -----------------------------------------------------------------------
    // 2. Equal bar lengths are interchangeable.
    //
    // 4/4, 8/8 and 2/2 are the same bar length in quarters and differ only in
    // how the bar is counted, so every bar line of each must land on exactly
    // the same quarter. This is the cheap stand-in for the strongest test the
    // research describes, which is rendering the same pattern under each and
    // asserting the output is sample identical: anything that scales a
    // position by the denominator where it should not fails here immediately.
    // -----------------------------------------------------------------------
    {
        const Meter::Signature family[] = { { 4, 4 }, { 8, 8 }, { 2, 2 }, { 16, 16 }, { 32, 32 }, { 1, 1 } };
        for (const auto& s : family)
            for (long long bar = -4; bar <= 64; ++bar)
                checkEqual (Meter::barStartQuarters (s, bar),
                            Meter::barStartQuarters ({ 4, 4 }, bar),
                            name (s) + ": bar " + std::to_string (bar) + " starts where 4/4's does");

        // And the same family must put its clicks at the same rate as its own
        // numerator says, which is what tells 4/4 from 8/8 at all: one is four
        // clicks to that bar and the other is eight.
        checkEqualInt (Meter::clickIndexAt ({ 4, 4 }, 4.0), 4, "4/4 has four clicks to the bar");
        checkEqualInt (Meter::clickIndexAt ({ 8, 8 }, 4.0), 8, "8/8 has eight clicks to the same bar");
        checkEqualInt (Meter::clickIndexAt ({ 2, 2 }, 4.0), 2, "2/2 has two clicks to the same bar");
    }

    // -----------------------------------------------------------------------
    // 3. The bar|beat readout.
    // -----------------------------------------------------------------------

    // The literals first, because a sign error or an off by one in the beat
    // index is invisible to a round trip and these are what another studio
    // would print. A display beat is the denominator unit: Logic, "A beat
    // corresponds to the denominator in the time signature."
    {
        const auto at = [] (const Meter::Signature& s, double q) { return Meter::barBeatAt (s, q); };

        checkEqualInt (at ({ 4, 4 }, 0.0).bar,  1, "quarter 0 is bar 1");
        checkEqualInt (at ({ 4, 4 }, 0.0).beat, 1, "quarter 0 is beat 1");

        // In 6/8, beat 6 of bar 1 is five eighths in, which is 2.5 quarters.
        checkEqualInt (at ({ 6, 8 }, 2.5).bar,  1, "6/8: 2.5 quarters is still bar 1");
        checkEqualInt (at ({ 6, 8 }, 2.5).beat, 6, "6/8: 2.5 quarters is beat 6");
        checkEqual    (at ({ 6, 8 }, 2.5).fraction, 0.0, "6/8: and exactly on it");

        // 5/4: bar 2 begins five quarters in. 7/8: three and a half.
        checkEqualInt (at ({ 5, 4 }, 5.0).bar,  2, "5/4: five quarters in is bar 2");
        checkEqualInt (at ({ 5, 4 }, 5.0).beat, 1, "5/4: and beat 1 of it");
        checkEqualInt (at ({ 7, 8 }, 3.5).bar,  2, "7/8: three and a half quarters in is bar 2");
        checkEqualInt (at ({ 7, 8 }, 3.5).beat, 1, "7/8: and beat 1 of it");

        // Just before a bar line is the last beat of the bar before, not the
        // first of the next. The off by one that would read 2|1 here is the
        // one a round trip cannot see.
        checkEqualInt (at ({ 7, 8 }, 3.5 - 1.0e-9).bar,  1, "7/8: a hair before bar 2 is bar 1");
        checkEqualInt (at ({ 7, 8 }, 3.5 - 1.0e-9).beat, 7, "7/8: and its seventh beat");

        // The count-in locates before the start of the song, so bar numbers go
        // below one and the beat index has to keep counting upward inside them.
        checkEqualInt (at ({ 4, 4 }, -4.0).bar,  0, "a bar before bar 1 is bar 0");
        checkEqualInt (at ({ 4, 4 }, -4.0).beat, 1, "and its first beat");
        checkEqualInt (at ({ 4, 4 }, -1.0).bar,  0, "one quarter before the song is in bar 0");
        checkEqualInt (at ({ 4, 4 }, -1.0).beat, 4, "on its last beat");
        checkEqualInt (at ({ 3, 4 }, -1.0).beat, 3, "3/4: and its last beat is the third");
    }

    // Then the sweep: round trip exactly, stay in range, and stay monotonic.
    //
    // Exact equality rather than a tolerance, and that is a claim worth
    // stating: every accepted denominator is a power of two, so a display beat
    // is an exactly representable length, the division and multiplication by
    // it are exact, and the subtraction of a bar start from a position inside
    // that bar is exact by Sterbenz. A tolerance here would hide the one thing
    // this is for.
    {
        std::mt19937 rng (0x7e11a1);
        std::uniform_real_distribution<double> position (0.0, 4000.0);

        for (const auto& s : all)
        {
            long long previousBar = std::numeric_limits<long long>::min();
            double    previousQ   = -1.0;

            for (int trial = 0; trial < 400; ++trial)
            {
                // Half the draws are deliberately on or next to a line, which
                // is where the faults are, and half are anywhere.
                double q = position (rng);
                if (trial % 4 == 1) q = std::floor (q / Meter::barLengthQuarters (s)) * Meter::barLengthQuarters (s);
                if (trial % 4 == 2) q = std::floor (q / Meter::beatLengthQuarters (s)) * Meter::beatLengthQuarters (s);
                if (trial % 4 == 3) q = std::nextafter (std::floor (q / Meter::barLengthQuarters (s))
                                                        * Meter::barLengthQuarters (s), 0.0);

                const auto bb = Meter::barBeatAt (s, q);

                check (bb.beat >= 1 && bb.beat <= s.numerator,
                       name (s) + ": beat " + std::to_string (bb.beat) + " is in range");
                check (bb.fraction >= 0.0 && bb.fraction < 1.0,
                       name (s) + ": the fraction of a beat is in [0, 1)");
                checkEqual (Meter::quartersAt (s, bb), q,
                            name (s) + ": bar|beat round trips exactly");

                // The bar containing a position must be the bar whose start is
                // at or before it and whose end is after it.
                const double start = Meter::barStartQuarters (s, bb.bar);
                check (start <= q && q < start + Meter::barLengthQuarters (s),
                       name (s) + ": the position is inside the bar it reports");

                if (q >= previousQ)
                {
                    check (bb.bar >= previousBar, name (s) + ": the bar number never goes backwards");
                    previousBar = bb.bar;
                    previousQ   = q;
                }
            }
        }
    }

    // The free functions take a Signature, whose fields are public, so a
    // caller can hand them one the studio would never accept. They have to
    // stay total over those too, and the beat index clamp is what keeps them
    // so: with a denominator that is not a power of two a display beat is not
    // exactly representable, and the division at a bar line can land a hair
    // past the numerator. 11/13 at quarter 192.923... reads 11.000000000000007
    // beats into an eleven beat bar, which unclamped is beat 12 of 11.
    //
    // Found by deliberately removing the clamp and looking for a position that
    // noticed. It is not reachable through the Map, which refuses these, and
    // that is not a reason to let a public function wander.
    {
        const Meter::Signature unaccepted[] { { 4, 3 }, { 5, 7 }, { 9, 6 }, { 3, 5 }, { 11, 13 } };

        std::mt19937 rng (0xbad513);
        std::uniform_real_distribution<double> position (0.0, 5000.0);

        for (const auto& s : unaccepted)
        {
            check (! Meter::isValid (s), name (s) + " is not a signature the studio accepts");

            for (int trial = 0; trial < 20000; ++trial)
            {
                double q = position (rng);
                const double bar = Meter::barLengthQuarters (s);

                // On a bar line and a hair either side of one, which is where
                // the inexact division lands past the end of the bar.
                if (trial % 3 == 1) q = std::floor (q / bar) * bar;
                if (trial % 3 == 2) q = std::nextafter (std::floor (q / bar) * bar, 1.0e18);

                const auto bb = Meter::barBeatAt (s, q);
                check (bb.beat >= 1 && bb.beat <= s.numerator,
                       name (s) + ": an unaccepted signature still reports a beat in range");
                check (bb.fraction >= 0.0 && bb.fraction < 1.0,
                       name (s) + ": and a fraction in [0, 1)");
            }
        }
    }

    // The same argument taken to the end of its range, which is where it had
    // not been taken. A numerator of zero is the one unaccepted signature
    // that is not merely inexact: four functions here divide by the bar
    // length, and a bar of no quarters makes `barOf` cast an infinity to a
    // `long long`. That is undefined behaviour, and it does not announce
    // itself under the sanitizers this suite runs. Before the guard in
    // `barLengthQuarters`, `barOf ({ 0, 4 }, 7.0)` returned
    // -9223372036854775807 and exited zero, while `barFloorQuarters` and
    // `roundUpToBar` returned a NaN that then spread into whatever asked for
    // a bar line.
    //
    // So the property asserted is the one those four rest on: no `Signature`
    // a caller can build has a length that is zero, negative, infinite or not
    // a number. Found by removing the `std::max (1, s.numerator)` from
    // `isAccentedClick`, the only one of these that had a guard, and watching
    // the whole suite stay green.
    {
        const Meter::Signature degenerate[] {
            { 0, 4 }, { 0, 8 }, { 0, 0 }, { -3, 4 }, { 4, 0 }, { 1, 0 },
            { 4, -4 }, { -1, -1 } };

        for (const auto& s : degenerate)
        {
            check (! Meter::isValid (s), name (s) + " is not a signature the studio accepts");

            const double beat = Meter::beatLengthQuarters (s);
            const double bar  = Meter::barLengthQuarters (s);

            check (std::isfinite (beat) && beat > 0.0,
                   name (s) + ": a display beat is still a positive length");
            check (std::isfinite (bar) && bar > 0.0,
                   name (s) + ": and a bar is still a positive length");

            for (double q : { -193.3, -7.0, -0.5, 0.0, 1.0, 7.0, 193.3 })
            {
                // A bar number cast from an infinity reads as a number, so
                // this pins the range a position of a few hundred quarters
                // can honestly produce rather than only checking for a NaN.
                const long long bars = Meter::barOf (s, q);
                check (bars > -100000 && bars < 100000,
                       name (s) + ": and a bar number a position that size could have");

                const auto bb = Meter::barBeatAt (s, q);
                check (bb.beat >= 1, name (s) + ": a display beat of at least one");
                check (bb.fraction >= 0.0 && bb.fraction < 1.0,
                       name (s) + ": and a fraction in [0, 1)");

                check (std::isfinite (Meter::quartersAt (s, bb)),
                       name (s) + ": the round trip comes back a position");
                check (std::isfinite (Meter::barFloorQuarters (s, q)),
                       name (s) + ": the bar floor is a position, not a NaN");
                check (std::isfinite (Meter::roundUpToBar (s, q, 1.0e-3)),
                       name (s) + ": and so is the bar above it");
                check (std::isfinite (Meter::snapQuarters (s, 0.25)),
                       name (s) + ": and so is a step of snap");
                check (std::isfinite (Meter::gridStepQuarters (s, 32.0, 6.0, 0.25)),
                       name (s) + ": and so is a grid spacing");

                // Reached rather than asserted about for its own sake: with a
                // numerator below one the modulus is `% 0` unless something
                // reads it as one, which is a division by zero the sanitizers
                // do catch, and this is the call that makes them.
                const long long index = Meter::clickIndexAt (s, q);
                if (s.numerator < 1)
                    check (Meter::isAccentedClick (s, index),
                           name (s) + ": a bar read as one beat accents every click");

                check (std::isfinite (Meter::clicksRemaining (s, 4.0) * 1.0),
                       name (s) + ": and a count-in has a number of clicks");
            }
        }
    }

    // -----------------------------------------------------------------------
    // 4. The click and the accent.
    //
    // One click per display beat, exactly one of them accented, and the
    // accented one on the bar line. Asserted by walking the clicks rather than
    // by recomputing the rule, so a rule that accented the wrong beat, or two
    // beats, or none, fails.
    // -----------------------------------------------------------------------
    for (const auto& s : all)
    {
        for (long long bar = -3; bar <= 8; ++bar)
        {
            const double start = Meter::barStartQuarters (s, bar);
            const double beatLength = Meter::beatLengthQuarters (s);

            int accents = 0;
            for (int beat = 0; beat < s.numerator; ++beat)
            {
                // A hair inside the beat, because the first sample of a click
                // is what the engine tests and landing exactly on a boundary
                // is the ambiguous case the readout check above covers.
                const double q = start + ((double) beat + 0.25) * beatLength;
                const long long index = Meter::clickIndexAt (s, q);

                if (Meter::isAccentedClick (s, index))
                {
                    ++accents;
                    checkEqualInt (beat, 0, name (s) + ": the accent is on the first beat of the bar");
                }
            }

            checkEqualInt (accents, 1, name (s) + ": exactly one accent in bar " + std::to_string (bar));
        }

        // And the clicks run at the numerator's rate: one bar of them is
        // numerator clicks, so a bar line is always a whole number of bars of
        // clicks from every other bar line.
        const double bar = Meter::barLengthQuarters (s);
        checkEqualInt (Meter::clickIndexAt (s, 3.0 * bar) - Meter::clickIndexAt (s, 0.0),
                       3LL * s.numerator,
                       name (s) + ": three bars is three numerators of clicks");
    }

    // -----------------------------------------------------------------------
    // 4b. The count-in, which is where the two things called a beat meet in
    //     the same readout.
    //
    // Its length is bars of the signature, the transport counts the quarters
    // every position is stored in, and the metronome clicks display beats, so
    // the number to show the player is the third of those and not either of
    // the first two. The property asserted is that the readout agrees with
    // what the player is hearing: at the moment each click sounds, the number
    // shown is how many clicks are still to come including that one, counting
    // down to one on the last of them, and it holds that number until the
    // next click rather than changing between them.
    //
    // This is the fault the readout shipped with on 2026-11-11. It showed the
    // quarters the transport gives it, so a two bar count-in in 6/8 sounded
    // twelve clicks and counted down from six, changing the number on every
    // other one, and a one bar count-in in 7/8 sounded seven clicks and
    // counted down from four. 4/4 and 3/4 were right, which is why it was not
    // noticed: there a quarter is a display beat.
    // -----------------------------------------------------------------------
    {
        // The literals, which are what a player watching the screen counts.
        checkEqualInt (Meter::clicksRemaining ({ 4, 4 }, 4.0),  4,
                       "4/4: a one bar count-in counts four");
        checkEqualInt (Meter::clicksRemaining ({ 3, 4 }, 6.0),  6,
                       "3/4: a two bar count-in counts six");
        checkEqualInt (Meter::clicksRemaining ({ 6, 8 }, 6.0),  12,
                       "6/8: a two bar count-in counts twelve, not the six quarters it lasts");
        checkEqualInt (Meter::clicksRemaining ({ 7, 8 }, 3.5),  7,
                       "7/8: a one bar count-in counts seven, not four");
        checkEqualInt (Meter::clicksRemaining ({ 4, 1 }, 16.0), 4,
                       "4/1: a one bar count-in counts four, not sixteen");

        checkEqualInt (Meter::clicksRemaining ({ 4, 4 }, 0.0),  0,
                       "a count-in that has run out counts nothing");
        checkEqualInt (Meter::clicksRemaining ({ 4, 4 }, -1.0), 0,
                       "and so does one the transport has already passed");
        checkEqualInt (Meter::clicksRemaining ({ 4, 4 }, std::numeric_limits<double>::quiet_NaN()), 0,
                       "and a length that is not a number");
        checkEqualInt (Meter::clicksRemaining ({ 4, 4 }, std::numeric_limits<double>::infinity()), 0,
                       "and an infinite one");

        for (const auto& s : all)
        {
            const double beatLength = Meter::beatLengthQuarters (s);

            for (int bars = 1; bars <= 3; ++bars)
            {
                const double length = (double) bars * Meter::barLengthQuarters (s);
                const long long clicks = (long long) bars * (long long) s.numerator;

                for (long long k = 0; k < clicks; ++k)
                {
                    const double left = length - (double) k * beatLength;

                    checkEqualInt (Meter::clicksRemaining (s, left), clicks - k,
                                   name (s) + ": the readout is the clicks still to sound");

                    checkEqualInt (Meter::clicksRemaining (s, left - 0.5 * beatLength),
                                   clicks - k,
                                   name (s) + ": and holds between one click and the next");

                    // And the tolerance's own purpose, which is the other
                    // direction: a position a hair short of the next click
                    // counts as having reached it, because the transport
                    // works its position out in floating point and lands a
                    // few parts in 10^16 either side of a boundary. Without
                    // that, the number before it would show twice.
                    checkEqualInt (Meter::clicksRemaining (s, std::nextafter (left - beatLength, length)),
                                   clicks - k - 1,
                                   name (s) + ": a hair short of the next click is already at it");
                }

                // And the whole count-in is as long as the clicks it sounds,
                // which is the agreement the window got wrong: the first
                // number shown is the number of clicks, not of quarters.
                checkEqualInt (Meter::clicksRemaining (s, length), clicks,
                               name (s) + ": the first number shown is the click count");
                check (clicks == (long long) bars * (long long) s.numerator,
                       name (s) + ": which is bars of numerator clicks");
            }
        }
    }

    // -----------------------------------------------------------------------
    // 5. The MIDI meta event.
    // -----------------------------------------------------------------------
    {
        // The specification's own worked example, which is a 6/8 file whose
        // metronome clicks every three eighths: "FF 58 04 06 03 24 08". The
        // click length rather than the signature is what decides the cc byte,
        // so this is checked through the length and reproduces 0x24 = 36.
        checkEqualInt (Meter::midiClocksForQuarters (1.5), 0x24,
                       "the spec's dotted quarter click is 36 clocks");
        checkEqualInt (Meter::midiClocksForQuarters (1.0), 24, "a quarter note click is 24 clocks");
        checkEqualInt (Meter::midiClocksForQuarters (0.5), 12, "an eighth note click is 12 clocks");

        // Not 0x18. The widely mirrored copy of that example gives the cc byte
        // as 0x18, which is 24 decimal, one quarter, two eighths, and
        // contradicts its own prose. Pinned so nobody "corrects" this to it.
        check (Meter::midiClocksForQuarters (1.5) != 0x18,
               "the dotted quarter click is not 0x18, whatever the jglatt page says");

        // A click that is not a whole number of clocks rounds to the nearest
        // one rather than always early. Nothing in the studio asks for one:
        // every display beat is 96, 48, 24, 12, 6 or 3 clocks, so truncating
        // instead of rounding passed every check here. The function is
        // exposed apart from the signature for the caller that does not exist
        // yet, which is the additional click on the groups of a compound
        // meter, and the rounding is the contract that caller will get.
        checkEqualInt (Meter::midiClocksForQuarters (0.9), 22,
                       "21.6 clocks rounds to the nearest, 22, not down to 21");
        checkEqualInt (Meter::midiClocksForQuarters (0.1), 2,
                       "and 2.4 clocks rounds to 2");

        // Both ends of the byte, which the comment above the function claims
        // and nothing produced. A dotted whole note click in 4/1 is six
        // quarters, which is 144 clocks and fits; anything longer is clamped,
        // which is the lie that future caller has to know about.
        checkEqualInt (Meter::midiClocksForQuarters (6.0), 144,
                       "a dotted whole note click is 144 clocks, which the byte holds");
        checkEqualInt (Meter::midiClocksForQuarters (20.0), 255,
                       "and a click past the byte is clamped into it");
        checkEqualInt (Meter::midiClocksForQuarters (0.0), Meter::kMidiClocksPerQuarter,
                       "a click of no length reads as a quarter note, not as none");

        const auto bytes = Meter::metaEventBytes ({ 6, 8 });
        checkEqualInt (bytes[0], 0xff, "meta prefix");
        checkEqualInt (bytes[1], 0x58, "time signature meta type");
        checkEqualInt (bytes[2], 0x04, "four data bytes");
        checkEqualInt (bytes[3], 6,    "6/8 writes a numerator of 6");
        checkEqualInt (bytes[4], 3,    "6/8 writes a denominator byte of 3, not 8");
        checkEqualInt (bytes[5], 12,   "and clicks every eighth, which is 12 clocks");
        checkEqualInt (bytes[6], 8,    "eight notated 32nds to a MIDI quarter note");

        // The two bytes JUCE gets wrong. Its own timeSignatureMetaEvent
        // returns 1 and 96 here, so these two lines are the regression.
        check (bytes[5] != 1,  "the click byte is not JUCE's 1");
        check (bytes[6] != 96, "the 32nds byte is not JUCE's 96");

        for (const auto& s : all)
        {
            const auto b = Meter::metaEventBytes (s);

            // The byte is a logarithm. A missing one writes 8 where 3 belongs,
            // which reads back as 6/4 rather than 6/8: present, plausible and
            // wrong, and out of range for the format besides.
            checkEqualInt (1 << b[4], s.denominator, name (s) + ": the denominator byte is a logarithm");
            check (b[4] >= 0 && b[4] <= 5, name (s) + ": the denominator byte is in range");
            checkEqualInt (b[3], s.numerator, name (s) + ": the numerator byte is literal");
            checkEqualInt (b[6], 8, name (s) + ": the 32nds byte is always eight");
            check (b[5] >= 1 && b[5] <= 255, name (s) + ": the click byte fits in a byte");

            // And the reader gets the signature back, which is the property
            // that matters: a file this studio writes, read by this studio,
            // must be the same music.
            const auto back = Meter::fromMetaEventBytes (b[3], b[4]);
            check (back == s, name (s) + ": survives a round trip through the file bytes");
        }

        // The writer refuses a signature nothing validated, which is the one
        // claim its comment makes and was the one thing never exercised: the
        // Map cannot hold 200/4, but the free function takes a struct with
        // public fields, and writing nn = 200 or, after the byte truncates,
        // nn = 44, would be a file saying something nobody asked for.
        for (const auto bad : { Meter::Signature { 200, 4 }, Meter::Signature { 300, 4 },
                                Meter::Signature { 4, 3 },   Meter::Signature { 0, 4 } })
        {
            const auto b = Meter::metaEventBytes (bad);
            checkEqualInt (b[3], 4, name (bad) + ": an unaccepted signature writes 4/4's numerator");
            checkEqualInt (b[4], 2, name (bad) + ": and 4/4's denominator byte");
        }

        // A reader must not be talked into an impossible signature by a file.
        check (Meter::fromMetaEventBytes (6, 6)   == Meter::kDefault, "a denominator byte of 6 is refused");

        // The bound on the byte is there for the shift and not only for the
        // range: a denominator byte of 64 would be `1 << 64`, which is
        // undefined behaviour, and a file is free to carry one. Bytes 6 to 8
        // are refused either way, by the validation a line later, so these are
        // the values that make the bound load bearing.
        check (Meter::fromMetaEventBytes (4, 31)  == Meter::kDefault, "a denominator byte of 31 is refused");
        check (Meter::fromMetaEventBytes (4, 32)  == Meter::kDefault, "a denominator byte of 32 is refused");
        check (Meter::fromMetaEventBytes (4, 64)  == Meter::kDefault, "a denominator byte of 64 is refused");
        check (Meter::fromMetaEventBytes (4, 255) == Meter::kDefault, "a denominator byte of 255 is refused");
        check (Meter::fromMetaEventBytes (6, -1)  == Meter::kDefault, "a negative denominator byte is refused");
        check (Meter::fromMetaEventBytes (0, 2)   == Meter::kDefault, "a numerator of zero is refused");
        check (Meter::fromMetaEventBytes (200, 2) == Meter::kDefault, "a numerator of 200 is refused");
        check (Meter::fromMetaEventBytes (7, 3)   == Meter::Signature { 7, 8 }, "7/8 reads back as 7/8");
    }

    // -----------------------------------------------------------------------
    // 6. Rounding up to a bar line.
    // -----------------------------------------------------------------------
    {
        std::mt19937 rng (0x8a2c01);
        std::uniform_real_distribution<double> length (0.0, 500.0);
        const double tolerance = 1.0e-3;

        for (const auto& s : all)
        {
            const double bar = Meter::barLengthQuarters (s);

            for (int trial = 0; trial < 200; ++trial)
            {
                const double content = length (rng);
                const double end = Meter::roundUpToBar (s, content, tolerance);

                const double bars = end / bar;
                checkEqual (bars, std::floor (bars), name (s) + ": the rounded end is a whole number of bars");
                check (end >= content - tolerance, name (s) + ": the rounded end is not before the content");
                check (end < content + bar, name (s) + ": and is less than a whole bar past it");
            }

            // On a line, it stays. A hair past one, it still stays, which is
            // the whole reason for the tolerance: a one bar loop whose length
            // in beats came out a fraction long must not gain a bar of
            // silence.
            checkEqual (Meter::roundUpToBar (s, 2.0 * bar, tolerance), 2.0 * bar,
                        name (s) + ": a content end on a bar line does not gain a bar");
            checkEqual (Meter::roundUpToBar (s, 2.0 * bar + tolerance * 0.5, tolerance), 2.0 * bar,
                        name (s) + ": nor does one a hair past it");
            check (Meter::roundUpToBar (s, 2.0 * bar + tolerance * 4.0, tolerance) > 2.0 * bar,
                   name (s) + ": but one clearly past it does");
        }

        // It has to actually move with the signature. A left behind four would
        // pass everything above for 4/4 and this is what fails it.
        checkEqual (Meter::roundUpToBar ({ 7, 8 }, 4.0, tolerance), 7.0, "7/8 rounds 4 quarters up to seven");
        checkEqual (Meter::roundUpToBar ({ 3, 4 }, 4.0, tolerance), 6.0, "3/4 rounds 4 quarters up to 6");
        checkEqual (Meter::roundUpToBar ({ 4, 4 }, 4.0, tolerance), 4.0, "4/4 leaves 4 quarters where it is");
        check (Meter::roundUpToBar ({ 3, 4 }, 4.0, tolerance)
               != Meter::roundUpToBar ({ 4, 4 }, 4.0, tolerance),
               "the same content rounds to a different place in a different signature");
    }

    // -----------------------------------------------------------------------
    // 7. What is accepted, and what the map promises.
    // -----------------------------------------------------------------------
    {
        for (const auto& s : all)
            check (Meter::isValid (s), name (s) + " is accepted");

        check (! Meter::isValid ({ 0, 4 }),   "a numerator of zero is refused");
        check (! Meter::isValid ({ 100, 4 }), "a numerator of 100 is refused");
        check (! Meter::isValid ({ 4, 3 }),   "a denominator of 3 is refused: the MIDI byte cannot say it");
        check (! Meter::isValid ({ 4, 0 }),   "a denominator of zero is refused");
        check (! Meter::isValid ({ 4, 64 }),  "a denominator of 64 is refused");
        check (! Meter::isValid ({ 4, -4 }),  "a negative denominator is refused");

        // Refused, not clamped. 4/3 becoming 4/2 or 4/4 depending on which way
        // it rounded is a signature nobody asked for arriving without a word.
        check (Meter::validated ({ 4, 3 }) == Meter::kDefault, "an impossible signature falls back to 4/4");
        check (Meter::validated ({ 7, 8 }) == Meter::Signature { 7, 8 }, "a possible one is left alone");

        // 4/4 pinned as a literal, not as a comparison with kDefault.
        //
        // Every statement in here about "the default" used to compare against
        // kDefault itself, which is a restatement of the implementation and
        // not a check on it: changing kDefault to 4/8 passed the whole file.
        // What that would mean in the studio is that every project saved
        // before the signature existed opens with its bar lines in halves,
        // two clicks a bar and a MIDI export announcing 4/8, which is the
        // exact promise the format version 9 note makes.
        check (Meter::kDefault == Meter::Signature { 4, 4 }, "the default signature is 4/4");
        check (Meter::barLengthQuarters (Meter::kDefault) == 4.0, "and its bar is four quarters");

        // And the default BarBeat, which is what a position that is not a
        // position reads as. Bar 1 beat 1, not bar 0.
        check (Meter::BarBeat {}.bar == 1, "a default BarBeat is bar 1");
        check (Meter::BarBeat {}.beat == 1, "and beat 1");
        checkEqualInt (Meter::barBeatAt ({ 7, 8 }, std::numeric_limits<double>::quiet_NaN()).bar, 1,
                       "a NaN position reads as bar 1 and not bar 0");

        Meter::Map map;
        check (map.at (0.0) == Meter::kDefault, "a new project is in 4/4");

        check (map.setFirst ({ 6, 8 }), "6/8 can be set");
        check (map.at (0.0) == Meter::Signature { 6, 8 }, "and takes");
        check (! map.setFirst ({ 4, 3 }), "4/3 is refused");
        check (map.at (0.0) == Meter::Signature { 6, 8 }, "and leaves the project on the signature it had");
        check (! map.setFirst ({ 0, 8 }), "a numerator of zero is refused");
        check (map.at (0.0) == Meter::Signature { 6, 8 }, "and leaves it alone too");

        // The invariants the map has to keep once it holds more than one
        // entry. Asserted now, while they are trivially true, so that the
        // night it does arrives into a test that already describes them.
        std::mt19937 rng (0x15c0de);
        std::uniform_real_distribution<double> anywhere (-1000.0, 1000.0);

        long long previousBar = std::numeric_limits<long long>::min();
        double previous = -1.0e9;

        std::vector<double> positions;
        for (int i = 0; i < 2000; ++i)
            positions.push_back (anywhere (rng));
        std::sort (positions.begin(), positions.end());

        for (double q : positions)
        {
            // Total: every finite position has a signature, and it is one the
            // studio accepts.
            check (Meter::isValid (map.at (q)), "the map answers with an accepted signature everywhere");

            const auto bb = map.barBeatAt (q);
            check (bb.bar >= previousBar, "bar numbering is monotonic non-decreasing in the position");
            previousBar = bb.bar;
            previous = q;

            checkEqual (map.quartersAt (bb), q, "the map's own round trip is exact");
        }
        (void) previous;

        // Nothing that is not a position may reach it. The engine reads a
        // transport position that can be a NaN for one block after a device
        // change, and a NaN bar number would be drawn as a bar line somewhere
        // unpredictable.
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();

        // And a position that is finite and still not a position. A bar
        // number is a long long, and the cast from a double that does not fit
        // in one is undefined behaviour rather than a large answer: 1e300
        // quarters tripped "signed integer overflow: 9223372036854775807 + 1"
        // under the suite's own sanitizers. ProjectIO loads a clip start with
        // a floor of zero and no ceiling, so a damaged project can carry one
        // as far as the position readout.
        for (double huge : { 1.0e300, -1.0e300, 1.0e18, -1.0e18, 1.0e16 })
        {
            checkEqualInt (Meter::barOf ({ 7, 8 }, huge), 1,
                           "a position too large to have a bar number reads as bar 1");
            const auto at = Meter::barBeatAt ({ 7, 8 }, huge);
            checkEqualInt (at.bar, 1, "and its readout is bar 1 rather than an overflow");
            checkEqualInt (at.beat, 1, "and beat 1, not the last beat of the bar");
            checkEqual (at.fraction, 0.0, "and no fraction of a beat");
            checkEqualInt (Meter::clickIndexAt ({ 7, 8 }, huge), 0,
                           "and it sounds no click");
            check (Meter::isValid (map.at (huge)), "and the map still answers with a signature");
        }

        // The bound itself, either side: 2^53 is in and the double above it is
        // out, so the guard is where it says it is.
        check (Meter::barOf ({ 4, 4 }, Meter::kMaxQuarters) != 1,
               "the largest position with a bar number has one");
        checkEqualInt (Meter::barOf ({ 4, 4 }, std::nextafter (Meter::kMaxQuarters, 1.0e300)), 1,
                       "and the one above it does not");

        check (Meter::isValid (map.at (nan)), "a NaN position still answers with a signature");
        checkEqual (Meter::barBeatAt ({ 7, 8 }, nan).fraction, 0.0, "a NaN position has no fraction either");
        checkEqualInt (Meter::barOf ({ 7, 8 }, nan), 1, "a NaN position reads as bar 1 rather than wandering");
        checkEqualInt (Meter::barOf ({ 7, 8 }, inf), 1, "and so does an infinite one");
        checkEqualInt (Meter::barBeatAt ({ 7, 8 }, nan).beat, 1, "and its beat is 1");
        checkEqualInt (Meter::clickIndexAt ({ 7, 8 }, nan), 0, "a NaN position does not sound a click somewhere");
        checkEqual (Meter::roundUpToBar ({ 7, 8 }, nan, 1.0e-3), 0.0, "and rounds to nothing rather than to a NaN");
        checkEqual (Meter::roundUpToBar ({ 7, 8 }, 4.0, nan), 7.0, "a NaN tolerance is read as none");
    }

    // -----------------------------------------------------------------------
    // 7b. The snap sizes and the bar floor.
    //
    // The editors' snap tables are in display beats so their names stay true
    // in every signature, and the one property that makes that change safe
    // under every saved project is that 4/4 gives back exactly the numbers
    // those tables used to hold.
    // -----------------------------------------------------------------------
    {
        checkEqual (Meter::snapQuarters ({ 4, 4 }, Meter::kSnapWholeBar), 4.0,  "4/4: Bar is still four");
        checkEqual (Meter::snapQuarters ({ 4, 4 }, 1.0),                  1.0,  "4/4: Beat is still one");
        checkEqual (Meter::snapQuarters ({ 4, 4 }, 0.25),                 0.25, "4/4: Step is still a quarter");
        checkEqual (Meter::snapQuarters ({ 4, 4 }, 1.0 / 24.0), 1.0 / 24.0,     "4/4: a sixth of a step is unchanged");
        checkEqual (Meter::snapQuarters ({ 4, 4 }, 0.0),                  0.0,  "4/4: None is still none");

        // And in 6/8 a beat is an eighth and a step a thirty second, so the
        // names still describe what the grid does: they are relative to a
        // display beat, which is the whole reason they are stored that way.
        checkEqual (Meter::snapQuarters ({ 6, 8 }, Meter::kSnapWholeBar), 3.0,   "6/8: a bar is three quarters");
        checkEqual (Meter::snapQuarters ({ 6, 8 }, 1.0),                  0.5,   "6/8: a beat is an eighth");
        checkEqual (Meter::snapQuarters ({ 6, 8 }, 0.25),                 0.125, "6/8: a step is a thirty second");
        checkEqual (Meter::snapQuarters ({ 7, 8 }, Meter::kSnapWholeBar), 3.5,   "7/8: a bar is three and a half");

        // Only the sentinel means a bar. It is -1.0 compared with `==`, and
        // that is safe only because nothing else negative means anything, so
        // this asserts it rather than assuming it.
        checkEqual (Meter::snapQuarters ({ 6, 8 }, -0.5), 0.0,
                    "6/8: a negative that is not the sentinel is no snap");
        checkEqual (Meter::snapQuarters ({ 6, 8 }, -2.0), 0.0,
                    "and so is one past the sentinel");
        checkEqual (Meter::snapQuarters ({ 6, 8 }, std::nextafter (Meter::kSnapWholeBar, 0.0)), 0.0,
                    "and so is a hair off it");

        for (const auto& s : all)
        {
            // Every snap size divides a bar a whole number of times, or the
            // grid drawn from it would not line up with the bar lines it is
            // drawn next to.
            const double bar = Meter::snapQuarters (s, Meter::kSnapWholeBar);
            checkEqual (bar, Meter::barLengthQuarters (s), name (s) + ": the Bar snap is a bar");

            const double beat = Meter::snapQuarters (s, 1.0);
            checkEqual (bar / beat, (double) s.numerator, name (s) + ": a bar is numerator beats of snap");

            check (Meter::snapQuarters (s, 0.0) == 0.0, name (s) + ": None snaps to nothing");

            // The floor lands on a line at or before the position, every time.
            for (double q : { 0.0, 0.5, 1.0, 7.3, 101.75 })
            {
                const double floored = Meter::barFloorQuarters (s, q);
                check (floored <= q, name (s) + ": the bar floor is not after the position");
                check (q - floored < bar, name (s) + ": and is less than a bar before it");
                checkEqual (floored / bar, std::floor (floored / bar), name (s) + ": and is a bar line");
            }
        }
    }

    // -----------------------------------------------------------------------
    // 7c. The grid spacing the editors draw to.
    //
    // Three promises, over every accepted signature and every zoom the
    // editors can reach: it is at least as wide as asked, it divides the bar
    // so that every bar line gets a line, and it is positive and finite so the
    // loop that steps along it terminates.
    //
    // The second is the one with teeth. Doubling up from a display beat
    // satisfies the first two only when the numerator is a power of two: 99/32
    // has a bar of 12.375 quarters and four quarters is not a part of it, so a
    // doubling ladder drew lines either side of the bar and none on it.
    // -----------------------------------------------------------------------
    {
        const double zooms[] { 2.0, 2.5, 3.0, 4.0, 7.0, 8.0, 50.0, 137.0, 400.0 };

        // A `finest` that is not a number falls back to a display beat and not
        // to a bar. The guard was there and its answer was untested, and the
        // two differ by the numerator: in 7/8 at a zoom where everything is
        // wide enough a bar is seven times the grid that was asked for.
        for (const auto& s : all)
        {
            const double notANumber = std::numeric_limits<double>::quiet_NaN();
            checkEqual (Meter::gridStepQuarters (s, 1000.0, 6.0, notANumber),
                        Meter::beatLengthQuarters (s),
                        name (s) + ": a finest that is not a number draws display beats");
            checkEqual (Meter::gridStepQuarters (s, 1000.0, 6.0, 0.0),
                        Meter::beatLengthQuarters (s),
                        name (s) + ": and so does a finest of nothing");
        }

        // The minimum is swept rather than fixed at the six pixels both
        // editors ask for today. An audit mutated the accessor to hardcode
        // that six and to raise anything smaller to it, and both survived a
        // sweep that only ever passed six: the one argument never varied is
        // the one that can be ignored without the suite noticing.
        for (double wanted : { 2.0, 6.0, 10.0, 25.0 })
        for (const auto& s : all)
        {
            const double bar  = Meter::barLengthQuarters (s);
            const double beat = Meter::beatLengthQuarters (s);

            for (double zoom : zooms)
                for (double finest : { beat, 0.25 * beat, 0.5 * beat })
                {
                    const double step = Meter::gridStepQuarters (s, zoom, wanted, finest);

                    check (step > 0.0 && std::isfinite (step),
                           name (s) + ": the grid step is a positive finite spacing");
                    check (step * zoom >= wanted - 1.0e-9,
                           name (s) + ": the grid step is at least as wide as asked");

                    // Divides the bar, or is a whole number of bars. Either
                    // way every bar line falls on a line of the grid.
                    const double barsPerStep  = step / bar;
                    const double stepsPerBar  = bar / step;
                    const bool onTheBar = std::abs (stepsPerBar - std::round (stepsPerBar)) < 1.0e-9
                                       || std::abs (barsPerStep - std::round (barsPerStep)) < 1.0e-9;
                    check (onTheBar, name (s) + ": and the bar line falls on it");

                    // Never coarser than a bar while a bar is readable. The
                    // multi-bar rung exists for a zoom where even a bar is
                    // narrower than the minimum, and must not be reached
                    // otherwise: a grid drawn every four bars while every bar
                    // would have fitted is a grid that has stopped being one.
                    check (step <= bar + 1.0e-9 || bar * zoom < wanted,
                           name (s) + ": and is no coarser than a bar while a bar is readable");
                }

            // It uses the finest spacing when that is already wide enough,
            // rather than jumping to a beat.
            checkEqual (Meter::gridStepQuarters (s, 1000.0, wanted, 0.25 * beat), 0.25 * beat,
                        name (s) + ": a step that is already wide enough is what is drawn");

            // And a spacing finer than a display beat is never offered when it
            // would be too narrow.
            check (Meter::gridStepQuarters (s, 2.0, wanted, 0.25 * beat) >= beat - 1.0e-9
                   || 0.25 * beat * 2.0 >= wanted,
                   name (s) + ": a step too narrow to read is not drawn");
        }

        // Nothing that is not a zoom may reach it either, and the answer has
        // to be a number and not only a positive one: a NaN spacing makes the
        // loop that steps along it draw nothing at all, silently, which is
        // worse than a grid that is too dense.
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();

        const auto usable = [] (double step, const std::string& what)
        {
            check (step > 0.0 && std::isfinite (step), what);
        };

        usable (Meter::gridStepQuarters ({ 4, 4 }, nan, 6.0, 1.0), "a NaN zoom gives a usable spacing");
        usable (Meter::gridStepQuarters ({ 4, 4 }, inf, 6.0, 1.0), "an infinite zoom gives a usable spacing");
        usable (Meter::gridStepQuarters ({ 4, 4 }, 8.0, nan, 1.0), "a NaN minimum gives a usable spacing");
        usable (Meter::gridStepQuarters ({ 4, 4 }, 8.0, 6.0, nan), "a NaN finest gives a usable spacing");
        usable (Meter::gridStepQuarters ({ 4, 4 }, 0.0, 6.0, 1.0), "a zoom of zero gives a usable spacing");
        usable (Meter::gridStepQuarters ({ 4, 4 }, -8.0, 6.0, 1.0), "a negative zoom gives a usable spacing");
        usable (Meter::gridStepQuarters ({ 4, 3 }, 8.0, 6.0, 1.0), "an unaccepted signature gives a usable spacing");

        // Usable is not enough: dropping `validated` from the top of the
        // function left every one of these giving an answer that is positive
        // and finite and wrong, so the check that was meant to pin it did not.
        // Unvalidated, 0/4 gives a NaN, which makes the draw loop draw
        // nothing, and -4/4 gives zero, which makes it loop forever on the
        // message thread. The answer has to be the 4/4 answer.
        for (double zoom : { 2.0, 8.0, 60.0 })
            for (const auto& bad : { Meter::Signature { 4, 3 }, Meter::Signature { 0, 4 },
                                     Meter::Signature { -4, 4 }, Meter::Signature { 4, 0 },
                                     Meter::Signature { 100, 4 } })
                checkEqual (Meter::gridStepQuarters (bad, zoom, 6.0, 1.0),
                            Meter::gridStepQuarters (Meter::kDefault, zoom, 6.0, 1.0),
                            "an unaccepted signature gives exactly the 4/4 grid");

        // The threshold is "at least this wide", and exactly this wide counts.
        // Turning the >= into a > skips a rung at a zoom where one lands on
        // the minimum exactly, which fit-to-width reaches routinely because it
        // divides a pixel width by a song length. None of the property checks
        // above can see it: the coarser answer is still wide enough, still
        // divides the bar and is still no coarser than a bar.
        //
        // The second half is the same boundary from below, and is why the
        // minimum may not be quietly relaxed either: a thousandth off it, in
        // the direction of accepting a narrower rung, was a surviving mutation
        // until this was here. Written as a fraction of the boundary rather
        // than with a chosen zoom, so it holds for every signature.
        for (const auto& s : all)
        {
            const double beat = Meter::beatLengthQuarters (s);
            const double onTheNose = 6.0 / beat;      // a display beat is exactly 6 px

            checkEqual (Meter::gridStepQuarters (s, onTheNose, 6.0, beat), beat,
                        name (s) + ": a beat exactly as wide as asked is drawn");

            if (s.numerator >= 2)
                check (Meter::gridStepQuarters (s, onTheNose * 0.9995, 6.0, beat) > beat + 1.0e-12,
                       name (s) + ": and a beat a twentieth of a percent too narrow is not");
        }

        // A caller asking for a finest spacing coarser than a display beat is
        // held to a display beat, because a coarser one need not divide the
        // bar and the promise is the bar line. No caller does this today; the
        // bound is what lets the next one not have to know.
        for (const auto& s : all)
        {
            const double beat = Meter::beatLengthQuarters (s);
            const double bar  = Meter::barLengthQuarters (s);
            const double step = Meter::gridStepQuarters (s, 1000.0, 6.0, 3.0 * beat);

            check (step <= beat + 1.0e-9, name (s) + ": a finest coarser than a beat is held to a beat");

            const double stepsPerBar = bar / step;
            check (std::abs (stepsPerBar - std::round (stepsPerBar)) < 1.0e-9,
                   name (s) + ": so it still divides the bar");
        }

        // The 4/4 answers, pinned, because these are what the editors drew
        // before the signature existed.
        checkEqual (Meter::gridStepQuarters ({ 4, 4 }, 8.0, 6.0, 1.0), 1.0, "4/4 at 8 px a quarter draws beats");
        checkEqual (Meter::gridStepQuarters ({ 4, 4 }, 2.0, 6.0, 1.0), 4.0, "4/4 at 2 px a quarter draws bars");
        checkEqual (Meter::gridStepQuarters ({ 4, 4 }, 0.5, 6.0, 1.0), 12.0, "4/4 at half a pixel draws three bars");
        checkEqual (Meter::gridStepQuarters ({ 4, 4 }, 30.0, 6.0, 0.25), 0.25, "4/4 at 30 px a quarter draws steps");

        // Two bands where 4/4 did move, pinned so that they are recorded
        // rather than discovered, because the two editors did not fall back on
        // the same thing before this and one of them changed much more.
        //
        // The playlist chose between a beat and a whole bar on `ppb >= 6`, so
        // from three to six pixels a quarter it drew bar lines and nothing
        // between them. The ladder climbs to the narrowest spacing that is at
        // least six pixels and still divides the bar, which there is the half
        // bar, so that band gains a line. Its zoom is clamped to [2, 400].
        checkEqual (Meter::gridStepQuarters ({ 4, 4 }, 4.0, 6.0, 1.0), 2.0,
                    "4/4 at 4 px a quarter draws half bars, where it used to draw bars");
        checkEqual (Meter::gridStepQuarters ({ 4, 4 }, 2.9, 6.0, 1.0), 4.0,
                    "and is back to bars as soon as the half bar is too narrow");

        // The piano roll drew a flat quarter of a beat at every zoom, down to
        // its own floor of eight pixels a quarter, where sixteenth lines are
        // two pixels apart and read as a smear rather than as a grid. It now
        // holds the quarter note until the sixteenth is six pixels wide, which
        // is twenty four pixels a quarter, so a sixteen pixel wide band of its
        // zoom loses the step lines and keeps the beat lines. That is the
        // change somebody will see, and it is the larger of the two.
        //
        // Deliberate, for the reason the two pixels gives, and it costs
        // nothing anybody was using: the snap branch is untouched in both
        // editors, so a sixteenth snap still draws sixteenths exactly where it
        // drew them before, at every zoom where six pixels fit.
        checkEqual (Meter::gridStepQuarters ({ 4, 4 }, 8.0, 6.0, 0.25), 1.0,
                    "4/4 at the piano roll's zoom floor draws quarters, where it used to draw steps");
        checkEqual (Meter::gridStepQuarters ({ 4, 4 }, 23.9, 6.0, 0.25), 1.0,
                    "and still does just below where a step is six pixels");
        checkEqual (Meter::gridStepQuarters ({ 4, 4 }, 24.0, 6.0, 0.25), 0.25,
                    "and draws steps from exactly there, which is the boundary being inclusive");
        // 99/32's bar is 12.375 quarters, and the only spacings that divide it
        // are the divisors of 99 times a thirty second: 3, 9, 11, 33 and 99 of
        // them. At four pixels a quarter the first one wide enough is 33, a
        // third of the bar. A doubling ladder would have stopped at four
        // quarters, which is not a part of 12.375 at all.
        checkEqual (Meter::gridStepQuarters ({ 99, 32 }, 4.0, 6.0, 0.125), 33.0 * 0.125,
                    "99/32 climbs to a third of its bar, which divides it");
        checkEqual (Meter::gridStepQuarters ({ 99, 32 }, 50.0, 6.0, 0.125), 0.125,
                    "and zoomed in it draws every thirty second");
    }

    // -----------------------------------------------------------------------
    // 8. What the studio will take from a keyboard.
    //
    // Strict on purpose: a field that read "4/3" as 4/4 would leave somebody
    // looking at a signature they did not type, and one that read it as the 4
    // and threw the rest away would be worse.
    // -----------------------------------------------------------------------
    {
        Meter::Signature out { 9, 16 };
        const Meter::Signature untouched = out;

        const auto accepts = [&out] (const std::string& text, int numerator, int denominator)
        {
            Meter::Signature got { 0, 0 };
            const bool ok = Meter::parse (text, got);
            check (ok, "\"" + text + "\" is accepted");
            check (ok && got == Meter::Signature { numerator, denominator },
                   "\"" + text + "\" reads as " + std::to_string (numerator) + "/" + std::to_string (denominator));
        };

        const auto refuses = [&out, &untouched] (const std::string& text)
        {
            out = untouched;
            check (! Meter::parse (text, out), "\"" + text + "\" is refused");
            check (out == untouched, "\"" + text + "\" leaves the signature alone");
        };

        accepts ("4/4", 4, 4);
        accepts ("7/8", 7, 8);
        accepts (" 6 / 8 ", 6, 8);
        accepts ("99/32", 99, 32);
        accepts ("1/1", 1, 1);

        refuses ("");
        refuses ("4");
        refuses ("4/");
        refuses ("/4");
        refuses ("4/4/4");
        refuses ("4-4");
        refuses ("4 4");          // the slash is not optional
        refuses ("four four");
        refuses ("4/4 swing");
        refuses ("4/3");          // not a power of two
        refuses ("0/4");
        refuses ("100/4");
        refuses ("4/64");
        refuses ("-4/4");
        refuses ("4/4.0");
        refuses ("1000/4");       // more digits than a numerator can have
        refuses ("4/1000");
        refuses ("99999999999/4");  // more digits than an int holds, let alone a numerator
        refuses ("4/99999999999");
        refuses ("999999999999999999999999/4");

        // And what it writes is what it reads, over every accepted signature.
        for (const auto& s : all)
        {
            Meter::Signature got { 0, 0 };
            check (Meter::parse (Meter::toString (s), got) && got == s,
                   name (s) + ": what the control shows is what the control takes");
        }
    }

    // -----------------------------------------------------------------------
    // 9. The Map's delegating accessors.
    //
    // Everything in the studio goes through these rather than through the free
    // functions, and nothing above touched them: an audit mutated each one and
    // all nine survived the suite. Making `barLengthQuartersAt` return a
    // display beat puts every bar line in the studio on the eighth note in
    // 6/8, loops the song at the end of an eighth, rounds a recorded take to
    // an eighth and makes a two bar count-in two eighths, and the tests were
    // green. So each one is checked against the free function it delegates to,
    // over every accepted signature, which is the whole of what it promises.
    // -----------------------------------------------------------------------
    {
        for (const auto& s : all)
        {
            Meter::Map map;
            check (map.setFirst (s), name (s) + ": the map takes it");

            // firstSignature is what ProjectIO::save writes. Returning the
            // default from it would save every 7/8 project as 4/4.
            check (map.firstSignature() == s, name (s) + ": firstSignature is what was set");

            for (double q : { 0.0, 0.5, 3.0, 17.25, 101.0, 1000.75 })
            {
                checkEqual (map.barLengthQuartersAt (q), Meter::barLengthQuarters (s),
                            name (s) + ": the map's bar length is the signature's bar");
                checkEqual (map.beatLengthQuartersAt (q), Meter::beatLengthQuarters (s),
                            name (s) + ": the map's display beat is the signature's");
                checkEqual (map.barFloorQuartersAt (q), Meter::barFloorQuarters (s, q),
                            name (s) + ": the map's bar floor is the signature's");
                checkEqual (map.roundUpToBarAt (q, 1.0e-3), Meter::roundUpToBar (s, q, 1.0e-3),
                            name (s) + ": the map's bar rounding is the signature's, tolerance and all");

                const auto a = map.barBeatAt (q);
                const auto b = Meter::barBeatAt (s, q);
                checkEqualInt (a.bar, b.bar,   name (s) + ": the map's bar is the signature's");
                checkEqualInt (a.beat, b.beat, name (s) + ": the map's beat is the signature's");
                checkEqual (a.fraction, b.fraction, name (s) + ": and so is the fraction");

                for (double fraction : { Meter::kSnapWholeBar, 1.0, 0.5, 0.25, 1.0 / 24.0, 0.0 })
                    checkEqual (map.snapQuartersAt (q, fraction), Meter::snapQuarters (s, fraction),
                                name (s) + ": the map's snap size is the signature's");

                // The accessor this section missed the first time, and the one
                // added by the same commit that made the free function
                // testable. Three mutations of its single line survived the
                // suite: handing the free function kDefault rather than the
                // signature, swapping the two pixel arguments, and returning
                // the raw unaligned spacing the function exists to avoid. The
                // first of those draws a 7/8 project's grid on the even
                // quarters at four pixels a quarter, which is an ordinary
                // zoom: lines at 0, 2, 4, 6 against bars at 0, 3.5, 7, and
                // every bar after the first with no line on it at all.
                //
                // The zooms span both editors' clamps, which are not the
                // same: the playlist holds `ppb` to [2, 400] and the piano
                // roll to [8, 800]. Both finest values are the ones the two
                // actually pass, a display beat and a quarter of one.
                //
                // The minimum is swept as well, even though both callers pass
                // six pixels today. Hardcoding it to six in this one line, and
                // raising anything smaller to six, were both surviving
                // mutations while the sweep only ever passed six: a later
                // caller asking for ten would silently have got six.
                // One of the swept minimums is below the six both callers
                // pass and one is above, because raising anything smaller to
                // six is a mutation that two values on the same side of six
                // cannot see. And the last zoom is a twentieth of a percent
                // inside the boundary where a display beat is exactly the
                // minimum, which is the only place a minimum relaxed by a
                // thousandth changes which rung comes back: without it, that
                // relaxation is invisible here however many round numbers are
                // swept.
                const double displayBeat = Meter::beatLengthQuarters (s);
                const double zooms[] { 2.0, 4.0, 13.0, 24.0, 800.0,
                                       0.9995 * 6.0 / displayBeat };

                for (double zoom : zooms)
                    for (double finest : { displayBeat, 0.25 * displayBeat })
                        for (double wanted : { 2.0, 6.0, 11.0 })
                            checkEqual (map.gridStepQuartersAt (q, zoom, wanted, finest),
                                        Meter::gridStepQuarters (s, zoom, wanted, finest),
                                        name (s) + ": the map's grid step is the signature's");
            }

            // The tolerance in particular: dropping it, or rounding down,
            // changes the length of every recorded clip and every clip the
            // piano roll grows, and nothing was asking.
            const double bar = Meter::barLengthQuarters (s);
            checkEqual (map.roundUpToBarAt (2.0 * bar + 1.0e-4, 1.0e-3), 2.0 * bar,
                        name (s) + ": the map keeps the tolerance");
            check (map.roundUpToBarAt (bar * 1.5, 1.0e-3) > bar * 1.5,
                   name (s) + ": and rounds up rather than down");
        }
    }

    // The 6/8 bar in seconds, which is the number the whole feature turns on.
    // Done here rather than in the engine because the engine cannot be
    // compiled in the sandbox, and this is the arithmetic the engine does.
    {
        const double bpm = 120.0;
        const double secondsPerQuarter = 60.0 / bpm;

        checkNear (Meter::barLengthQuarters ({ 4, 4 }) * secondsPerQuarter, 2.0, 1.0e-12,
                   "a 4/4 bar at 120 bpm is two seconds");
        checkNear (Meter::barLengthQuarters ({ 6, 8 }) * secondsPerQuarter, 1.5, 1.0e-12,
                   "a 6/8 bar at 120 bpm is a second and a half, not three seconds");
        checkNear (Meter::barLengthQuarters ({ 7, 8 }) * secondsPerQuarter, 1.75, 1.0e-12,
                   "a 7/8 bar at 120 bpm is 1.75 seconds");
        checkNear (Meter::barLengthQuarters ({ 4, 1 }) * secondsPerQuarter, 8.0, 1.0e-12,
                   "a 4/1 bar at 120 bpm is eight seconds");
    }

    if (failures > 0)
    {
        std::printf ("\n%d check(s) failed.\n", failures);
        return 1;
    }

    std::printf ("All checks passed.\n");
    return 0;
}
