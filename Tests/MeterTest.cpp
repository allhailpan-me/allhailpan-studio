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

        // A reader must not be talked into an impossible signature by a file.
        check (Meter::fromMetaEventBytes (6, 6)   == Meter::kDefault, "a denominator byte of 6 is refused");
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

        check (Meter::isValid (map.at (nan)), "a NaN position still answers with a signature");
        checkEqualInt (Meter::barOf ({ 7, 8 }, nan), 1, "a NaN position reads as bar 1 rather than wandering");
        checkEqualInt (Meter::barOf ({ 7, 8 }, inf), 1, "and so does an infinite one");
        checkEqualInt (Meter::barBeatAt ({ 7, 8 }, nan).beat, 1, "and its beat is 1");
        checkEqualInt (Meter::clickIndexAt ({ 7, 8 }, nan), 0, "a NaN position does not sound a click somewhere");
        checkEqual (Meter::roundUpToBar ({ 7, 8 }, nan, 1.0e-3), 0.0, "and rounds to nothing rather than to a NaN");
        checkEqual (Meter::roundUpToBar ({ 7, 8 }, 4.0, nan), 7.0, "a NaN tolerance is read as none");
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
