// ---------------------------------------------------------------------------
// Checks quantise: where a note actually ends up when it is moved onto the
// grid, and what the strength, the swing and the range gate really do.
//
// Every number this produces is a claim about agreement with other studios,
// so the useful check is not that the program is self consistent. It is that
// the positions match the published definitions, worked out here from those
// definitions rather than from the code under test. A strength that is a
// percentage of the wrong thing, or a gate whose sign is inverted, both leave
// a control that visibly does something, to the wrong notes, and that is the
// hardest kind of wrong to notice: the part still plays.
//
// The things that fail quietly, and are therefore checked hard:
//
//   - A strength of nothing has to leave the performance alone bit for bit.
//     Apple, Steinberg and Avid all publish that endpoint, and a quantiser
//     that rewrites every length by a rounding error while claiming to do
//     nothing would never be noticed until something else compared two
//     patterns.
//   - Strength has to be a percentage of the distance to the grid, not of the
//     step. Both read as "50 per cent" in a dialog and only one of them is a
//     quantiser. The discriminator is Steinberg's published property that
//     "repeated use moves them progressively closer to the grid": two passes
//     at strength s leave exactly (1 - s)^2 of the offset, which no snapping
//     shortcut reproduces.
//   - The downbeats must never move, at any swing. A grid that nudges the
//     beats as well as the offbeats is not swing, it is a timing error.
//   - Destructive quantise and Groove.h have to agree about what a swing
//     number means, or the studio contradicts itself between an edit and a
//     playback setting. That is the same class of fault as playback and export
//     disagreeing.
//   - The gate's two senses have to be exactly complementary. Inverted, it is
//     a control that tidies the notes that were already right and leaves the
//     stragglers.
//   - A quantised pattern has to export as itself. Quantise is the operation
//     that lands two notes of one pitch on top of each other, and a file
//     cannot hold that, so anything left unresolved here is a note that
//     vanishes on the way out.
//   - No note may come out with no length. Both ends of a note shorter than
//     the grid can land on the same point, and a note on and a note off at the
//     same tick is a note nothing plays.
//
// Quantise.h has no JUCE in it, so this builds and runs on its own with
// sanitizers in a couple of seconds. CI runs it on every push.
//
//   ./Tests/run.sh
//
// Sources, all of them manufacturers' own manuals:
//   Avid, Pro Tools help, "What to Quantize" and "Additional Grid Quantize
//     Options": strength as a percentage moved toward the grid, Include Within
//     and Exclude Within with two worked examples that quantify the window,
//     and Preserve Note Duration being unavailable when both ends are
//     quantised.
//   Apple, Logic Pro User Guide, "Quantize parameters" and the quantize grid
//     table: Q-Strength's two endpoints, Q-Swing as the position of every
//     second grid point, and the six named presets 50, 54, 58, 62, 66, 71.
//   Steinberg, Cubase Operation Manual and Plug-in Reference: strength at
//     100 per cent forcing notes to the closest grid position, and repeated
//     iterative quantise moving them progressively closer.
//   Ableton Live manual: quantising a note end "will stretch the note".
//   Checked here against positions computed from those definitions, not from
//   Quantise.h.
// ---------------------------------------------------------------------------

#include "Quantise.h"
#include "Groove.h"
#include "MidiTimebase.h"

#include <cstdio>
#include <random>
#include <string>
#include <vector>

static int failures = 0;

static void fail (const std::string& what)
{
    std::printf ("  FAIL  %s\n", what.c_str());
    ++failures;
}

static void check (bool ok, const std::string& what)
{
    if (! ok)
        fail (what);
}

static void checkClose (double a, double b, double tol, const std::string& what)
{
    if (! (std::abs (a - b) <= tol))
        fail (what + "  (" + std::to_string (a) + " vs " + std::to_string (b)
              + ", tol " + std::to_string (tol) + ")");
}

static void checkExact (double a, double b, const std::string& what)
{
    if (a != b)
        fail (what + "  (" + std::to_string (a) + " vs " + std::to_string (b) + ")");
}

static TimebaseNote note (double start, double length, int pitch = 60)
{
    TimebaseNote n;
    n.start  = start;
    n.length = length;
    n.note   = pitch;
    return n;
}

// The grid, worked out from the definition rather than from the header: every
// second point is an offbeat and sits at the swing fraction of its pair, and
// the others are whole numbers of steps from the origin.
static double expectedPoint (long long index, double step, double swing)
{
    const long long pairIndex = index / 2;
    const double pairStart = (double) (pairIndex * 2) * step;
    return (index % 2 == 0) ? pairStart : pairStart + swing * step * 2.0;
}

static Quantise::Options straight (double grid, double strength = 1.0)
{
    Quantise::Options o;
    o.grid = grid;
    o.strength = strength;
    return o;
}

// ---------------------------------------------------------------------------

static void nothingHappensAtNoStrength()
{
    std::mt19937 rng (1234);
    std::uniform_real_distribution<double> position (0.0, 32.0);
    std::uniform_real_distribution<double> length (0.01, 2.0);

    std::vector<TimebaseNote> notes;
    for (int i = 0; i < 400; ++i)
        notes.push_back (note (position (rng), length (rng), 40 + (i % 40)));

    for (double grid : { 1.0, 0.5, 0.25, 1.0 / 3.0 })
    {
        for (double swing : { 0.5, 0.58, 0.6667 })
        {
            auto copy = notes;
            auto o = straight (grid, 0.0);
            o.swing = swing;
            const auto result = Quantise::apply (copy, o);

            check (result.moved == 0, "a strength of nothing moved a note");
            check (result.dropped == 0, "a strength of nothing lost a note");
            check (result.trimmed == 0, "a strength of nothing shortened a note");
            check (copy.size() == notes.size(), "a strength of nothing changed the note count");

            // Bit for bit, and in the order they were in. The material here
            // deliberately contains overlapping notes of the same pitch, which
            // a piano roll will draw and which quantise has no business
            // tidying when it has not moved anything.
            for (size_t i = 0; i < notes.size() && i < copy.size(); ++i)
            {
                checkExact (copy[i].start, notes[i].start,
                            "a strength of nothing moved a note");
                checkExact (copy[i].length, notes[i].length,
                            "a strength of nothing changed a note's length");
                check (copy[i].note == notes[i].note,
                       "a strength of nothing reordered the notes");
            }
        }
    }
}

static void fullStrengthLandsOnTheGrid()
{
    std::mt19937 rng (99);
    std::uniform_real_distribution<double> offset (-0.49, 0.49);

    for (double grid : { 1.0, 0.5, 0.25, 0.125 })
    {
        for (double swing : { 0.5, 0.54, 0.62, 0.71 })
        {
            auto o = straight (grid);
            o.swing = swing;

            for (long long index = 0; index < 64; ++index)
            {
                const double point = expectedPoint (index, grid, swing);

                // Near enough to this point that it is unambiguously the
                // nearest one, whatever the swing has done to the spacing.
                const double played = point + offset (rng) * grid * 0.4;

                checkClose (Quantise::place (played, o), point, 1.0e-12,
                            "a full strength quantise did not land on the grid point");
            }
        }
    }
}

static void strengthIsAPercentageOfTheDistance()
{
    std::mt19937 rng (7);
    std::uniform_real_distribution<double> strengths (0.01, 0.99);
    std::uniform_real_distribution<double> away (-0.4, 0.4);

    for (int i = 0; i < 20000; ++i)
    {
        const double grid = (i % 2 == 0) ? 0.25 : 0.5;
        const double s = strengths (rng);
        const long long index = i % 32;
        const double point = expectedPoint (index, grid, 0.5);
        const double delta = away (rng) * grid;
        const double played = point + delta;

        auto o = straight (grid, s);
        const double placed = Quantise::place (played, o);

        // The displacement is the strength times the distance to the grid.
        checkClose (placed - played, s * (point - played), 1.0e-12,
                    "the displacement was not the strength times the distance");

        // And what is left of the offset is (1 - s) of it, which is the half
        // of the definition that a percentage of the step would get wrong.
        checkClose (placed - point, (1.0 - s) * delta, 1.0e-12,
                    "the remaining offset was not (1 - s) of the original");
    }
}

static void repeatedPassesCloseInWithoutArriving()
{
    // Steinberg: "Repeated use moves them progressively closer to the grid."
    // Interpolation leaves exactly (1 - s)^n of the offset after n passes, and
    // never zero below full strength. A quantiser that snaps anything within
    // some tolerance would arrive on the second pass.
    std::mt19937 rng (31337);
    std::uniform_real_distribution<double> strengths (0.05, 0.95);

    for (int i = 0; i < 4000; ++i)
    {
        const double grid = 0.25;
        const double s = strengths (rng);
        const double point = 2.0;
        const double delta = (i % 2 == 0 ? 1.0 : -1.0) * grid * 0.37;

        auto o = straight (grid, s);

        double where = point + delta;
        for (int pass = 1; pass <= 4; ++pass)
        {
            where = Quantise::place (where, o);
            const double expected = delta * std::pow (1.0 - s, (double) pass);

            checkClose (where - point, expected, 1.0e-12,
                        "a repeated pass did not leave (1 - s)^n of the offset");
            check (where != point, "a pass below full strength arrived exactly on the grid");
        }
    }
}

static void fullStrengthIsIdempotent()
{
    std::mt19937 rng (5150);
    std::uniform_real_distribution<double> position (0.0, 16.0);
    std::uniform_real_distribution<double> length (0.05, 1.5);

    std::vector<TimebaseNote> notes;
    for (int i = 0; i < 200; ++i)
        notes.push_back (note (position (rng), length (rng), 48 + (i % 24)));

    for (double swing : { 0.5, 0.66 })
    {
        for (auto target : { Quantise::Target::starts, Quantise::Target::ends,
                             Quantise::Target::both })
        {
            auto o = straight (0.25);
            o.swing = swing;
            o.target = target;

            auto once = notes;
            Quantise::apply (once, o);

            auto twice = once;
            const auto second = Quantise::apply (twice, o);

            check (second.dropped == 0, "a second full strength pass lost a note");
            check (once.size() == twice.size(), "a second pass changed the note count");

            if (target == Quantise::Target::starts)
            {
                // Quantising starts is a tidy up and has to be idempotent.
                check (second.moved == 0, "a second pass moved a start that was already on the grid");

                for (size_t i = 0; i < once.size() && i < twice.size(); ++i)
                {
                    checkExact (twice[i].start, once[i].start, "a second pass moved a start");
                    checkExact (twice[i].length, once[i].length, "a second pass changed a length");
                }
                continue;
            }

            // Quantising ends is not quite idempotent, and the reason is
            // structural rather than a fault: an end put on the grid and then
            // trimmed back to where the next note of its pitch begins is no
            // longer on the grid, because the trim answers to a different
            // constraint. What has to hold is that it settles, that it settles
            // downward, and that it never costs a note. Those three are what
            // tell this apart from an oscillation.
            auto thrice = twice;
            const auto third = Quantise::apply (thrice, o);
            check (third.dropped == 0, "a third pass lost a note");
            check (thrice.size() == twice.size(), "a third pass changed the note count");

            for (size_t i = 0; i < twice.size() && i < thrice.size(); ++i)
            {
                checkExact (thrice[i].start, twice[i].start, "the passes had not settled by the third");
                checkExact (thrice[i].length, twice[i].length, "the passes had not settled by the third");
                check (twice[i].length <= once[i].length + 1.0e-12,
                       "a later pass made a note longer");
            }
        }
    }
}

static void closerAtEveryHigherStrength()
{
    const double grid = 0.5;
    const double point = 1.5;      // a straight grid point
    const double played = point + 0.21;

    double previous = std::abs (played - point);

    for (int percent = 0; percent <= 100; ++percent)
    {
        auto o = straight (grid, percent / 100.0);
        const double distance = std::abs (Quantise::place (played, o) - point);

        check (distance <= previous + 1.0e-15,
               "a higher strength left the note further from the grid");
        previous = distance;
    }

    checkClose (previous, 0.0, 1.0e-12, "full strength did not reach the grid");
}

// ---------------------------------------------------------------------------
// Swing

static void straightSwingIsNoSwing()
{
    std::mt19937 rng (4242);
    std::uniform_real_distribution<double> position (0.0, 24.0);

    for (int i = 0; i < 2000; ++i)
    {
        const double grid = (i % 3 == 0) ? 0.5 : 0.25;
        const double played = position (rng);

        auto swung = straight (grid);
        swung.swing = 0.5;

        // Worked out from the definition: with the offbeat at half of its
        // pair, every grid point is a whole number of steps from the origin.
        const double expected = std::round (played / grid) * grid;

        checkClose (Quantise::place (played, swung), expected, 1.0e-12,
                    "a swing of exactly half did not give a plain grid");
    }
}

static void theOffbeatLandsWhereTheNumberSays()
{
    // Apple's own table of named swing presets, which is six exact points on
    // the scale rather than one.
    for (double percent : { 50.0, 54.0, 58.0, 62.0, 66.0, 71.0 })
    {
        const double swing = percent / 100.0;

        for (double grid : { 0.5, 0.25 })
        {
            auto o = straight (grid);
            o.swing = swing;

            for (long long pair = 0; pair < 8; ++pair)
            {
                const double pairStart = (double) (pair * 2) * grid;
                const double offbeat = pairStart + swing * grid * 2.0;

                // A note played a hair off the swung offbeat has to come back
                // to it, and the downbeat must not move at all.
                checkClose (Quantise::place (offbeat + grid * 0.02, o), offbeat, 1.0e-12,
                            "an offbeat did not land at the swing percentage of its pair");
                checkExact (Quantise::place (pairStart, o), pairStart,
                            "a downbeat moved when swing was applied");
            }
        }
    }

    // The triplet, which is the one value with a name of its own: two thirds
    // of the way through the pair is the third triplet.
    auto o = straight (0.5);
    o.swing = 2.0 / 3.0;
    checkClose (Quantise::place (0.68, o), 2.0 / 3.0, 1.0e-12,
                "a swing of two thirds did not put the offbeat on the third triplet");
}

static void belowHalfPullsTheOffbeatEarly()
{
    // Apple: "values over 50% delay the beats; values under 50% pre-delay
    // (advance) them". Most implementations silently clamp the low side.
    const double grid = 0.25;
    auto early = straight (grid);
    early.swing = 0.42;

    const double offbeat = 0.42 * grid * 2.0;
    checkClose (Quantise::place (offbeat + 0.001, early), offbeat, 1.0e-12,
                "a swing below half did not advance the offbeat");
    check (offbeat < grid, "a swing below half did not place the offbeat early");
}

static void downbeatsNeverMoveAtAnySwing()
{
    for (int percent = 25; percent <= 75; ++percent)
    {
        for (double grid : { 1.0, 0.5, 0.25, 0.125 })
        {
            auto o = straight (grid);
            o.swing = percent / 100.0;

            for (long long pair = 0; pair < 16; ++pair)
            {
                const double downbeat = (double) (pair * 2) * grid;
                checkExact (Quantise::place (downbeat, o), downbeat,
                            "a downbeat moved at some swing value");
            }
        }
    }
}

static void quantiseAndGrooveAgreeAboutSwing()
{
    // The cross-check that matters most: a note quantised destructively at a
    // swing, and a note left straight and played through a Groove at the same
    // swing, have to land in the same place. If these disagree the studio
    // contradicts itself between an edit and a playback setting.
    struct Resolution { Groove::Base base; double grid; };
    const Resolution resolutions[] = { { Groove::Base::eighth, 0.5 },
                                       { Groove::Base::sixteenth, 0.25 },
                                       { Groove::Base::thirtySecond, 0.125 } };

    for (const auto& resolution : resolutions)
    {
        for (int percent = 25; percent <= 75; ++percent)
        {
            Groove g;
            g.enabled = true;
            g.base = resolution.base;
            g.swing = percent / 100.0;
            g.amount = 1.0;
            g.velocity = 0.0;
            g.random = 0.0;

            auto o = straight (resolution.grid);
            o.swing = g.swing;

            for (long long step = 0; step < 32; ++step)
            {
                const double onGrid = (double) step * resolution.grid;
                const double byGroove = g.place (onGrid, 60);
                const double byQuantise = Quantise::place (onGrid, o);

                checkClose (byQuantise, byGroove, 1.0e-12,
                            "quantise and Groove disagree about where a swing puts a note");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The range gate

static void avidsWorkedExamplesComeOutRight()
{
    // Avid, Additional Grid Quantize Options: "the Quantize Grid set to
    // quarter notes with the Include Within option set to 50%" gives "an
    // eighth note area around each beat", so the window is half a quarter
    // wide and the tolerance is an eighth note either side... which is a
    // sixteenth of a beat wide each way. A note further off than that must
    // not move.
    auto include = straight (1.0);
    include.gate = Quantise::Gate::include;
    include.range = 0.5;

    const double tolerance = 0.25;    // half of 50 per cent of a quarter note

    checkClose (Quantise::place (1.0 + tolerance * 0.9, include), 1.0, 1.0e-12,
                "Include Within did not quantise a note inside its window");
    checkExact (Quantise::place (1.0 + tolerance * 1.1, include), 1.0 + tolerance * 1.1,
                "Include Within quantised a note outside its window");

    // "the Quantize Grid set to quarter notes with the Exclude Within option
    // set to 25%" gives "a sixteenth note area", inside which notes are not
    // quantised.
    auto exclude = straight (1.0);
    exclude.gate = Quantise::Gate::exclude;
    exclude.range = 0.25;

    const double safe = 0.125;

    checkExact (Quantise::place (1.0 + safe * 0.9, exclude), 1.0 + safe * 0.9,
                "Exclude Within quantised a note inside its safe zone");
    checkClose (Quantise::place (1.0 + safe * 1.1, exclude), 1.0, 1.0e-12,
                "Exclude Within did not quantise a note outside its safe zone");
}

static void theTwoSensesAreComplementary()
{
    // Whatever the window, every note is quantised by exactly one of the two
    // senses. This is the assertion that makes the sign impossible to ship
    // backwards, and it needs off grid material: a note already on the grid
    // does not move under either.
    std::mt19937 rng (808);
    std::uniform_real_distribution<double> position (0.0, 20.0);

    for (int i = 0; i < 20000; ++i)
    {
        const double grid = (i % 2 == 0) ? 0.25 : 0.5;
        const double range = 0.05 + (i % 19) * 0.05;
        const double played = position (rng);

        auto include = straight (grid);
        include.gate = Quantise::Gate::include;
        include.range = range;

        auto exclude = include;
        exclude.gate = Quantise::Gate::exclude;

        auto off = include;
        off.gate = Quantise::Gate::off;

        const double byOff = Quantise::place (played, off);

        // Off grid by construction: skip anything that happens to be sitting
        // on a grid point, where neither sense moves it and the test would be
        // measuring nothing.
        if (std::abs (byOff - played) < 1.0e-9)
            continue;

        const bool movedByInclude = Quantise::place (played, include) != played;
        const bool movedByExclude = Quantise::place (played, exclude) != played;

        check (movedByInclude != movedByExclude,
               "a note was quantised by both senses of the gate, or by neither");

        // And whichever one moves it lands it where no gate at all would.
        const double moved = movedByInclude ? Quantise::place (played, include)
                                            : Quantise::place (played, exclude);
        checkExact (moved, byOff, "the gate changed where a note landed, not whether it moved");
    }
}

static void aFullWindowIsNoGateOnAStraightGrid()
{
    std::mt19937 rng (2024);
    std::uniform_real_distribution<double> position (0.0, 12.0);

    for (int i = 0; i < 2000; ++i)
    {
        const double grid = 0.25;
        const double played = position (rng);

        auto include = straight (grid);
        include.gate = Quantise::Gate::include;
        include.range = 1.0;

        auto off = straight (grid);

        checkExact (Quantise::place (played, include), Quantise::place (played, off),
                    "a full window behaved differently from no gate at all");
    }

    // Under swing the gaps between grid points are unequal, so a full window
    // is not the whole step any more: this is a real consequence of Avid's
    // definition rather than a bug, and it is written down so nobody
    // "corrects" it. At a swing of 0.75 the gap from the downbeat to the
    // offbeat is one and a half steps, and its middle is three quarters of a
    // step from either, which is outside a window half a step wide.
    const double grid = 0.25;
    auto include = straight (grid);
    include.gate = Quantise::Gate::include;
    include.range = 1.0;
    include.swing = 0.75;

    const double middle = 0.75 * grid;
    checkExact (Quantise::place (middle, include), middle,
                "a full window moved a note sitting in a widened swing gap");
}

static void aNoteOnTheGridIsLeftAloneByEveryGate()
{
    for (auto gate : { Quantise::Gate::off, Quantise::Gate::include, Quantise::Gate::exclude })
    {
        for (double range : { 0.0, 0.25, 0.5, 1.0 })
        {
            auto o = straight (0.25);
            o.gate = gate;
            o.range = range;

            for (long long index = 0; index < 16; ++index)
            {
                const double point = expectedPoint (index, 0.25, 0.5);
                checkExact (Quantise::place (point, o), point,
                            "a note already on the grid was moved by a gate");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Lengths and ends

static void startsOnlyKeepsEveryLength()
{
    std::mt19937 rng (616);
    std::uniform_real_distribution<double> position (0.0, 16.0);
    std::uniform_real_distribution<double> length (0.02, 3.0);

    std::vector<TimebaseNote> notes;
    for (int i = 0; i < 300; ++i)
        notes.push_back (note (position (rng), length (rng), 30 + i));   // a pitch each, so nothing collides

    auto o = straight (0.25);
    o.target = Quantise::Target::starts;
    o.preserveLength = true;

    auto copy = notes;
    Quantise::apply (copy, o);

    check (copy.size() == notes.size(), "a starts only quantise lost a note");

    for (const auto& original : notes)
    {
        bool found = false;
        for (const auto& n : copy)
            if (n.note == original.note)
            {
                found = true;
                checkExact (n.length, original.length,
                            "a starts only quantise changed a note's length");
            }
        check (found, "a note disappeared from a starts only quantise");
    }
}

static void endsOnlyLeavesEveryStart()
{
    std::mt19937 rng (717);
    std::uniform_real_distribution<double> position (0.0, 16.0);
    std::uniform_real_distribution<double> length (0.02, 3.0);

    std::vector<TimebaseNote> notes;
    for (int i = 0; i < 300; ++i)
        notes.push_back (note (position (rng), length (rng), 30 + i));

    auto o = straight (0.25);
    o.target = Quantise::Target::ends;

    auto copy = notes;
    Quantise::apply (copy, o);

    for (const auto& original : notes)
        for (const auto& n : copy)
            if (n.note == original.note)
                checkExact (n.start, original.start,
                            "an ends only quantise moved a start");
}

static void aFixedEndMeansTheLengthChanges()
{
    // Avid: "When deselected with the Note On option selected, note end
    // points are not moved." So the note keeps its end and its length is
    // whatever that leaves.
    std::vector<TimebaseNote> notes { note (1.1, 0.8, 64) };

    auto o = straight (0.5);
    o.target = Quantise::Target::starts;
    o.preserveLength = false;

    auto copy = notes;
    Quantise::apply (copy, o);

    checkClose (copy[0].start, 1.0, 1.0e-12, "the start did not quantise");
    checkClose (copy[0].start + copy[0].length, 1.1 + 0.8, 1.0e-12,
                "the end moved when it was supposed to stay");
}

static void preserveLengthIsRefusedWhenBothEndsMove()
{
    // Avid makes the option unavailable in exactly this case rather than
    // silently honouring one of two contradictory requests.
    Quantise::Options both;
    both.target = Quantise::Target::both;
    check (! Quantise::preserveLengthApplies (both),
           "preserving a length was offered while both ends were being quantised");

    Quantise::Options starts;
    starts.target = Quantise::Target::starts;
    check (Quantise::preserveLengthApplies (starts),
           "preserving a length was refused for a starts only quantise");

    Quantise::Options ends;
    ends.target = Quantise::Target::ends;
    check (! Quantise::preserveLengthApplies (ends),
           "preserving a length was offered for an ends only quantise");

    // And asking for it anyway has to be refused, not quietly obeyed.
    std::mt19937 rng (909);
    std::uniform_real_distribution<double> position (0.0, 8.0);
    std::uniform_real_distribution<double> length (0.02, 1.0);

    std::vector<TimebaseNote> notes;
    for (int i = 0; i < 200; ++i)
        notes.push_back (note (position (rng), length (rng), 30 + i));

    auto asked = straight (0.25);
    asked.target = Quantise::Target::both;
    asked.preserveLength = true;

    auto refused = asked;
    refused.preserveLength = false;

    auto a = notes, b = notes;
    Quantise::apply (a, asked);
    Quantise::apply (b, refused);

    check (a.size() == b.size(), "the refused request changed the note count");
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
    {
        checkExact (a[i].start, b[i].start, "the refused request moved a start");
        checkExact (a[i].length, b[i].length, "the refused request changed a length");
    }
}

static void bothEndsOnTheGridStillLeavesANoteToPlay()
{
    // The case that produces the fault: notes far shorter than the grid, so
    // both of their ends want the same grid point.
    std::mt19937 rng (1001);
    std::uniform_real_distribution<double> position (0.0, 16.0);
    std::uniform_real_distribution<double> tiny (0.0005, 0.02);

    std::vector<TimebaseNote> notes;
    for (int i = 0; i < 500; ++i)
        notes.push_back (note (position (rng), tiny (rng), 20 + i % 100));

    for (double grid : { 1.0, 0.5, 0.25 })
    {
        auto o = straight (grid);
        o.target = Quantise::Target::both;

        auto copy = notes;
        Quantise::apply (copy, o);

        for (const auto& n : copy)
            check (n.length >= Quantise::kMinLengthBeats,
                   "a note came out shorter than a tick, which is a note nothing plays");
    }
}

// ---------------------------------------------------------------------------
// Overlaps, and the export the pattern has to survive

static void aQuantisedPatternExportsAsItself()
{
    std::mt19937 rng (31415);
    std::uniform_real_distribution<double> position (0.0, 32.0);
    std::uniform_real_distribution<double> length (0.01, 2.0);
    std::uniform_int_distribution<int> pitch (36, 48);          // few pitches, so collisions are common
    std::uniform_int_distribution<int> howMany (2, 40);

    for (int pattern = 0; pattern < 2000; ++pattern)
    {
        // A pattern the export was already happy with, so that anything it
        // has to repair afterwards was created by the quantise. Notes of one
        // pitch are laid out in order and trimmed so none overlaps the next,
        // which is what playing a part in gives you.
        std::vector<TimebaseNote> notes;
        const int count = howMany (rng);

        for (int p = 36; p <= 48; ++p)
        {
            double where = position (rng) * 0.1;
            for (int i = 0; i < count / 4 + 1; ++i)
            {
                const double gap = 0.05 + length (rng);
                const double len = std::min (length (rng), gap * 0.9);
                notes.push_back (note (where, len, p));
                where += gap;
            }
        }

        int droppedByInput = 0;
        midiNotesToTicks (notes, kMidiExportPpq, &droppedByInput);
        check (droppedByInput == 0, "the test's own material was not exportable to begin with");

        auto o = straight ((pattern % 3 == 0) ? 1.0 : (pattern % 3 == 1) ? 0.5 : 0.25);
        o.target = (pattern % 2 == 0) ? Quantise::Target::starts : Quantise::Target::both;
        o.swing = (pattern % 5 == 0) ? 0.58 : 0.5;

        const auto before = notes.size();
        const auto result = Quantise::apply (notes, o);

        check (notes.size() + (size_t) result.dropped == before,
               "the dropped count does not account for the notes that went missing");

        // Nothing of one pitch may overlap anything else of that pitch.
        for (size_t i = 0; i < notes.size(); ++i)
            for (size_t j = i + 1; j < notes.size(); ++j)
                if (notes[i].note == notes[j].note)
                {
                    const auto& first = notes[i].start <= notes[j].start ? notes[i] : notes[j];
                    const auto& second = notes[i].start <= notes[j].start ? notes[j] : notes[i];
                    check (first.start + first.length <= second.start + 1.0e-12,
                           "two notes of one pitch overlap after quantising");
                }

        // In playing order, which is what a roll and a file both want.
        for (size_t i = 1; i < notes.size(); ++i)
            check (notes[i - 1].start <= notes[i].start, "the notes came back out of order");

        // And the file holds exactly this: nothing left for the export to
        // throw away.
        int droppedByExport = 0;
        const auto ticks = midiNotesToTicks (notes, kMidiExportPpq, &droppedByExport);
        check (droppedByExport == 0,
               "exporting a quantised pattern still had to throw a note away");
        check (ticks.size() == notes.size(), "the export changed the note count");

        for (const auto& t : ticks)
            check (t.endTick > t.startTick, "a quantised note exported with no length");
    }
}

static void everythingSurvivesAnythingAskedOfIt()
{
    // Thousands of randomised patterns against randomised options, including
    // nonsense ones, asserting only the invariants. This is the pass that has
    // found the most elsewhere in this project.
    std::mt19937 rng (27182);
    std::uniform_real_distribution<double> position (-1.0, 40.0);
    std::uniform_real_distribution<double> length (-0.1, 4.0);
    std::uniform_real_distribution<double> unit (-0.5, 1.5);
    std::uniform_int_distribution<int> pitch (0, 127);
    std::uniform_int_distribution<int> howMany (0, 30);
    std::uniform_int_distribution<int> which (0, 2);

    const double grids[] = { 1.0, 0.5, 0.25, 1.0 / 3.0, 0.125, 1.0 / 12.0, 0.0, -1.0 };

    for (int round = 0; round < 6000; ++round)
    {
        std::vector<TimebaseNote> notes;
        const int count = howMany (rng);
        for (int i = 0; i < count; ++i)
            notes.push_back (note (position (rng), length (rng), pitch (rng)));

        Quantise::Options o;
        o.grid = grids[(size_t) (round % 8)];
        o.strength = unit (rng);
        o.swing = unit (rng);
        o.range = unit (rng);
        o.target = (Quantise::Target) which (rng);
        o.gate = (Quantise::Gate) which (rng);
        o.preserveLength = (round % 2 == 0);

        const auto before = notes.size();
        const auto result = Quantise::apply (notes, o);

        check (notes.size() <= before, "quantising invented a note");
        check (result.dropped >= 0 && result.moved >= 0 && result.trimmed >= 0,
               "a count came back negative");

        for (const auto& n : notes)
        {
            check (std::isfinite (n.start) && std::isfinite (n.length),
                   "a note came out with a position that is not a number");
            check (n.start >= 0.0, "a note came out before the start of the clip");
            check (n.length > 0.0, "a note came out with no length at all");
            check (n.note >= 0 && n.note <= 127, "a pitch left the MIDI range");

            // The floor is claimed for notes this pass touched. A note already
            // shorter than a tick that nothing asked to move is left exactly
            // as it was found, which is the same rule as the strength of
            // nothing: quantise does not quietly repair what it did not move.
            if (result.moved > 0 && result.dropped == 0 && result.trimmed == 0
                && (size_t) result.moved == notes.size())
                check (n.length >= Quantise::kMinLengthBeats,
                       "a note this pass moved came out shorter than a tick");
        }

        // Playing order, once anything has moved. A pass that moved nothing
        // does not even reorder, which is the point of the early return.
        if (result.moved > 0)
            for (size_t i = 1; i < notes.size(); ++i)
                check (notes[i - 1].start <= notes[i].start, "the notes came back out of order");
    }
}

static void theAvidSwingScaleConverts()
{
    // Three points published by two different sources: 0 per cent straight,
    // 100 per cent the third triplet, 300 per cent the offbeat on the next
    // grid point.
    checkClose (Quantise::swingFromAvidPercent (0.0), 0.5, 1.0e-15,
                "Avid's zero per cent is not straight on this scale");
    checkClose (Quantise::swingFromAvidPercent (100.0), 2.0 / 3.0, 1.0e-15,
                "Avid's hundred per cent is not the third triplet on this scale");
    checkClose (Quantise::swingFromAvidPercent (300.0), 1.0, 1.0e-15,
                "Avid's three hundred per cent is not the next grid point");
}

int main()
{
    std::printf ("quantise\n");

    nothingHappensAtNoStrength();
    fullStrengthLandsOnTheGrid();
    strengthIsAPercentageOfTheDistance();
    repeatedPassesCloseInWithoutArriving();
    fullStrengthIsIdempotent();
    closerAtEveryHigherStrength();

    straightSwingIsNoSwing();
    theOffbeatLandsWhereTheNumberSays();
    belowHalfPullsTheOffbeatEarly();
    downbeatsNeverMoveAtAnySwing();
    quantiseAndGrooveAgreeAboutSwing();
    theAvidSwingScaleConverts();

    avidsWorkedExamplesComeOutRight();
    theTwoSensesAreComplementary();
    aFullWindowIsNoGateOnAStraightGrid();
    aNoteOnTheGridIsLeftAloneByEveryGate();

    startsOnlyKeepsEveryLength();
    endsOnlyLeavesEveryStart();
    aFixedEndMeansTheLengthChanges();
    preserveLengthIsRefusedWhenBothEndsMove();
    bothEndsOnTheGridStillLeavesANoteToPlay();

    aQuantisedPatternExportsAsItself();
    everythingSurvivesAnythingAskedOfIt();

    if (failures != 0)
    {
        std::printf ("\n%d quantise check(s) failed\n", failures);
        return 1;
    }

    std::printf ("all quantise checks passed\n");
    return 0;
}
