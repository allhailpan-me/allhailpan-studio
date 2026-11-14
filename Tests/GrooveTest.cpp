// ---------------------------------------------------------------------------
// Checks groove: where a swung note actually lands, and what the feel
// controls do to it.
//
// Swing is a number other people already own. A drummer, an MPC, and every
// other studio that reports a swing percentage all mean the same thing by 58:
// the offbeat sits 58 percent of the way through its pair. So the useful
// check is not that this program is self consistent, it is that the positions
// agree with that definition, worked out independently here from the
// definition rather than from the code under test. A swing number that is
// almost right is the worst case: the pattern still plays, it just does not
// feel like what it says on the box, and it stops matching a pattern brought
// in from somewhere else.
//
// The things that fail quietly, and are therefore checked exhaustively:
//
//   - The downbeats must not move. A groove that nudges the beats as well as
//     the offbeats is not swing, it is a timing error, and against a drum
//     loop it sounds like the whole pattern is late.
//   - The order of the notes must survive. If an offbeat can be pushed past
//     the downbeat after it, a pattern reorders itself, and with humanise on
//     that would happen on some patterns and not others.
//   - Randomisation must be a function of position, not of when it was asked.
//     Grooves are applied when the arrangement is handed to the engine, and
//     the export does that again, so a groove drawn from a live random source
//     would make a bounce differ from what was heard. That is the recurring
//     hazard in this program, and here it would be inaudible until somebody
//     compared the two files.
//   - Off grid notes must be left alone. A part played in by hand is already
//     where the performance put it.
//
// Groove.h has no JUCE in it, so this builds and runs on its own with
// sanitizers in a couple of seconds. CI runs it on every push.
//
//   ./Tests/run.sh
//
// Sources:
//   The swing percentage convention hardware samplers established, where the
//   number is the offbeat's position within its pair and 66.67 puts it on the
//   third triplet. Checked here against positions computed from that
//   definition, not from Groove.h.
//   Ableton Live's manual, on grooves carrying a base resolution, a timing
//   amount, a velocity amount and a randomise amount rather than one knob.
// ---------------------------------------------------------------------------

#include "Groove.h"

#include <algorithm>
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
                    + ", off by " + std::to_string (std::abs (a - b)) + ")");
}

static const char* baseName (Groove::Base b)
{
    switch (b)
    {
        case Groove::Base::eighth:       return "1/8";
        case Groove::Base::sixteenth:    return "1/16";
        case Groove::Base::thirtySecond: return "1/32";
    }
    return "?";
}

// The three resolutions, with the step length the groove is measured against
// worked out here rather than read back from the thing being tested.
struct Resolution { Groove::Base base; double step; };
static const Resolution resolutions[] =
{
    { Groove::Base::eighth,       0.5   },
    { Groove::Base::sixteenth,    0.25  },
    { Groove::Base::thirtySecond, 0.125 },
};

static Groove swungAt (Groove::Base base, double swing)
{
    Groove g;
    g.enabled = true;
    g.base    = base;
    g.swing   = swing;
    g.amount  = 1.0;
    return g;
}

// ---------------------------------------------------------------------------

static void straightIsStraight()
{
    std::printf ("fifty percent leaves everything exactly where it was\n");

    for (const auto& r : resolutions)
    {
        auto g = swungAt (r.base, 0.5);

        for (int step = 0; step < 64; ++step)
        {
            const double at = step * r.step;
            checkClose (g.place (at, 60), at, 1.0e-12,
                        std::string ("straight swing moved step ") + std::to_string (step)
                            + " at " + baseName (r.base));
        }
    }

    // And with the groove switched off entirely, including positions that are
    // nowhere near the grid.
    Groove off;
    off.swing = 2.0 / 3.0;
    off.random = 1.0;
    off.velocity = 1.0;
    for (double at = 0.0; at < 8.0; at += 0.03125)
        checkClose (off.place (at, 60), at, 1.0e-12, "a disabled groove moved a note");
}

static void theOffbeatLandsWhereTheNumberSays()
{
    std::printf ("the offbeat sits at the swing fraction of its pair\n");

    // Worked out from the definition: the pair spans two steps, the offbeat
    // sits `swing` of the way through it, so step 2k+1 lands at
    // (k + swing) * pair rather than (k + 0.5) * pair.
    const double swings[] { 0.52, 0.54, 0.58, 0.62, 2.0 / 3.0, 0.75 };

    for (const auto& r : resolutions)
        for (double swing : swings)
        {
            auto g = swungAt (r.base, swing);
            const double pair = r.step * 2.0;

            for (int k = 0; k < 16; ++k)
            {
                const double downbeat = k * pair;
                const double offbeat  = downbeat + r.step;

                checkClose (g.place (downbeat, 60), downbeat, 1.0e-12,
                            std::string ("swing ") + std::to_string (swing) + " at "
                                + baseName (r.base) + " moved downbeat " + std::to_string (k));

                checkClose (g.place (offbeat, 60), downbeat + swing * pair, 1.0e-9,
                            std::string ("swing ") + std::to_string (swing) + " at "
                                + baseName (r.base) + " put offbeat " + std::to_string (k)
                                + " in the wrong place");
            }
        }

    // The headline case, spelled out: a full shuffle puts the second
    // sixteenth on the last third of the eighth note it shares. At 1/16 the
    // pair is half a beat, so that is a sixth of a beat late.
    auto shuffle = swungAt (Groove::Base::sixteenth, 2.0 / 3.0);
    checkClose (shuffle.place (0.25, 60), 1.0 / 3.0, 1.0e-9,
                "a full shuffle did not land the offbeat on the third triplet");
    std::printf ("    1/16 at 66.67: offbeat 0.250 plays at %.6f beats, a triplet is %.6f\n",
                 shuffle.place (0.25, 60), 1.0 / 3.0);
}

static void anotherResolutionIsLeftAlone()
{
    std::printf ("a groove at one resolution leaves the coarser notes straight\n");

    // A 1/16 groove swings sixteenths. The eighths and quarters inside the
    // same bar are even numbered steps, so they must not move: this is what
    // the base resolution is for, and getting it wrong would swing a bassline
    // that was meant to stay flat under a shuffled hat.
    auto g = swungAt (Groove::Base::sixteenth, 2.0 / 3.0);

    for (int eighth = 0; eighth < 16; ++eighth)
    {
        const double at = eighth * 0.5;
        checkClose (g.place (at, 60), at, 1.0e-12,
                    "a 1/16 groove moved the eighth note at " + std::to_string (at));
    }
}

static void amountScalesTheShift()
{
    std::printf ("the amount scales the shift, and zero is straight\n");

    const double pair = 0.5;
    const double swing = 0.62;
    const double full = swing * pair - 0.25;

    for (double amount = 0.0; amount <= 1.0001; amount += 0.1)
    {
        auto g = swungAt (Groove::Base::sixteenth, swing);
        g.amount = amount;
        checkClose (g.place (0.25, 60), 0.25 + full * amount, 1.0e-9,
                    "amount " + std::to_string (amount) + " did not scale the shift");
    }
}

static void offGridNotesAreNotTouched()
{
    std::printf ("a note played off the grid stays where the performance put it\n");

    auto g = swungAt (Groove::Base::sixteenth, 2.0 / 3.0);

    // Far enough off the grid to be a played note rather than a rounding
    // error. The tolerance in Groove.h is two percent of a step.
    const double offsets[] { 0.03, 0.06, 0.1, -0.05, -0.09 };

    for (int step = 1; step < 32; ++step)
        for (double off : offsets)
        {
            const double at = step * 0.25 + off * 0.25;
            if (at <= 0.0)
                continue;
            checkClose (g.place (at, 60), at, 1.0e-12,
                        "an off grid note at " + std::to_string (at) + " was moved");
        }
}

static void theOrderOfTheNotesSurvives()
{
    std::printf ("nothing can be pushed past the note after it\n");

    // Every combination of resolution, swing and humanise, over a long run of
    // steps and every note number that changes the random draw. A reordering
    // here would make a pattern play its own notes in the wrong order, and
    // only on some patterns, which is close to undiagnosable from a bug
    // report.
    std::mt19937 rng (20261004);
    std::uniform_int_distribution<int> noteDist (0, 127);

    int cases = 0;

    for (const auto& r : resolutions)
        for (double swing : { 0.5, 0.54, 0.58, 0.62, 2.0 / 3.0, 0.75 })
            for (double random : { 0.0, 0.15, 0.4, 1.0 })
                for (double amount : { 0.5, 1.0 })
                {
                    auto g = swungAt (r.base, swing);
                    g.random = random;
                    g.amount = amount;

                    double previous = -1.0;
                    for (int step = 0; step < 128; ++step)
                    {
                        const int note = noteDist (rng);
                        const double placed = g.place (step * r.step, note);

                        check (placed >= 0.0, "a groove placed a note before the clip start");
                        if (! (placed > previous || (step == 0 && placed >= 0.0)))
                            fail (std::string ("step ") + std::to_string (step) + " at "
                                  + baseName (r.base) + ", swing " + std::to_string (swing)
                                  + ", random " + std::to_string (random)
                                  + " landed at " + std::to_string (placed)
                                  + " which is not after " + std::to_string (previous));
                        previous = placed;
                        ++cases;
                    }
                }

    std::printf ("    %d placements, all still in order\n", cases);
}

static void randomisationIsTheSameEveryTime()
{
    std::printf ("humanising is a function of position, so a bounce matches what was heard\n");

    auto g = swungAt (Groove::Base::sixteenth, 0.58);
    g.random = 0.4;

    // Same groove, same note, asked a hundred times: the arrangement is
    // rebuilt on every edit and again for the export, so anything that
    // changed between calls would make the exported file differ from the one
    // that was played.
    for (int step = 0; step < 32; ++step)
        for (int note : { 36, 38, 42, 60 })
        {
            const double first = g.place (step * 0.25, note);
            for (int again = 0; again < 100; ++again)
                checkClose (g.place (step * 0.25, note), first, 0.0,
                            "humanising gave a different answer the second time");
        }

    // A fresh Groove with the same settings has to agree too, since the
    // export builds its own arrangement rather than reusing one.
    auto copy = swungAt (Groove::Base::sixteenth, 0.58);
    copy.random = 0.4;
    for (int step = 0; step < 32; ++step)
        checkClose (copy.place (step * 0.25, 38), g.place (step * 0.25, 38), 0.0,
                    "two identical grooves humanised differently");

    // And it has to actually do something, or the check above is vacuous.
    bool moved = false;
    for (int step = 0; step < 32; ++step)
        if (std::abs (g.place (step * 0.25, 38) - swungAt (Groove::Base::sixteenth, 0.58)
                                                     .place (step * 0.25, 38)) > 1.0e-9)
            moved = true;
    check (moved, "humanising at 0.4 moved nothing at all");
}

static void humanisingStaysInsideItsStep()
{
    std::printf ("humanising stays within a quarter step, so it cannot swallow a note\n");

    for (const auto& r : resolutions)
    {
        auto g = swungAt (r.base, 0.5);
        g.random = 1.0;

        double worst = 0.0;
        for (int step = 1; step < 256; ++step)
            for (int note = 0; note < 128; ++note)
                worst = std::max (worst, std::abs (g.place (step * r.step, note) - step * r.step));

        check (worst <= r.step * 0.25 + 1.0e-12,
               std::string ("humanising at ") + baseName (r.base) + " moved a note by "
                   + std::to_string (worst) + " beats, more than a quarter step");
        std::printf ("    %-4s  worst drift %.6f beats of a %.6f step\n",
                     baseName (r.base), worst, r.step);
    }
}

static void velocityOnlySoftensOffbeats()
{
    std::printf ("the velocity amount softens offbeats and leaves downbeats alone\n");

    Groove g;
    g.enabled  = true;
    g.base     = Groove::Base::sixteenth;
    g.velocity = 0.5;

    for (int step = 0; step < 32; ++step)
    {
        const double at = step * 0.25;
        const float shaded = g.shade (at, 0.8f);

        if ((step & 1) == 0)
            checkClose (shaded, 0.8f, 1.0e-6, "a downbeat was softened");
        else
            check (shaded < 0.8f, "an offbeat was not softened");
    }

    // Nothing may be pushed to silence or above full: a note that was audible
    // has to stay audible, and a velocity over one would be clamped
    // somewhere further downstream where it is harder to see.
    Groove hard = g;
    hard.velocity = 1.0;
    for (float v : { 0.001f, 0.01f, 0.5f, 1.0f })
        for (int step = 1; step < 8; step += 2)
        {
            const float shaded = hard.shade (step * 0.25, v);
            check (shaded >= 0.02f && shaded <= 1.0f,
                   "shading produced " + std::to_string (shaded) + " from " + std::to_string (v));
        }

    // With the amount at zero, or the groove off, velocities pass straight
    // through: switching the feel off has to give back exactly what was
    // played, not something close to it.
    Groove none = g;
    none.velocity = 0.0;
    Groove disabled = g;
    disabled.enabled = false;
    for (float v : { 0.0f, 0.25f, 0.5f, 1.0f })
        for (int step = 0; step < 8; ++step)
        {
            checkClose (none.shade (step * 0.25, v), v, 0.0, "a zero velocity amount changed a note");
            checkClose (disabled.shade (step * 0.25, v), v, 0.0, "a disabled groove changed a velocity");
        }
}

static void theStepLengthsAreTheNoteValues()
{
    std::printf ("each resolution measures itself against the right note value\n");

    for (const auto& r : resolutions)
        checkClose (Groove { r.base }.stepBeats(), r.step, 0.0,
                    std::string ("the step at ") + baseName (r.base) + " is not the note value");
}


// ---------------------------------------------------------------------------
// The humanising draw.
//
// Everything below exists because the first version of this file asked only
// two questions of it: does it give the same answer twice, and does it stay
// inside a quarter step. Both are necessary and neither says what the draw
// is. A draw that was always positive, or half the size it claims, or the
// same number for every step, or a steady ramp, passed both. So the four
// things that make it humanising rather than a shift are measured here, and
// each measurement is pointed at a generator known to fail it first.
//
//   - It goes both ways. A draw that only ever pushes late is a delay with a
//     random length, and on a loop it is heard as the pattern dragging.
//   - It fills the range the knob promises. At the top of the knob the drift
//     reaches a quarter step; at half that, the knob does half what it says
//     and no reading of the output can tell.
//   - It moves from step to step and from note to note. One draw for the
//     whole part is a shift, and one draw per step shared by every note on
//     it cannot loosen a hat against a kick.
//   - It is a scatter rather than a pattern. Successive steps have to be
//     uncorrelated: a draw that advances by a fixed amount each step is
//     perfectly spread and perfectly predictable, and what it plays is a
//     slow sweep rather than a player's unsteadiness.
// ---------------------------------------------------------------------------

struct DrawStats
{
    double lowest   = 0.0;   // in fractions of a step
    double highest  = 0.0;
    double mean     = 0.0;
    double lag1     = 0.0;   // correlation between a step's draw and the next
    int    distinct = 0;
};

static DrawStats summarise (const std::vector<double>& draws)
{
    DrawStats s;
    s.lowest  = draws.front();
    s.highest = draws.front();

    double sum = 0.0;
    for (double d : draws)
    {
        s.lowest  = std::min (s.lowest, d);
        s.highest = std::max (s.highest, d);
        sum += d;
    }
    s.mean = sum / (double) draws.size();

    double var = 0.0, cov = 0.0;
    for (std::size_t i = 0; i < draws.size(); ++i)
    {
        const double a = draws[i] - s.mean;
        var += a * a;
        if (i + 1 < draws.size())
            cov += a * (draws[i + 1] - s.mean);
    }
    s.lag1 = var > 0.0 ? cov / var : 1.0;

    auto sorted = draws;
    std::sort (sorted.begin(), sorted.end());
    sorted.erase (std::unique (sorted.begin(), sorted.end()), sorted.end());
    s.distinct = (int) sorted.size();

    return s;
}

// The four questions, as one answer, so the same yardstick can be turned on
// a generator that is meant to fail it. `bound` is the drift the knob
// promises at the top of its travel, in fractions of a step.
static bool looksLikeHumanising (const DrawStats& s, double bound)
{
    return s.lowest  <= -0.95 * bound
        && s.highest >=  0.95 * bound
        && std::abs (s.mean) <= 0.03 * bound
        && std::abs (s.lag1) <= 0.12
        && s.distinct > 200;
}

static void theHumanisingDrawIsADrawAndNotAShift()
{
    std::printf ("the humanising draw goes both ways, fills its range and is not a pattern\n");

    // The yardstick has to be able to fail, or the passes below mean nothing.
    // Four generators, each wrong in one of the four ways, measured over the
    // same number of samples.
    const int n = 1024;
    std::vector<double> unipolar, halfRange, biased, ramp;
    {
        std::mt19937 rng (20261114);
        std::uniform_real_distribution<double> uniform (-1.0, 1.0);
        double weyl = 0.0;

        for (int i = 0; i < n; ++i)
        {
            const double u = uniform (rng);
            unipolar .push_back (std::abs (u) * 0.25);
            halfRange.push_back (u * 0.125);
            biased   .push_back (u * 0.25 * 0.5 + 0.125);
            weyl = weyl + 0.6180339887498949;
            weyl -= std::floor (weyl);
            ramp     .push_back ((weyl * 2.0 - 1.0) * 0.25);
        }
    }

    check (! looksLikeHumanising (summarise (unipolar),  0.25), "a draw that never goes early was accepted");
    check (! looksLikeHumanising (summarise (halfRange), 0.25), "a draw half the size it claims was accepted");
    check (! looksLikeHumanising (summarise (biased),    0.25), "a draw biased late was accepted");
    check (! looksLikeHumanising (summarise (ramp),      0.25), "a draw that is a steady ramp was accepted");

    // And now the real one, at every resolution, with the knob at the top and
    // the swing straight so that nothing but the draw is moving anything.
    for (const auto& r : resolutions)
    {
        auto g = swungAt (r.base, 0.5);
        g.random = 1.0;

        std::vector<double> draws;
        for (int step = 1; step < 1024; ++step)
            draws.push_back ((g.place (step * r.step, 60) - step * r.step) / r.step);

        const auto s = summarise (draws);
        std::printf ("    %-4s  low %+.4f  high %+.4f  mean %+.5f  lag1 %+.4f  %d distinct\n",
                     baseName (r.base), s.lowest, s.highest, s.mean, s.lag1, s.distinct);

        check (looksLikeHumanising (s, 0.25),
               std::string ("the humanising draw at ") + baseName (r.base)
                   + " is not a draw: low " + std::to_string (s.lowest)
                   + ", high " + std::to_string (s.highest)
                   + ", mean " + std::to_string (s.mean)
                   + ", lag1 " + std::to_string (s.lag1)
                   + ", " + std::to_string (s.distinct) + " distinct");
    }

    // The knob is a fraction of that bound rather than a switch, so halfway
    // up it has to reach halfway and no further. Checked as the extremes of
    // a long run rather than note by note, because a single note's draw is
    // whatever the hash gave it.
    for (double random : { 0.25, 0.5, 0.75 })
    {
        auto g = swungAt (Groove::Base::sixteenth, 0.5);
        g.random = random;

        double worst = 0.0;
        std::vector<double> draws;
        for (int step = 1; step < 1024; ++step)
        {
            const double d = (g.place (step * 0.25, 60) - step * 0.25) / 0.25;
            worst = std::max (worst, std::abs (d));
            draws.push_back (d);
        }

        const auto s = summarise (draws);
        check (worst <= 0.25 * random + 1.0e-12,
               "the humanise knob at " + std::to_string (random) + " drifted " + std::to_string (worst));
        check (s.lowest <= -0.95 * 0.25 * random && s.highest >= 0.95 * 0.25 * random,
               "the humanise knob at " + std::to_string (random) + " did not reach what it promises");
    }
}

static void everyNoteAndEveryStepGetsItsOwnDraw()
{
    std::printf ("the draw changes with the step and with the note\n");

    auto g = swungAt (Groove::Base::sixteenth, 0.5);
    g.random = 1.0;

    // One draw for the whole part would be a shift: the pattern would arrive
    // late or early as a whole and still be perfectly even inside itself,
    // which is the one thing humanising is not.
    int movedFromItsNeighbour = 0;
    for (int step = 1; step < 256; ++step)
    {
        const double here = g.place (step * 0.25, 60) - step * 0.25;
        const double next = g.place ((step + 1) * 0.25, 60) - (step + 1) * 0.25;
        if (std::abs (here - next) > 1.0e-9)
            ++movedFromItsNeighbour;
    }
    check (movedFromItsNeighbour > 250,
           "the draw is the same from one step to the next in "
               + std::to_string (255 - movedFromItsNeighbour) + " places");

    // And two notes landing on the same step have to be loosened against each
    // other, or a hat and a kick written on the same sixteenth stay welded
    // together however far the knob is turned up.
    int differsBetweenNotes = 0;
    for (int step = 1; step < 256; ++step)
        if (std::abs (g.place (step * 0.25, 36) - g.place (step * 0.25, 42)) > 1.0e-9)
            ++differsBetweenNotes;

    check (differsBetweenNotes > 250,
           "two notes on the same step were drawn the same way in "
               + std::to_string (255 - differsBetweenNotes) + " places");
    std::printf ("    %d of 255 steps differ from their neighbour, %d of 255 differ between two notes\n",
                 movedFromItsNeighbour, differsBetweenNotes);
}

static void aSwingUnderStraightPushesTheOffbeatEarly()
{
    std::printf ("a swing under fifty percent plays the offbeat early, and keeps the order\n");

    // `ProjectIO` admits a swing from 0.3 to 0.8, so a file can carry a push:
    // an offbeat ahead of its halfway point rather than behind it. Nothing in
    // this file swept below 0.5, so the whole early half of what a project
    // may contain was untested. Worked out from the definition the same way
    // as the late half: the offbeat sits `swing` of the way through its pair,
    // wherever that is.
    const double swings[] { 0.3, 0.35, 0.4, 0.45, 0.48 };

    for (const auto& r : resolutions)
        for (double swing : swings)
        {
            auto g = swungAt (r.base, swing);
            const double pair = r.step * 2.0;

            for (int k = 1; k < 16; ++k)
            {
                const double downbeat = k * pair;
                const double offbeat  = downbeat + r.step;

                checkClose (g.place (downbeat, 60), downbeat, 1.0e-12,
                            std::string ("a swing of ") + std::to_string (swing)
                                + " moved a downbeat");
                checkClose (g.place (offbeat, 60), downbeat + swing * pair, 1.0e-9,
                            std::string ("a swing of ") + std::to_string (swing)
                                + " put the offbeat in the wrong place");
                check (g.place (offbeat, 60) < offbeat,
                       std::string ("a swing of ") + std::to_string (swing)
                           + " did not play the offbeat early at all");
            }
        }

    // With the humanise knob at the top as well, since the early direction
    // spends the gap in front of the offbeat rather than behind it.
    std::mt19937 rng (20261114);
    std::uniform_int_distribution<int> noteDist (0, 127);

    for (const auto& r : resolutions)
        for (double swing : swings)
        {
            auto g = swungAt (r.base, swing);
            g.random = 1.0;

            double previous = -1.0;
            for (int step = 0; step < 128; ++step)
            {
                const double placed = g.place (step * r.step, noteDist (rng));
                if (! (placed > previous || step == 0))
                    fail (std::string ("a swing of ") + std::to_string (swing) + " at "
                          + baseName (r.base) + " put step " + std::to_string (step)
                          + " at " + std::to_string (placed)
                          + ", which is not after " + std::to_string (previous));
                previous = placed;
            }
        }
}

static void theOfferedSwingsAreWhatTheyClaim()
{
    std::printf ("the swings the interface offers are the numbers they are named after\n");

    // Straight first, so a caller can read "is this a swing at all" off the
    // index, and increasing, so the list reads as a strength.
    checkClose (Groove::offeredSwing (0), 0.5, 0.0, "the first offered swing is not straight");
    for (int i = 1; i < Groove::kNumOfferedSwings; ++i)
        check (Groove::offeredSwing (i) > Groove::offeredSwing (i - 1),
               "offered swing " + std::to_string (i) + " is not stronger than the one before it");

    // The strongest one is the shuffle, and a shuffle is the third triplet
    // rather than a number near it. Checked where it can be heard, by placing
    // a note through it: at 1/16 the offbeat has to land exactly a third of
    // the way through the beat. A rounded 0.66 in the table passes a round
    // trip through `nearestOfferedSwing` and lands 0.7 ms late at 120 bpm,
    // which is the kind of difference this check exists to refuse.
    auto shuffle = swungAt (Groove::Base::sixteenth, Groove::offeredSwing (Groove::kNumOfferedSwings - 1));
    checkClose (shuffle.place (0.25, 60), 1.0 / 3.0, 1.0e-12,
                "the strongest offered swing is not a triplet shuffle");

    // An index from outside the list is clamped rather than read past the end
    // of it. Every caller today arrives through `juce::jlimit` or through
    // `nearestOfferedSwing`, so this is a guard rather than a live path, and
    // it is asserted because a guard nothing exercises is one somebody tidies
    // away: without it these four lines are an out of bounds read, which the
    // sanitizers this suite builds with would stop on.
    checkClose (Groove::offeredSwing (-1), Groove::offeredSwing (0), 0.0,
                "a negative index did not give the first offered swing");
    checkClose (Groove::offeredSwing (Groove::kNumOfferedSwings),
                Groove::offeredSwing (Groove::kNumOfferedSwings - 1), 0.0,
                "an index past the end did not give the last offered swing");
    checkClose (Groove::offeredSwing (-1000000), Groove::offeredSwing (0), 0.0,
                "a wild negative index did not give the first offered swing");
    checkClose (Groove::offeredSwing (1000000),
                Groove::offeredSwing (Groove::kNumOfferedSwings - 1), 0.0,
                "a wild index did not give the last offered swing");

    // A swing exactly between two of them is shown as the straighter of the
    // two, which is what keeps the control from claiming more swing than the
    // file carries. Three of these ties are exact in doubles and so are
    // reachable rather than theoretical: halfway between straight and 0.54,
    // halfway between 0.58 and 0.62, and halfway between 0.62 and the
    // shuffle. Nothing checked the direction, so the comparison could be
    // relaxed to a non-strict one and a stored 0.52 would start reading as a
    // swung setting, which is also what decides whether the groove can be
    // switched off at all.
    //
    // Not every midpoint is a tie: 0.56 is nearer 0.58 than 0.54 by a part in
    // ten thousand million million, which is a fact about doubles rather than
    // a decision, so it is not pinned here.
    check (Groove::nearestOfferedSwing (0.52) == 0,
           "a swing halfway between straight and 0.54 was not shown as the straighter one");
    check (Groove::nearestOfferedSwing (0.6) == 2,
           "a swing halfway between 0.58 and 0.62 was not shown as the straighter one");
    check (Groove::nearestOfferedSwing ((0.62 + 2.0 / 3.0) * 0.5) == 3,
           "a swing halfway between 0.62 and the shuffle was not shown as the straighter one");
}

static void theVelocityAmountIsTheFractionItSays()
{
    std::printf ("the velocity amount takes the fraction off the offbeat that it says it does\n");

    // How much an offbeat is softened is this studio's own choice: no
    // hardware number and no other studio's manual fixes it, which is why it
    // is written down here rather than cited. At the top of the knob an
    // offbeat is played at 60 per cent of what was written, and the knob is a
    // straight fraction of that. Pinned because a constant in the middle of
    // one expression is the easiest kind of number to drift, and because
    // nothing could hear the difference between this and twice it except by
    // comparing two mixes.
    for (double amount = 0.0; amount <= 1.0001; amount += 0.05)
    {
        Groove g;
        g.enabled  = true;
        g.base     = Groove::Base::sixteenth;
        g.velocity = amount;

        for (float written : { 0.2f, 0.5f, 0.8f, 1.0f })
            for (int offbeat = 1; offbeat < 16; offbeat += 2)
                checkClose (g.shade (offbeat * 0.25, written),
                            std::max (0.02, (double) written * (1.0 - amount * 0.4)), 1.0e-6,
                            "the velocity amount " + std::to_string (amount)
                                + " did not take 40 per cent of it at the top of the knob");
    }

    Groove full;
    full.enabled  = true;
    full.base     = Groove::Base::sixteenth;
    full.velocity = 1.0;
    std::printf ("    at the top of the knob an offbeat written at 1.00 plays at %.3f\n",
                 (double) full.shade (0.25, 1.0f));

    // The ceiling is a guard rather than a live path: nothing upstream should
    // hand this a velocity above full scale, and softening cannot raise one
    // anyway. It is asserted because the floor beside it is, and because a
    // velocity over one would be clamped somewhere further downstream where
    // it is harder to see which stage did it.
    check (full.shade (0.25, 1.5f) <= 1.0f,
           "a velocity above full scale came back above full scale");
}

static void shadeLeavesAPlayedNoteAlone()
{
    std::printf ("a note played off the grid keeps the velocity it was played at\n");

    // `place` leaves an off grid note where the performance put it, and
    // `shade` has to leave its accent alone for exactly the same reason: a
    // part played by hand has its own dynamics, and softening every other
    // sixteenth of it would flatten them. Nothing checked this half, so the
    // window in `shade` could be deleted and the whole suite still passed.
    Groove g;
    g.enabled  = true;
    g.base     = Groove::Base::sixteenth;
    g.velocity = 1.0;

    const double offsets[] { 0.03, 0.06, 0.1, 0.25, -0.05, -0.09, -0.2 };

    for (int step = 1; step < 32; ++step)
        for (double off : offsets)
        {
            const double at = (step + off) * 0.25;
            if (at <= 0.0)
                continue;
            checkClose (g.shade (at, 0.8f), 0.8f, 0.0,
                        "an off grid note at " + std::to_string (at) + " was softened");
        }

    // The two halves have to draw the line in the same place, or a note is
    // moved and not softened, or softened and not moved. Checked at the edge
    // of the window from both sides rather than in the middle, which is
    // where a widened or deleted window shows.
    for (int offbeat = 1; offbeat < 32; offbeat += 2)
        for (double off : { -0.019, 0.0, 0.019 })
        {
            const double at = (offbeat + off) * 0.25;
            check (std::abs ((double) g.shade (at, 0.8f) - 0.8) > 1.0e-9,
                   "an offbeat just inside the window at " + std::to_string (at)
                       + " was not softened");
        }

    for (int offbeat = 1; offbeat < 32; offbeat += 2)
        for (double off : { -0.0201, 0.0201, 0.05 })
            checkClose (g.shade ((offbeat + off) * 0.25, 0.8f), 0.8f, 0.0,
                        "an offbeat just outside the window at "
                            + std::to_string ((offbeat + off) * 0.25) + " was softened");
}

int main()
{
    std::printf ("groove\n");

    theStepLengthsAreTheNoteValues();
    straightIsStraight();
    theOffbeatLandsWhereTheNumberSays();
    anotherResolutionIsLeftAlone();
    amountScalesTheShift();
    offGridNotesAreNotTouched();
    theOrderOfTheNotesSurvives();
    randomisationIsTheSameEveryTime();
    humanisingStaysInsideItsStep();
    theHumanisingDrawIsADrawAndNotAShift();
    everyNoteAndEveryStepGetsItsOwnDraw();
    aSwingUnderStraightPushesTheOffbeatEarly();
    theOfferedSwingsAreWhatTheyClaim();
    velocityOnlySoftensOffbeats();
    theVelocityAmountIsTheFractionItSays();
    shadeLeavesAPlayedNoteAlone();

    if (failures != 0)
    {
        std::printf ("\n%d groove check(s) failed\n", failures);
        return 1;
    }

    std::printf ("all groove checks passed\n");
    return 0;
}
