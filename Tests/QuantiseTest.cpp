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
//   - A note sitting exactly between two grid lines has to go to the same one
//     every time. That is not a curiosity: quantising eighths to a quarter
//     grid puts every offbeat exactly there, so the tie is the ordinary case
//     and a tie broken the other way quantises half a pattern backwards.
//   - An overlap quantise did not create is none of its business. A piano roll
//     will draw two notes of one pitch on top of each other, and a pass that
//     tidies those while moving something else has deleted part of somebody's
//     arrangement without being asked.
//   - Where two notes really do collide, the earlier one loses its tail and
//     the later one lives. Dropping the later note instead leaves a pattern a
//     file can hold just as well and loses something that was played, which is
//     the worst thing this operation can do.
//   - A setting out of range has to behave like the bound it is clamped to. A
//     swing of one puts the offbeat on top of the next downbeat, where "the
//     nearest grid point" stops having an answer at all.
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
#include <limits>
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
    // Floor division rather than C++'s truncating one, so that a negative
    // index counts its pairs the way a positive one does. A clip's notes can
    // sit left of its own origin, and a grid that counts pairs from zero
    // outward in one direction and inward in the other is not a grid.
    const long long pairIndex = (long long) std::floor ((double) index / 2.0);
    const double pairStart = (double) (pairIndex * 2) * step;
    return (index - pairIndex * 2 == 0) ? pairStart : pairStart + swing * step * 2.0;
}

// The nearest point of the grid, found by scanning the grid itself rather than
// by the six candidate search the header uses. The grid is written out from
// the definition: the downbeats are whole pairs from the origin, and every
// second point sits at the swing fraction of its pair. Scanning upward and
// keeping only a strictly nearer candidate is what "ties go to the earlier
// point" means, so this is the reference for the tie as well as for the
// distance.
static double nearestByDefinition (double beat, double step, double swing)
{
    const double pair = step * 2.0;
    const long long middle = (long long) std::floor (beat / pair);

    double best = 0.0;
    double bestDistance = -1.0;

    for (long long k = middle - 6; k <= middle + 6; ++k)
        for (int member = 0; member < 2; ++member)
        {
            const double candidate = expectedPoint (k * 2 + member, step, swing);
            const double distance = std::abs (beat - candidate);

            if (bestDistance < 0.0 || distance < bestDistance)
            {
                bestDistance = distance;
                best = candidate;
            }
        }

    return best;
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

static void aSettingOutOfRangeBehavesLikeItsBound()
{
    // A setting out of range is not a hypothetical: it arrives from a project
    // saved by a later version, from a host automating a parameter over its
    // own end, or from a number typed into a box. The header's answer is a
    // clamp, and the clamp has to be the whole answer, because each of the
    // three means something different gone wrong. A strength above one flings
    // the note past the grid and out the other side. A swing of one puts the
    // offbeat on top of the next downbeat, where the grid has half as many
    // distinct points and "the nearest" has no answer. A window wider than
    // the step reaches into the next step's territory.
    std::mt19937 rng (2600);
    std::uniform_real_distribution<double> position (-8.0, 40.0);

    for (int i = 0; i < 8000; ++i)
    {
        const double grid = (i % 3 == 0) ? 1.0 : (i % 3 == 1) ? 0.25 : 1.0 / 3.0;
        const double played = position (rng);

        auto atTheBound = straight (grid);
        auto beyond = atTheBound;

        atTheBound.strength = 1.0;
        beyond.strength = 1.0 + (double) (i % 7) + 0.25;
        checkExact (Quantise::place (played, beyond), Quantise::place (played, atTheBound),
                    "a strength above one did not behave like full strength");

        atTheBound.strength = 0.0;
        beyond.strength = -0.25 - (double) (i % 7);
        checkExact (Quantise::place (played, beyond), Quantise::place (played, atTheBound),
                    "a strength below nothing did not leave the performance alone");

        atTheBound.strength = beyond.strength = 1.0;

        atTheBound.swing = Quantise::kMaxSwing;
        beyond.swing = Quantise::kMaxSwing + 0.01 * (double) (1 + i % 40);
        checkExact (Quantise::place (played, beyond), Quantise::place (played, atTheBound),
                    "a swing above its bound did not behave like the bound");

        atTheBound.swing = Quantise::kMinSwing;
        beyond.swing = Quantise::kMinSwing - 0.01 * (double) (1 + i % 40);
        checkExact (Quantise::place (played, beyond), Quantise::place (played, atTheBound),
                    "a swing below its bound did not behave like the bound");

        // The window is measured on a swung grid on purpose. On a straight one
        // the nearest point is never more than half a step away, so a window
        // already a whole step wide covers every note and widening it further
        // cannot be seen at all: the measurement would have no power and would
        // report a clamp that is not there. Swung, the gap from a downbeat to
        // the offbeat after it is one and a half steps, and the notes sitting
        // in the middle of it are the ones a wider window reaches.
        atTheBound.swing = beyond.swing = Quantise::kMaxSwing;

        for (auto gate : { Quantise::Gate::include, Quantise::Gate::exclude })
        {
            atTheBound.gate = beyond.gate = gate;

            atTheBound.range = 1.0;
            beyond.range = 1.0 + 0.2 * (double) (1 + i % 20);
            checkExact (Quantise::place (played, beyond), Quantise::place (played, atTheBound),
                        "a window wider than the step did not behave like the whole step");

            atTheBound.range = 0.0;
            beyond.range = -0.1 * (double) (1 + i % 20);
            checkExact (Quantise::place (played, beyond), Quantise::place (played, atTheBound),
                        "a window narrower than nothing did not behave like no window at all");
        }
    }

    // And a setting that is not a number at all, which has to land on the
    // plainest value of the three rather than on arithmetic with a NaN in it.
    const double notANumber = std::numeric_limits<double>::quiet_NaN();

    auto nonsense = straight (0.25);
    nonsense.strength = notANumber;
    checkExact (Quantise::place (1.37, nonsense), 1.37,
                "a strength that is not a number moved a note");

    nonsense = straight (0.25);
    nonsense.swing = notANumber;
    checkExact (Quantise::place (1.37, nonsense), Quantise::place (1.37, straight (0.25)),
                "a swing that is not a number did not fall back to straight");

    nonsense = straight (0.25);
    nonsense.gate = Quantise::Gate::include;
    nonsense.range = notANumber;
    auto noWindow = nonsense;
    noWindow.range = 0.0;
    checkExact (Quantise::place (1.37, nonsense), Quantise::place (1.37, noWindow),
                "a window that is not a number did not fall back to no window at all");
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

static void theGridIsWhereTheDefinitionPutsIt()
{
    // The grid's own two claims, checked directly rather than through where a
    // note ends up: every second point is an offbeat that moves with the
    // swing, and the others "never move, for any swing value".
    //
    // Checked on both sides of the origin because the index arithmetic for a
    // negative pair is written differently from the positive one, and nothing
    // else in this file looks at it: a grid whose negative half is a pair out
    // of step is a clip whose notes move the wrong way as soon as it is
    // dragged left of where it was recorded.
    for (double step : { 1.0, 0.5, 0.25, 1.0 / 3.0, 0.125 })
    {
        for (int percent = 25; percent <= 75; ++percent)
        {
            const double swing = percent / 100.0;

            for (long long pair = -32; pair <= 32; ++pair)
            {
                const double downbeat = (double) (pair * 2) * step;

                checkExact (Quantise::gridPoint (pair * 2, step, swing), downbeat,
                            "a downbeat of the grid moved with the swing");
                checkExact (Quantise::gridPoint (pair * 2 + 1, step, swing),
                            downbeat + swing * step * 2.0,
                            "an offbeat of the grid is not at the swing fraction of its pair");
            }

            // And they come out in order. Monotonic is what makes "the nearest
            // point" a single answer, and it is the property a swing taken
            // past its bound would break.
            for (long long index = -64; index < 64; ++index)
                check (Quantise::gridPoint (index, step, swing)
                         < Quantise::gridPoint (index + 1, step, swing),
                       "the grid's points did not come out in increasing order");
        }
    }
}

static void theNearestPointIsTheNearestOneIncludingTies()
{
    // Checked against a scan of the grid rather than against the header's six
    // candidates, so a search that cannot see far enough, or an index that
    // counts pairs wrongly on one side of the origin, has nowhere to hide.
    //
    // The tie is the reason this is randomised over dyadic grids and swings:
    // with the offbeat at an exact fraction of its pair, the midpoint between
    // two grid points is exactly representable, so the tie is a real tie and
    // not a near miss. A note exactly between two lines is the common case
    // rather than a corner: quantising eighths to a quarter grid puts every
    // offbeat there, and a tie broken the other way moves half of a part
    // forward instead of back.
    std::mt19937 rng (6502);
    std::uniform_real_distribution<double> jitter (-3.0, 3.0);
    std::uniform_int_distribution<int> pairPick (-12, 12);

    const double steps[] = { 1.0, 0.5, 0.25, 0.125 };
    const double swings[] = { 0.25, 0.375, 0.5, 0.625, 0.75 };

    for (int i = 0; i < 20000; ++i)
    {
        const double step = steps[i % 4];
        const double swing = swings[(i / 4) % 5];
        const double pair = step * 2.0;
        const double downbeat = (double) (pairPick (rng) * 2) * step;
        const double offbeat = downbeat + swing * pair;

        auto o = straight (step);
        o.swing = swing;

        // The two midpoints inside a pair, which are the ties, and one beat
        // pulled off at random, which is not.
        const double beats[] = { (downbeat + offbeat) * 0.5,
                                 (offbeat + downbeat + pair) * 0.5,
                                 downbeat + jitter (rng) * step };

        for (double beat : beats)
            checkExact (Quantise::place (beat, o), nearestByDefinition (beat, step, swing),
                        "a note did not go to the nearest grid point, or a tie went to the later one");
    }

    // The awkward grids as well, where nothing is exact and the gaps either
    // side of an offbeat are unequal: no ties to speak of, but the search
    // still has to find the nearest point of an irregular grid.
    std::uniform_real_distribution<double> anywhere (-20.0, 60.0);
    const double odd[] = { 1.0 / 3.0, 1.0 / 6.0, 1.0 / 12.0, 1.0 / 7.0 };

    for (int i = 0; i < 20000; ++i)
    {
        const double step = odd[i % 4];
        const double swing = 0.25 + (double) (i % 51) * 0.01;
        const double beat = anywhere (rng);

        auto o = straight (step);
        o.swing = swing;

        checkExact (Quantise::place (beat, o), nearestByDefinition (beat, step, swing),
                    "a note did not go to the nearest point of an unevenly spaced grid");
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

static void theWindowsOwnEdgeIsInsideIt()
{
    // Which side of the line the edge itself falls on would be a measure zero
    // question for a performance and is not one for a programmed part, where
    // notes sit on exact divisions: against a quarter note grid with Avid's
    // 50 per cent window, every eighth in the pattern is exactly on the edge.
    // Avid's wording is Include *Within*, and the header's own note that at a
    // full window "nothing is outside it" only holds if the edge is in.
    auto include = straight (1.0);
    include.gate = Quantise::Gate::include;
    include.range = 0.5;

    auto exclude = include;
    exclude.gate = Quantise::Gate::exclude;

    for (long long beatNumber = 0; beatNumber < 8; ++beatNumber)
    {
        const double onTheEdge = (double) beatNumber + 0.25;   // exactly the tolerance

        checkExact (Quantise::place (onTheEdge, include), (double) beatNumber,
                    "Include Within left a note sitting exactly on the edge of its window");
        checkExact (Quantise::place (onTheEdge, exclude), onTheEdge,
                    "Exclude Within quantised a note sitting exactly on the edge of its safe zone");
    }

    // And the whole step case the header relies on: a full window reaches
    // exactly halfway to the next point, so there is nowhere left outside it.
    // A note on an exact eighth against a sixteenth grid is this case.
    auto full = straight (0.25);
    full.gate = Quantise::Gate::include;
    full.range = 1.0;

    checkExact (Quantise::place (0.125, full), 0.0,
                "a full window did not reach halfway to the next grid point");
    checkExact (Quantise::place (1.375, full), 1.25,
                "a full window did not reach halfway to the next grid point");
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

static void aDrawnOverlapIsLeftAlone()
{
    // "Quantise cleans up after itself and reaches for nothing else." A piano
    // roll will draw two notes of one pitch on top of each other, and a pass
    // that resolves those while moving something else entirely has deleted
    // part of an arrangement nobody asked it to touch. The overlap here is
    // already exactly on the grid, so nothing moves it, and one note of
    // another pitch is off the grid so that the pass does have work to do.
    std::mt19937 rng (1848);
    std::uniform_int_distribution<int> where (0, 40);
    std::uniform_int_distribution<int> extra (1, 8);

    for (int round = 0; round < 2000; ++round)
    {
        const double grid = 0.25;

        const double first = (double) where (rng) * grid;
        const double second = first + (double) extra (rng) * grid;
        const double firstLength = (second - first) + (double) extra (rng) * grid;
        const double secondLength = 2.0 * grid;

        std::vector<TimebaseNote> notes { note (first, firstLength, 60),
                                          note (second, secondLength, 60),
                                          note (first + grid * 0.37, grid, 62) };

        auto o = straight (grid);
        o.target = Quantise::Target::starts;
        o.preserveLength = true;

        const auto result = Quantise::apply (notes, o);

        check (result.moved == 1, "a note that was already on the grid was moved");
        check (result.trimmed == 0, "quantise shortened an overlap it had not moved");
        check (result.dropped == 0, "quantise deleted an overlap it had not moved");
        check (notes.size() == 3, "quantise changed the note count of an overlap it had not moved");

        for (const auto& n : notes)
            if (n.note == 60)
                checkExact (n.length, n.start == first ? firstLength : secondLength,
                            "a drawn overlap was shortened by a pass that never moved it");
    }
}

static void anOverlapCostsTheEarlierNoteItsTail()
{
    // The rule `MidiTimebase.h` established and this follows: the earlier note
    // is shortened to where the later one starts. Dropping the later note
    // instead leaves a pattern a file can hold just as well, and loses
    // something that was played, so the two outcomes are told apart here
    // rather than both being accepted as "resolved".
    std::mt19937 rng (1911);
    std::uniform_real_distribution<double> nudge (-0.3, 0.3);
    std::uniform_int_distribution<int> span (1, 4);
    std::uniform_int_distribution<int> start (0, 20);

    for (int round = 0; round < 3000; ++round)
    {
        const double grid = (round % 3 == 0) ? 1.0 : (round % 3 == 1) ? 0.5 : 0.25;

        const double firstPoint = (double) start (rng) * grid;
        const double secondPoint = firstPoint + (double) span (rng) * grid;

        // Played near enough to each point that it is unambiguously the
        // nearest, and the first long enough to run past the second.
        const double secondLength = grid * 0.5;
        std::vector<TimebaseNote> notes
        {
            note (firstPoint + nudge (rng) * grid, (secondPoint - firstPoint) + grid * 0.5, 60),
            note (secondPoint + nudge (rng) * grid, secondLength, 60)
        };

        auto o = straight (grid);
        o.preserveLength = true;

        const auto result = Quantise::apply (notes, o);

        check (notes.size() == 2, "an overlap cost a note that could have been shortened instead");
        check (result.dropped == 0, "an overlap dropped a note instead of shortening one");
        check (result.trimmed == 1, "the note that overran the next one was not reported as shortened");

        if (notes.size() == 2)
        {
            checkExact (notes[0].start, firstPoint, "the earlier note did not land on its grid point");
            checkExact (notes[1].start, secondPoint, "the later note did not land on its grid point");
            checkExact (notes[0].start + notes[0].length, notes[1].start,
                        "the earlier note was not shortened to exactly where the later one starts");
            checkExact (notes[1].length, secondLength,
                        "the later note paid for the overlap instead of the earlier one");
        }
    }
}

static void ofTwoNotesOnOnePointTheAudibleOneSurvives()
{
    // The one kept is the one that was audible, which is also what
    // `midiResolveSamePitchOverlaps` does at export. The two have to agree:
    // if quantise kept one of them and the file kept the other, a bounce
    // would not be the pattern that was played back.
    std::mt19937 rng (1963);
    std::uniform_real_distribution<double> nudge (-0.3, 0.3);
    std::uniform_real_distribution<double> spare (0.0, 2.0);
    std::uniform_int_distribution<int> start (0, 20);

    for (int round = 0; round < 3000; ++round)
    {
        const double grid = (round % 2 == 0) ? 0.5 : 0.25;
        const double point = (double) start (rng) * grid;

        const double longer = 1.0 + spare (rng);
        const double shorter = 0.01 + spare (rng) * 0.1;

        std::vector<TimebaseNote> notes { note (point + nudge (rng) * grid, longer, 60),
                                          note (point + nudge (rng) * grid, shorter, 60) };

        auto o = straight (grid);
        o.preserveLength = true;

        const auto result = Quantise::apply (notes, o);

        check (notes.size() == 1, "two notes quantised onto one point did not become one note");
        check (result.dropped == 1, "the note lost to a collision was not reported as lost");

        if (notes.size() == 1)
        {
            checkExact (notes[0].start, point, "the surviving note is not on the grid point");
            checkExact (notes[0].length, longer,
                        "the shorter of two notes beginning together was kept, so the audible one was thrown away");
        }
    }
}

static void twoOnsetsInsideOneTickBecomeOneNote()
{
    // A double triggered drum, or a flam, is two onsets a few microseconds
    // apart, and one MIDI tick apart is as close as a file can describe. The
    // header resolves the pair by dropping one, and the reason is the
    // alternative: trimming the earlier note to the later one leaves it
    // shorter than a tick, which is a note on and a note off at the same tick
    // and so a note nothing plays. Ends are quantised here so that the two
    // onsets stay the sub tick distance apart that they were played.
    std::mt19937 rng (2048);
    std::uniform_real_distribution<double> fraction (0.01, 0.98);
    std::uniform_real_distribution<double> position (0.0, 24.0);

    const double tick = Quantise::kMinLengthBeats;

    for (int round = 0; round < 3000; ++round)
    {
        const double first = position (rng);
        const double gap = fraction (rng) * tick;

        std::vector<TimebaseNote> notes { note (first, 0.5, 60),
                                          note (first + gap, 0.5, 60) };

        auto o = straight (0.25);
        o.target = Quantise::Target::ends;

        const auto result = Quantise::apply (notes, o);

        check (notes.size() == 1, "two onsets inside one tick did not become one note");
        check (result.dropped == 1, "the onset lost inside a tick was not reported as lost");

        for (const auto& n : notes)
            check (n.length >= tick,
                   "a trim left a note shorter than a tick, which is a note nothing plays");

        int droppedByExport = 0;
        midiNotesToTicks (notes, kMidiExportPpq, &droppedByExport);
        check (droppedByExport == 0,
               "the export still had to throw away one of two onsets inside a tick");
    }
}

static void notesThatOnlyTouchAreNotReportedAsTrimmed()
{
    // A legato line quantises to notes whose ends land exactly on the next
    // note's start, which is the ordinary case rather than an overlap. The
    // counts are all the caller has to tell somebody what the pass did, so a
    // tidy up that is reported and did not happen is the same class of fault
    // as one that happened and was not reported.
    std::mt19937 rng (2112);
    std::uniform_real_distribution<double> nudge (-0.3, 0.3);
    std::uniform_int_distribution<int> start (0, 20);
    std::uniform_int_distribution<int> span (1, 4);

    for (int round = 0; round < 2000; ++round)
    {
        const double grid = (round % 2 == 0) ? 0.5 : 0.25;
        const double firstPoint = (double) start (rng) * grid;
        const double gap = (double) span (rng) * grid;

        // Exactly long enough to reach the second note's point, and no longer.
        std::vector<TimebaseNote> notes { note (firstPoint + nudge (rng) * grid, gap, 60),
                                          note (firstPoint + gap + nudge (rng) * grid, grid, 60) };

        auto o = straight (grid);
        o.preserveLength = true;

        const auto result = Quantise::apply (notes, o);

        check (result.trimmed == 0, "two notes that only touch were reported as trimmed");
        check (result.dropped == 0, "two notes that only touch cost one of themselves");
        check (notes.size() == 2, "two notes that only touch did not both survive");

        if (notes.size() == 2)
            checkExact (notes[0].length, gap,
                        "a note that only reached the next note's start was shortened anyway");
    }
}

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
    std::uniform_int_distribution<int> oddity (0, 24);

    const double grids[] = { 1.0, 0.5, 0.25, 1.0 / 3.0, 0.125, 1.0 / 12.0, 0.0, -1.0 };
    const double infinity = std::numeric_limits<double>::infinity();
    const double notANumber = std::numeric_limits<double>::quiet_NaN();

    for (int round = 0; round < 6000; ++round)
    {
        std::vector<TimebaseNote> notes;
        const int count = howMany (rng);
        for (int i = 0; i < count; ++i)
        {
            auto n = note (position (rng), length (rng), pitch (rng));

            // A position that is not a number is a fault somewhere else, and
            // the one thing it must not be allowed to become is a comparison
            // that is not an ordering: the overlap pass sorts by start and by
            // length, and a sort whose comparator lies about its own operands
            // is free to walk off the end of the vector. One note in five
            // carries something a sort cannot order.
            switch (oddity (rng))
            {
                case 0:  n.start  = notANumber; break;
                case 1:  n.length = notANumber; break;
                case 2:  n.start  = infinity; break;
                case 3:  n.length = -infinity; break;
                case 4:  n.length = 0.0; break;
                default: break;
            }

            notes.push_back (n);
        }

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
    aSettingOutOfRangeBehavesLikeItsBound();

    straightSwingIsNoSwing();
    theOffbeatLandsWhereTheNumberSays();
    belowHalfPullsTheOffbeatEarly();
    downbeatsNeverMoveAtAnySwing();
    theGridIsWhereTheDefinitionPutsIt();
    theNearestPointIsTheNearestOneIncludingTies();
    quantiseAndGrooveAgreeAboutSwing();
    theAvidSwingScaleConverts();

    avidsWorkedExamplesComeOutRight();
    theTwoSensesAreComplementary();
    aFullWindowIsNoGateOnAStraightGrid();
    theWindowsOwnEdgeIsInsideIt();
    aNoteOnTheGridIsLeftAloneByEveryGate();

    startsOnlyKeepsEveryLength();
    endsOnlyLeavesEveryStart();
    aFixedEndMeansTheLengthChanges();
    preserveLengthIsRefusedWhenBothEndsMove();
    bothEndsOnTheGridStillLeavesANoteToPlay();

    aDrawnOverlapIsLeftAlone();
    anOverlapCostsTheEarlierNoteItsTail();
    ofTwoNotesOnOnePointTheAudibleOneSurvives();
    twoOnsetsInsideOneTickBecomeOneNote();
    notesThatOnlyTouchAreNotReportedAsTrimmed();

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
