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
    velocityOnlySoftensOffbeats();

    if (failures != 0)
    {
        std::printf ("\n%d groove check(s) failed\n", failures);
        return 1;
    }

    std::printf ("all groove checks passed\n");
    return 0;
}
