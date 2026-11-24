// Checks Synth/ModMatrix.h.
//
// What can go wrong here is almost never a crash. A modulation matrix that is
// subtly wrong still loads, still plays, and still moves things; it just moves
// the wrong thing, or by the wrong amount, or in the wrong direction. Every
// one of those presents as "this preset does not sound like it did", which is
// the hardest kind of report to act on, and the easiest kind of bug to ship.
//
// So the checks here are about meaning rather than mechanics:
//
//   - a routing saved today still points at the same thing tomorrow, which is
//     about ids rather than indices
//   - a routing that cannot be resolved is dropped, not redirected
//   - two routings onto one destination add up
//   - the shapes do what their names say, including the one that is easy to
//     get wrong by reaching for the absolute value
//   - a via scales a routing down and never turns it inside out
//   - nothing in a project file, however strange, can make this read outside
//     its arrays

#include "Synth/ModMatrix.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace
{

namespace MM = Synth::ModMatrix;

int failures = 0;

void fail (const std::string& what)
{
    std::printf ("  FAIL  %s\n", what.c_str());
    ++failures;
}

std::string show (double v)
{
    char buffer[32];
    std::snprintf (buffer, sizeof (buffer), "%.6g", v);
    return buffer;
}

void expectNear (double actual, double expected, double tolerance, const std::string& what)
{
    if (! (std::fabs (actual - expected) <= tolerance))
        fail (what + ": expected " + show (expected) + ", got " + show (actual));
}

std::vector<float> noSources()
{
    return std::vector<float> ((std::size_t) MM::numSources(), 0.0f);
}

std::vector<float> noDestinations()
{
    return std::vector<float> ((std::size_t) MM::numDestinations(), 0.0f);
}

//==============================================================================
/** The tables are the contract with every project ever saved, so the things
    that would break that contract are checked directly.

    An id that is duplicated, empty, or that changes between releases is a
    patch that comes back modulating something else. There is no way to notice
    that by playing the instrument: it sounds like a patch, just not the one
    that was saved. */
void theTablesAreAContract()
{
    std::printf ("the source and destination tables\n");

    std::set<std::string> seen;

    for (const auto& s : MM::sources())
    {
        if (s.id == nullptr || std::strlen (s.id) == 0)
            fail ("a source has no id");
        else if (! seen.insert (s.id).second)
            fail (std::string ("two sources share the id ") + s.id);

        if (s.name == nullptr || std::strlen (s.name) == 0)
            fail (std::string ("source ") + s.id + " has no name");
    }

    seen.clear();

    for (const auto& d : MM::destinations())
    {
        if (d.id == nullptr || std::strlen (d.id) == 0)
            fail ("a destination has no id");
        else if (! seen.insert (d.id).second)
            fail (std::string ("two destinations share the id ") + d.id);

        if (d.name == nullptr || std::strlen (d.name) == 0)
            fail (std::string ("destination ") + d.id + " has no name");

        // A range of zero is a destination that cannot be modulated at all,
        // which is a row somebody can select and that then does nothing.
        if (! (d.range > 0.0f))
            fail (std::string ("destination ") + d.id + " has no range");
    }

    // Every id resolves back to its own index, both ways round. This is the
    // property the whole saved format rests on.
    for (int i = 0; i < MM::numSources(); ++i)
        if (MM::sourceFromId (MM::sourceId (i)) != i)
            fail ("source " + std::to_string (i) + " does not survive a round trip through its id");

    for (int i = 0; i < MM::numDestinations(); ++i)
        if (MM::destinationFromId (MM::destinationId (i)) != i)
            fail ("destination " + std::to_string (i)
                  + " does not survive a round trip through its id");

    std::printf ("    %d sources, %d destinations\n",
                 MM::numSources(), MM::numDestinations());
}

/** An id nobody knows is minus one, and never an index.

    The whole reason for ids is that a routing from another version has to be
    dropped rather than pointed somewhere arbitrary. Returning zero here, which
    is what a careless lookup does, would silently move every unknown routing
    onto the first envelope and the first destination. */
void anUnknownIdIsNotAnIndex()
{
    std::printf ("unknown ids\n");

    for (const char* id : { "", "nonsense", "ENV1", "env", "env11", "osc3pitch" })
    {
        if (MM::sourceFromId (id) != -1)
            fail (std::string ("the source id '") + id + "' resolved to something");

        if (MM::destinationFromId (id) != -1)
            fail (std::string ("the destination id '") + id + "' resolved to something");
    }

    if (MM::sourceFromId (nullptr) != -1 || MM::destinationFromId (nullptr) != -1)
        fail ("a null id resolved to something");

    // And out of range indices give back an empty id rather than reading past
    // the end of the table.
    for (int i : { -1, -100, 10000 })
    {
        if (std::strlen (MM::sourceId (i)) != 0)
            fail ("source index " + std::to_string (i) + " produced an id");

        if (std::strlen (MM::destinationId (i)) != 0)
            fail ("destination index " + std::to_string (i) + " produced an id");
    }
}

//==============================================================================
/** The shapes do what their names say. */
void theShapesAreWhatTheySay()
{
    std::printf ("shapes\n");

    const auto uni = Synth::ModMatrix::Polarity::unipolar;
    const auto bi  = Synth::ModMatrix::Polarity::bipolar;

    using S = MM::Shape;

    // As is leaves everything alone.
    expectNear (MM::shaped (0.25f, uni, S::asIs), 0.25, 1e-6, "unipolar as is");
    expectNear (MM::shaped (-0.75f, bi, S::asIs), -0.75, 1e-6, "bipolar as is");

    // Inverted is a sign flip and nothing else.
    expectNear (MM::shaped (0.25f, uni, S::inverted), -0.25, 1e-6, "unipolar inverted");
    expectNear (MM::shaped (-0.75f, bi, S::inverted), 0.75, 1e-6, "bipolar inverted");

    // A bipolar source folded up lands on the right three points.
    expectNear (MM::shaped (-1.0f, bi, S::unipolar), 0.0, 1e-6, "bipolar to unipolar at -1");
    expectNear (MM::shaped (0.0f, bi, S::unipolar), 0.5, 1e-6, "bipolar to unipolar at 0");
    expectNear (MM::shaped (1.0f, bi, S::unipolar), 1.0, 1e-6, "bipolar to unipolar at 1");

    // A unipolar source stretched out lands on the right three points.
    expectNear (MM::shaped (0.0f, uni, S::bipolar), -1.0, 1e-6, "unipolar to bipolar at 0");
    expectNear (MM::shaped (0.5f, uni, S::bipolar), 0.0, 1e-6, "unipolar to bipolar at 0.5");
    expectNear (MM::shaped (1.0f, uni, S::bipolar), 1.0, 1e-6, "unipolar to bipolar at 1");

    // Asking for the shape a source already has is a no-op, not a second
    // conversion. A unipolar source asked to be unipolar that came back
    // halved and offset would quietly halve every envelope in every patch.
    for (float v : { 0.0f, 0.3f, 0.7f, 1.0f })
        expectNear (MM::shaped (v, uni, S::unipolar), v, 1e-6, "unipolar asked to be unipolar");

    for (float v : { -1.0f, -0.2f, 0.4f, 1.0f })
        expectNear (MM::shaped (v, bi, S::bipolar), v, 1e-6, "bipolar asked to be bipolar");
}

/** Folding a bipolar source up must not double its frequency.

    The obvious way to make an LFO unipolar is to take its absolute value, and
    it is wrong: the absolute value of a sine is a sine at twice the rate with
    a different shape. Somebody asking for a unipolar LFO wants the same
    movement, only upward, and would hear the doubled one as the rate control
    lying to them.

    Checked by counting how many times the shaped signal turns around over a
    cycle. The real thing turns twice, once at each end. The absolute value
    turns four times. */
void foldingDoesNotDoubleTheRate()
{
    std::printf ("folding a bipolar source up keeps its rate\n");

    const int points = 2000;
    int turns = 0;

    float previous = 0.0f, before = 0.0f;

    for (int i = 0; i <= points; ++i)
    {
        const double phase = 2.0 * 3.14159265358979323846 * (double) i / (double) points;
        const float lfo = (float) std::sin (phase);

        const float shaped = MM::shaped (lfo, MM::Polarity::bipolar, MM::Shape::unipolar);

        if (i >= 2)
        {
            const bool wasRising = previous > before;
            const bool isRising = shaped > previous;

            if (wasRising != isRising)
                ++turns;
        }

        before = previous;
        previous = shaped;
    }

    if (turns != 2)
        fail ("a folded sine turns round " + std::to_string (turns)
              + " times in a cycle rather than twice, so the rate is not what it was");

    // And it stays inside nought to one while it does it.
    for (int i = 0; i <= points; ++i)
    {
        const double phase = 2.0 * 3.14159265358979323846 * (double) i / (double) points;
        const float shaped = MM::shaped ((float) std::sin (phase),
                                         MM::Polarity::bipolar, MM::Shape::unipolar);

        if (! (shaped >= -1e-6f && shaped <= 1.0f + 1e-6f))
            fail ("a folded sine left the range at " + show (shaped));
    }
}

/** A source outside what its polarity promises is held in rather than trusted.

    An envelope that overshoots, or a value from a hand edited project, would
    otherwise push a destination past anything the clamps downstream expect. */
void sourcesOutsideTheirRangeAreHeldIn()
{
    std::printf ("sources outside their declared range\n");

    for (float v : { -5.0f, -1.5f, 1.5f, 1e9f, -1e9f })
    {
        const float uni = MM::shaped (v, MM::Polarity::unipolar, MM::Shape::asIs);
        const float bi  = MM::shaped (v, MM::Polarity::bipolar, MM::Shape::asIs);

        if (! (uni >= 0.0f && uni <= 1.0f))
            fail ("a unipolar source of " + show (v) + " came out as " + show (uni));

        if (! (bi >= -1.0f && bi <= 1.0f))
            fail ("a bipolar source of " + show (v) + " came out as " + show (bi));
    }
}

//==============================================================================
/** Two routings onto one destination add up.

    Summing is the only behaviour that composes: anything else and the order
    rows happen to sit in the table changes the sound, so moving a row up the
    list, which looks like tidying, would change the patch. */
void routingsOntoOneDestinationAddUp()
{
    std::printf ("several routings onto one destination\n");

    const int env1 = MM::sourceFromId ("env1");
    const int env2 = MM::sourceFromId ("env2");
    const int lfo1 = MM::sourceFromId ("lfo1");
    const int cutoff = MM::destinationFromId ("cutoff");

    MM::Patch patch;
    patch.add (env1, cutoff, 0.5f);
    patch.add (env2, cutoff, 0.25f);
    patch.add (lfo1, cutoff, -0.125f);

    auto values = noSources();
    values[(std::size_t) env1] = 1.0f;
    values[(std::size_t) env2] = 0.4f;
    values[(std::size_t) lfo1] = -1.0f;

    auto sums = noDestinations();
    MM::evaluate (patch, values.data(), (int) values.size(),
                  sums.data(), (int) sums.size());

    expectNear (sums[(std::size_t) cutoff],
                1.0 * 0.5 + 0.4 * 0.25 + (-1.0) * (-0.125), 1e-6,
                "three routings onto the cutoff");

    // Nothing else moved.
    for (int i = 0; i < MM::numDestinations(); ++i)
        if (i != cutoff && sums[(std::size_t) i] != 0.0f)
            fail (std::string ("destination ") + MM::destinationId (i)
                  + " moved when nothing was pointed at it");

    // And the order of the rows makes no difference.
    MM::Patch reversed;
    reversed.add (lfo1, cutoff, -0.125f);
    reversed.add (env2, cutoff, 0.25f);
    reversed.add (env1, cutoff, 0.5f);

    auto other = noDestinations();
    MM::evaluate (reversed, values.data(), (int) values.size(),
                  other.data(), (int) other.size());

    expectNear (other[(std::size_t) cutoff], sums[(std::size_t) cutoff], 1e-6,
                "the same routings in a different order");
}

/** The sums are written over, not added to, so a caller cannot accumulate a
    block's worth of modulation by forgetting to clear. */
void theSumsAreWrittenNotAccumulated()
{
    std::printf ("the sums are written over\n");

    const int env1 = MM::sourceFromId ("env1");
    const int amp = MM::destinationFromId ("amp");

    MM::Patch patch;
    patch.add (env1, amp, 1.0f);

    auto values = noSources();
    values[(std::size_t) env1] = 0.5f;

    auto sums = noDestinations();

    for (auto& s : sums)
        s = 99.0f;

    MM::evaluate (patch, values.data(), (int) values.size(),
                  sums.data(), (int) sums.size());

    expectNear (sums[(std::size_t) amp], 0.5, 1e-6, "the wired destination");

    for (int i = 0; i < MM::numDestinations(); ++i)
        if (i != amp)
            expectNear (sums[(std::size_t) i], 0.0, 0.0,
                        std::string ("the unwired destination ") + MM::destinationId (i));
}

/** A via scales a routing down, and never turns it round.

    "This much, but only as far as that lets it." A via at zero means nothing
    gets through; a via at full means the routing is untouched. A bipolar
    source used as a via is read as unipolar, so a negative one turns the
    routing down rather than inside out, which is what the words mean. */
void aViaScalesAndNeverInverts()
{
    std::printf ("the via column\n");

    const int lfo1 = MM::sourceFromId ("lfo1");
    const int wheel = MM::sourceFromId ("modwheel");
    const int keytrack = MM::sourceFromId ("keytrack");
    const int pitch = MM::destinationFromId ("osc1pitch");

    auto values = noSources();
    values[(std::size_t) lfo1] = 1.0f;

    // A unipolar via: nothing through at zero, everything through at one.
    {
        MM::Patch patch;
        patch.add (lfo1, pitch, 0.5f, MM::Shape::asIs, wheel);

        for (double wheelValue : { 0.0, 0.25, 0.5, 1.0 })
        {
            values[(std::size_t) wheel] = (float) wheelValue;

            auto sums = noDestinations();
            MM::evaluate (patch, values.data(), (int) values.size(),
                          sums.data(), (int) sums.size());

            expectNear (sums[(std::size_t) pitch], 0.5 * wheelValue, 1e-6,
                        "a mod wheel via at " + show (wheelValue));
        }
    }

    // A bipolar via read as unipolar: minus one shuts it, plus one opens it,
    // and it never changes the sign of what gets through.
    {
        MM::Patch patch;
        patch.add (lfo1, pitch, 0.5f, MM::Shape::asIs, keytrack);

        for (double key : { -1.0, -0.5, 0.0, 0.5, 1.0 })
        {
            values[(std::size_t) keytrack] = (float) key;

            auto sums = noDestinations();
            MM::evaluate (patch, values.data(), (int) values.size(),
                          sums.data(), (int) sums.size());

            const double through = sums[(std::size_t) pitch];

            expectNear (through, 0.5 * (key * 0.5 + 0.5), 1e-6,
                        "a key tracking via at " + show (key));

            if (through < -1e-9)
                fail ("a via at " + show (key) + " turned the routing inside out");
        }
    }
}

/** A row that cannot be resolved is dropped, and the rest of the patch plays.

    This is what happens to a project saved by a later version, or one whose
    routing pointed at something since removed. Losing one routing is a patch
    that is slightly less than it was; redirecting it is a patch that is wrong,
    and the second is much worse because nothing about it looks broken. */
void rowsThatCannotBeResolvedAreDropped()
{
    std::printf ("unresolvable rows\n");

    const int env1 = MM::sourceFromId ("env1");
    const int amp = MM::destinationFromId ("amp");
    const int cutoff = MM::destinationFromId ("cutoff");

    MM::Patch patch;
    patch.add (env1, amp, 1.0f);                        // good
    patch.add (-1, cutoff, 1.0f);                       // no source
    patch.add (env1, -1, 1.0f);                         // no destination
    patch.add (MM::numSources() + 50, cutoff, 1.0f);    // a source from the future
    patch.add (env1, MM::numDestinations() + 50, 1.0f); // a destination from the future

    auto values = noSources();
    values[(std::size_t) env1] = 1.0f;

    auto sums = noDestinations();
    MM::evaluate (patch, values.data(), (int) values.size(),
                  sums.data(), (int) sums.size());

    expectNear (sums[(std::size_t) amp], 1.0, 1e-6, "the one good routing still works");
    expectNear (sums[(std::size_t) cutoff], 0.0, 0.0, "nothing arrived from a broken row");

    // A disabled row does nothing either, and leaves the rest alone.
    MM::Patch disabled;
    MM::Route off;
    off.source = env1;
    off.destination = cutoff;
    off.depth = 1.0f;
    off.enabled = false;
    disabled.add (off);
    disabled.add (env1, amp, 0.25f);

    auto otherSums = noDestinations();
    MM::evaluate (disabled, values.data(), (int) values.size(),
                  otherSums.data(), (int) otherSums.size());

    expectNear (otherSums[(std::size_t) cutoff], 0.0, 0.0, "a disabled row");
    expectNear (otherSums[(std::size_t) amp], 0.25, 1e-6, "the row beside a disabled one");
}

/** The table holds what it says it holds and refuses what it cannot, rather
    than losing a routing without saying so. */
void theTableFillsAndThenRefuses()
{
    std::printf ("filling the table\n");

    const int env1 = MM::sourceFromId ("env1");
    const int amp = MM::destinationFromId ("amp");

    MM::Patch patch;

    for (int i = 0; i < MM::maxRoutes; ++i)
        if (! patch.add (env1, amp, 0.01f))
            fail ("the table refused row " + std::to_string (i)
                  + " of " + std::to_string (MM::maxRoutes));

    if (patch.add (env1, amp, 0.01f))
        fail ("the table took a row past its own limit");

    if (patch.count != MM::maxRoutes)
        fail ("the table holds " + std::to_string (patch.count) + " rows rather than "
              + std::to_string (MM::maxRoutes));

    auto values = noSources();
    values[(std::size_t) env1] = 1.0f;

    auto sums = noDestinations();
    MM::evaluate (patch, values.data(), (int) values.size(),
                  sums.data(), (int) sums.size());

    expectNear (sums[(std::size_t) amp], 0.01 * MM::maxRoutes, 1e-5, "a full table");

    patch.clear();

    if (patch.count != 0)
        fail ("clearing left " + std::to_string (patch.count) + " rows");

    auto cleared = noDestinations();
    MM::evaluate (patch, values.data(), (int) values.size(),
                  cleared.data(), (int) cleared.size());

    expectNear (cleared[(std::size_t) amp], 0.0, 0.0, "a cleared table");

    // A count claiming more rows than the array holds is read as the array's
    // size, not followed off the end.
    MM::Patch lying;
    lying.add (env1, amp, 1.0f);
    lying.count = MM::maxRoutes * 10;

    auto lied = noDestinations();
    MM::evaluate (lying, values.data(), (int) values.size(),
                  lied.data(), (int) lied.size());

    if (! std::isfinite (lied[(std::size_t) amp]))
        fail ("a patch claiming too many rows produced something that is not finite");
}

//==============================================================================
/** Applying a sum uses the destination's own range, and holds the result
    inside the limits it was given. */
void applyingUsesTheDestinationsRange()
{
    std::printf ("applying a sum to a parameter\n");

    const int cutoff = MM::destinationFromId ("cutoff");
    const int pitch = MM::destinationFromId ("osc1pitch");

    const float cutoffRange = MM::destinations()[(std::size_t) cutoff].range;
    const float pitchRange = MM::destinations()[(std::size_t) pitch].range;

    // Full depth is the declared range, in the destination's own units.
    expectNear (MM::apply (cutoff, 0.0f, 1.0f, -100.0f, 100.0f), cutoffRange, 1e-5,
                "full depth on the cutoff");
    expectNear (MM::apply (pitch, 0.0f, -1.0f, -100.0f, 100.0f), -pitchRange, 1e-5,
                "full negative depth on the pitch");

    // And it is added to where the patch left the control, not replacing it.
    expectNear (MM::apply (pitch, 7.0f, 0.0f, -100.0f, 100.0f), 7.0, 1e-5,
                "no modulation leaves the base alone");
    expectNear (MM::apply (pitch, 7.0f, 0.25f, -100.0f, 100.0f),
                7.0 + 0.25 * pitchRange, 1e-5, "modulation adds to the base");

    // Several routings can easily add up past anything sensible, so the
    // limits are not optional. A cutoff below zero is not a quiet filter.
    expectNear (MM::apply (cutoff, 0.0f, 50.0f, -2.0f, 3.0f), 3.0, 1e-6, "held at the top");
    expectNear (MM::apply (cutoff, 0.0f, -50.0f, -2.0f, 3.0f), -2.0, 1e-6, "held at the bottom");

    // An unknown destination leaves the base where it was rather than
    // guessing a range for it.
    expectNear (MM::apply (-1, 5.0f, 1.0f, -100.0f, 100.0f), 5.0, 1e-6,
                "an unknown destination");
    expectNear (MM::apply (MM::numDestinations() + 10, 5.0f, 1.0f, -100.0f, 100.0f), 5.0, 1e-6,
                "a destination from the future");
}

//==============================================================================
/** Whatever a project file contains, this cannot be made to read outside its
    arrays or produce something that is not a number.

    A `.ahp` is a file on somebody's disk. It can be written by a later
    version, copied half way, or edited by hand, and the matrix is walked on
    the audio thread where a read past the end of an array is a crash rather
    than a wrong note. The sanitizers are what actually catch that; this is
    the loop that gives them something to catch. */
void nothingInAPatchCanBreakIt()
{
    std::printf ("arbitrary patches\n");

    std::mt19937 rng (20261008);

    // Two ranges rather than one, and the narrow one is the one that matters.
    // A bound is admitted one index too far by an off by one, so what has to
    // be hit is a single exact value, and with both ends of a row drawn from
    // plus or minus two thousand the chance of a row reaching a given bound
    // at all is two draws of one in four thousand at once: 0.69 rows in a
    // whole run, which `theBoundsAreTriedAtTheirEdge` has the measurement
    // for. Drawn from plus or minus forty it is thousands of rows. The wide
    // range is kept for what it is really good at, which is counts and
    // indices far enough out to catch arithmetic that overflows rather than
    // arithmetic that is off by one.
    std::uniform_int_distribution<int> nearIndex (-40, 40);
    std::uniform_int_distribution<int> farIndex (-2000, 2000);
    std::uniform_int_distribution<int> howFar (0, 4);

    const auto anIndex = [&] { return howFar (rng) == 0 ? farIndex (rng) : nearIndex (rng); };

    std::uniform_int_distribution<int> anyCount (-50, MM::maxRoutes * 4);
    std::uniform_int_distribution<int> anyShape (0, 3);
    std::uniform_real_distribution<float> anyDepth (-1000.0f, 1000.0f);
    std::uniform_real_distribution<float> anyValue (-1000.0f, 1000.0f);

    for (int trial = 0; trial < 20000; ++trial)
    {
        MM::Patch patch;

        for (int i = 0; i < MM::maxRoutes; ++i)
        {
            patch.routes[i].source = anIndex();
            patch.routes[i].destination = anIndex();
            patch.routes[i].via = anIndex();
            patch.routes[i].depth = anyDepth (rng);
            patch.routes[i].shape = (MM::Shape) anyShape (rng);
            patch.routes[i].enabled = (trial % 7) != 0;
        }

        patch.count = anyCount (rng);

        auto values = noSources();

        for (auto& v : values)
            v = anyValue (rng);

        auto sums = noDestinations();
        MM::evaluate (patch, values.data(), (int) values.size(),
                      sums.data(), (int) sums.size());

        for (float s : sums)
            if (! std::isfinite (s))
            {
                fail ("a destination sum stopped being finite");
                return;
            }

        // Asked the same questions, over the same patches. It is read on the
        // audio thread beside `evaluate` and the count it is handed is the
        // same count, so it has to survive the same nonsense.
        //
        // The answer is checked rather than discarded, because these patches
        // carry rows pointed at destinations that do not exist, and a routing
        // to a destination that does not exist is not a routing. Thrown away,
        // every guard on the way in reads as covered and is not.
        // Every destination is asked, including the ones inside the table
        // whose answer these patches make unpredictable. Asking only about
        // the ones outside it would stop at the guard on the way in and never
        // walk the rows at all, which is where the counts these patches carry
        // would be followed off the end of the table.
        for (int d = -2; d <= MM::numDestinations() + 1; ++d)
        {
            const bool claimed = patch.anyRouteTo (d);

            if (claimed && (d < 0 || d >= MM::numDestinations()))
            {
                fail ("destination " + std::to_string (d)
                      + ", which does not exist, was claimed as having a routing");
                return;
            }
        }
    }

    // Short arrays, which is what a caller would hand over if the tables grew
    // and it had not been rebuilt.
    {
        MM::Patch patch;
        patch.add (MM::sourceFromId ("env1"), MM::destinationFromId ("cutoff"), 1.0f);

        std::vector<float> fewSources (2, 1.0f);
        std::vector<float> fewDestinations (2, 0.0f);

        MM::evaluate (patch, fewSources.data(), (int) fewSources.size(),
                      fewDestinations.data(), (int) fewDestinations.size());

        for (float s : fewDestinations)
            if (! std::isfinite (s))
                fail ("short arrays produced something that is not finite");
    }

    // And nothing at all.
    {
        MM::Patch patch;
        patch.add (0, 0, 1.0f);

        auto sums = noDestinations();

        MM::evaluate (patch, nullptr, 0, sums.data(), (int) sums.size());

        for (float s : sums)
            if (s != 0.0f)
                fail ("evaluating with no sources moved a destination");

        MM::evaluate (patch, nullptr, 0, nullptr, 0);   // must simply return
        MM::evaluate (patch, sums.data(), (int) sums.size(), nullptr, 0);
    }
}

/** A worked patch, end to end, because the pieces being right separately is
    not the same as the whole thing meaning what it reads as meaning.

    The patch: a filter envelope, vibrato that only arrives when the mod wheel
    is up, velocity opening the filter further, and the unison stack spread
    across the wavetable so each voice sits on a slightly different waveform. */
void aWholePatchMeansWhatItReads()
{
    std::printf ("a worked patch\n");

    const int env2 = MM::sourceFromId ("env2");
    const int lfo1 = MM::sourceFromId ("lfo1");
    const int wheel = MM::sourceFromId ("modwheel");
    const int velocity = MM::sourceFromId ("velocity");
    const int unison = MM::sourceFromId ("unison");

    const int cutoff = MM::destinationFromId ("cutoff");
    const int pitch = MM::destinationFromId ("osc1pitch");
    const int position = MM::destinationFromId ("osc1pos");

    MM::Patch patch;
    patch.add (env2, cutoff, 0.75f);
    patch.add (velocity, cutoff, 0.3f);
    patch.add (lfo1, pitch, 0.02f, MM::Shape::asIs, wheel);
    patch.add (unison, position, 0.15f);

    auto values = noSources();
    values[(std::size_t) env2] = 0.6f;
    values[(std::size_t) velocity] = 0.9f;
    values[(std::size_t) lfo1] = 1.0f;
    values[(std::size_t) wheel] = 0.5f;
    values[(std::size_t) unison] = -1.0f;      // the far left voice of the stack

    auto sums = noDestinations();
    MM::evaluate (patch, values.data(), (int) values.size(),
                  sums.data(), (int) sums.size());

    expectNear (sums[(std::size_t) cutoff], 0.6 * 0.75 + 0.9 * 0.3, 1e-6,
                "envelope and velocity on the cutoff");
    expectNear (sums[(std::size_t) pitch], 1.0 * 0.02 * 0.5, 1e-6,
                "vibrato at half a mod wheel");
    expectNear (sums[(std::size_t) position], -1.0 * 0.15, 1e-6,
                "the leftmost unison voice on the wavetable position");

    // In real units: the cutoff is eight octaves at full depth, so this patch
    // opens it by a bit over five.
    const float cutoffRange = MM::destinations()[(std::size_t) cutoff].range;
    expectNear (MM::apply (cutoff, 0.0f, sums[(std::size_t) cutoff], -20.0f, 20.0f),
                (0.6 * 0.75 + 0.9 * 0.3) * cutoffRange, 1e-5,
                "the cutoff in octaves");

    // With the wheel down the vibrato is gone and nothing else has changed.
    values[(std::size_t) wheel] = 0.0f;

    auto down = noDestinations();
    MM::evaluate (patch, values.data(), (int) values.size(),
                  down.data(), (int) down.size());

    expectNear (down[(std::size_t) pitch], 0.0, 1e-9, "vibrato with the wheel down");
    expectNear (down[(std::size_t) cutoff], sums[(std::size_t) cutoff], 1e-9,
                "the cutoff did not notice the wheel");
}

//==============================================================================
/** `anyRouteTo` and `evaluate` have to agree about which rows are live.

    This is the one function here whose caller cannot tolerate a wrong answer
    in either direction. `Synth/Voice.h` asks whether anything drives the
    gate, and opens the gate from the amplitude envelope when nothing does, so
    that an instrument nobody has patched still makes a sound. Answer false
    when something is wired and two things fight over the gate. Answer true
    when nothing live is wired and the envelope is taken away with nothing put
    in its place: the gate is in series with everything the voice produces, so
    the note does not come back quieter, it does not come back at all.

    So the property is not "does it find a row". It is that the two functions
    draw the line between a live row and a dead one in the same place, for
    every patch either of them could be handed. Asking the sum cannot answer
    it, which is the whole reason the function exists: a sum of zero is what a
    live row sitting at zero depth produces and also what no row at all
    produces.
*/
void aSwitchedOffRowIsNotARouting()
{
    std::printf ("what counts as a routing\n");

    const int env1 = MM::sourceFromId ("env1");
    const int gate = MM::destinationFromId ("gateopen");
    const int cutoff = MM::destinationFromId ("cutoff");

    auto values = noSources();
    values[(std::size_t) env1] = 1.0f;

    // A wired, switched on row is a routing.
    {
        MM::Patch patch;
        patch.add (env1, gate, 1.0f);

        if (! patch.anyRouteTo (gate))
            fail ("a wired row pointed at the gate did not count as a routing");

        if (patch.anyRouteTo (cutoff))
            fail ("a row pointed at the gate counted as a routing to the cutoff");
    }

    // The same row switched off is not. This is the row's own on/off control,
    // which is a saved parameter a host can automate, so it is a thing that
    // moves while somebody is playing.
    {
        MM::Patch patch;
        MM::Route off;
        off.source = env1;
        off.destination = gate;
        off.depth = 1.0f;
        off.enabled = false;
        patch.add (off);

        if (patch.anyRouteTo (gate))
            fail ("a switched off row still counted as a routing, so the voice "
                  "would take the amplitude envelope off the gate and put a "
                  "sum of nothing in its place");

        auto sums = noDestinations();
        MM::evaluate (patch, values.data(), (int) values.size(),
                      sums.data(), (int) sums.size());

        expectNear (sums[(std::size_t) gate], 0.0, 0.0,
                    "a switched off row contributing to the sum");
    }

    // A row with one end picked and the other still empty is what the editor
    // holds while somebody is halfway through wiring it up, and it drives
    // nothing.
    {
        MM::Patch patch;
        MM::Route half;
        half.source = -1;
        half.destination = gate;
        half.depth = 1.0f;
        patch.add (half);

        if (patch.anyRouteTo (gate))
            fail ("a row with no source counted as a routing");
    }

    // A row sitting above the count is a row the engine never looks at,
    // because the count is one past the last wired row and nothing below it
    // is read. It must not count as a routing either.
    {
        MM::Patch patch;
        patch.add (env1, cutoff, 0.5f);

        patch.routes[20].source = env1;
        patch.routes[20].destination = gate;
        patch.routes[20].depth = 1.0f;

        if (patch.anyRouteTo (gate))
            fail ("a row above the count counted as a routing");

        if (! patch.anyRouteTo (cutoff))
            fail ("a row below the count stopped counting as a routing");
    }

    // And a count claiming more rows than the array holds must be read as the
    // array's size, the way `evaluate` reads it. This runs on the audio
    // thread, where reading past the end of the table is a crash rather than
    // a wrong note, and the sanitizers are what catch it.
    {
        MM::Patch lying;
        lying.add (env1, gate, 1.0f);
        lying.count = MM::maxRoutes * 10;

        if (! lying.anyRouteTo (gate))
            fail ("a patch claiming too many rows lost a routing it really had");

        lying.count = -500;

        if (lying.anyRouteTo (gate))
            fail ("a patch claiming a negative number of rows found one anyway");
    }

    // A row pointing at a source that is not there drives nothing either,
    // and has to be read the same way at both ends. `evaluate` skips it
    // because it cannot read a source value for it; this has to agree, or a
    // project saved by a later version comes back with its gate held shut by
    // a row whose source this version has never heard of.
    {
        MM::Patch patch;
        patch.add (MM::numSources() + 50, gate, 1.0f);

        if (patch.anyRouteTo (gate))
            fail ("a row whose source does not exist counted as a routing");

        auto sums = noDestinations();
        MM::evaluate (patch, values.data(), (int) values.size(),
                      sums.data(), (int) sums.size());

        expectNear (sums[(std::size_t) gate], 0.0, 0.0,
                    "a row whose source does not exist contributing to the sum");
    }

    // Nothing is a routing to a destination that does not exist.
    for (int d : { -1, -2000, MM::numDestinations(), MM::numDestinations() + 1,
                   MM::numDestinations() + 2000 })
    {
        MM::Patch patch;
        patch.add (env1, gate, 1.0f);

        if (patch.anyRouteTo (d))
            fail ("destination " + std::to_string (d) + " counted as having a routing");
    }

    //==========================================================================
    // The agreement itself, over generated patches.
    //
    // Every source is held at 0.8, which is the value that makes all four
    // shapes non-zero for both polarities: as is and inverted give 0.8,
    // a bipolar source folded up gives 0.9, a unipolar source stretched out
    // gives 0.6. So a live row always moves its destination off zero, and a
    // destination that did not move had no live row pointed at it. Pick the
    // wrong constant here, 0.5 for instance, and a unipolar source stretched
    // out lands exactly on zero and the check quietly stops meaning anything.
    std::mt19937 rng (20261124);
    std::uniform_int_distribution<int> aSource (-1, MM::numSources() - 1);
    std::uniform_int_distribution<int> aDestination (-1, MM::numDestinations() - 1);
    std::uniform_int_distribution<int> aShape (0, 3);
    std::uniform_int_distribution<int> aCoin (0, 1);
    std::uniform_int_distribution<int> aRowCount (0, MM::maxRoutes);
    std::uniform_real_distribution<float> aDepth (0.1f, 1.0f);

    int liveRows = 0;
    int deadRows = 0;

    for (int trial = 0; trial < 4000; ++trial)
    {
        MM::Patch patch;

        // At most one row per destination, so a destination that moved can
        // only have moved through the row pointed at it. Two rows that
        // happened to cancel would make a live destination read as a dead
        // one and the agreement check would be measuring nothing.
        std::vector<bool> taken ((std::size_t) MM::numDestinations(), false);

        const int rows = aRowCount (rng);

        for (int i = 0; i < rows; ++i)
        {
            MM::Route r;
            r.source = aSource (rng);
            r.destination = aDestination (rng);

            if (r.destination >= 0)
            {
                if (taken[(std::size_t) r.destination])
                    continue;

                taken[(std::size_t) r.destination] = true;
            }

            r.depth = aDepth (rng) * (aCoin (rng) == 0 ? 1.0f : -1.0f);
            r.shape = (MM::Shape) aShape (rng);
            r.via = aCoin (rng) == 0 ? -1 : aSource (rng);
            r.enabled = aCoin (rng) == 0;

            patch.add (r);

            if (r.enabled && r.isWired())
                ++liveRows;
            else
                ++deadRows;
        }

        auto held = noSources();

        for (auto& v : held)
            v = 0.8f;

        auto sums = noDestinations();
        MM::evaluate (patch, held.data(), (int) held.size(),
                      sums.data(), (int) sums.size());

        for (int d = 0; d < MM::numDestinations(); ++d)
        {
            const bool claimed = patch.anyRouteTo (d);
            const bool moved = sums[(std::size_t) d] != 0.0f;

            if (claimed != moved)
            {
                fail (std::string ("destination ") + MM::destinationId (d)
                      + (claimed ? " was claimed as driven and did not move"
                                 : " moved without being claimed as driven"));
                return;
            }
        }
    }

    std::printf ("    agreed over 4000 patches, %d live rows and %d dead ones\n",
                 liveRows, deadRows);
}

//==============================================================================
/** Every bound is tried at the first index past the end, deliberately.

    This is here because the randomised loop below does not do it, and looks
    as though it does. It draws indices from minus two thousand to two
    thousand, and for a row to reach the source bound at all it needs its
    source to land on exactly one value and its destination to land anywhere
    inside the table at the same time: two independent draws of about one in
    four thousand, which works out at 0.69 rows in a whole run of twenty
    thousand trials. Measured rather than reasoned about: an off by one in
    that bound survived the suite, and narrowing the draw to plus or minus
    forty killed it immediately.

    So a wider fuzz is not a stronger one when what it has to hit is a single
    exact value. The range below is narrowed for that reason, and the exact
    edges are tried here as well, because a generated case that usually
    arrives is not the same thing as a case that always does. */
void theBoundsAreTriedAtTheirEdge()
{
    std::printf ("the first index past the end of each table\n");

    const int env1 = MM::sourceFromId ("env1");
    const int amp = MM::destinationFromId ("amp");

    // `sourceId` and `destinationId` at the last real index and the first
    // index past it.
    if (std::strlen (MM::sourceId (MM::numSources() - 1)) == 0)
        fail ("the last source has no id");

    if (std::strlen (MM::sourceId (MM::numSources())) != 0)
        fail ("the first index past the end of the source table produced an id");

    if (std::strlen (MM::destinationId (MM::numDestinations() - 1)) == 0)
        fail ("the last destination has no id");

    if (std::strlen (MM::destinationId (MM::numDestinations())) != 0)
        fail ("the first index past the end of the destination table produced an id");

    // `apply` at the first destination past the end. The range it would read
    // there is whatever happens to be in memory, so this is a sanitizer
    // check as much as a comparison.
    expectNear (MM::apply (MM::numDestinations(), 5.0f, 1.0f, -100.0f, 100.0f), 5.0, 1e-6,
                "apply at the first destination past the end");
    expectNear (MM::apply (MM::numDestinations() - 1, 0.0f, 1.0f, -100.0f, 100.0f),
                MM::destinations()[(std::size_t) (MM::numDestinations() - 1)].range, 1e-5,
                "apply at the last real destination");

    // `evaluate` with a row pointed at the first source past the end, and at
    // the first destination past the end, with the other end valid so the
    // row actually reaches the bound being tested.
    {
        MM::Patch patch;
        patch.add (MM::numSources(), amp, 1.0f);
        patch.add (env1, MM::numDestinations(), 1.0f);
        patch.add (env1, amp, 0.5f);

        auto values = noSources();

        for (auto& v : values)
            v = 1.0f;

        auto sums = noDestinations();
        MM::evaluate (patch, values.data(), (int) values.size(),
                      sums.data(), (int) sums.size());

        expectNear (sums[(std::size_t) amp], 0.5, 1e-6,
                    "the good row beside two rows that sit on a bound");
    }

    // The same for the via column, which has a bound of its own.
    {
        MM::Patch patch;
        patch.add (env1, amp, 1.0f, MM::Shape::asIs, MM::numSources());

        auto values = noSources();

        for (auto& v : values)
            v = 1.0f;

        auto sums = noDestinations();
        MM::evaluate (patch, values.data(), (int) values.size(),
                      sums.data(), (int) sums.size());

        if (! std::isfinite (sums[(std::size_t) amp]))
            fail ("a via on the first source past the end produced something "
                  "that is not finite");
    }

    // And a caller whose arrays are shorter than the tables: a row pointed at
    // the first index past the end of the array it was given, rather than
    // past the end of the table.
    {
        MM::Patch patch;
        patch.add (4, 4, 1.0f);

        std::vector<float> fourSources (4, 1.0f);
        std::vector<float> fourDestinations (4, 0.0f);

        MM::evaluate (patch, fourSources.data(), (int) fourSources.size(),
                      fourDestinations.data(), (int) fourDestinations.size());

        for (float s : fourDestinations)
            if (s != 0.0f)
                fail ("a row on the edge of a short array moved something");
    }
}

//==============================================================================
/** The two columns of the tables that are claims rather than arithmetic.

    Neither of these can be derived from anything, so they are pinned by name,
    the way `PanOneParamsTest` pins the presets. That is a weaker kind of
    check and it is written down here as one: it does not prove the numbers
    are the right numbers, it only makes changing one a deliberate act that
    shows up in a diff rather than a silent edit that rescales every patch
    anybody has saved.

    The two it protects are worth the words:

    A source's polarity is a fact about the module behind it, and it decides
    what the shapes do. Declare an envelope bipolar and every routing from it
    asking to be bipolar stops stretching, so the patch's movement halves,
    and the envelope itself still looks and sounds like an envelope.

    A destination's range is what a depth of one is worth. Halve the cutoff's
    and every saved patch's filter sweep halves with it. Nothing in the file
    can notice, because everything else here reads the range out of the same
    table: the existing check on `apply` compares against the number it is
    testing, which is why halving it survived the whole suite. */
void theTablesSayWhatTheyMean()
{
    std::printf ("the declared polarities and ranges\n");

    const auto uni = MM::Polarity::unipolar;
    const auto bi  = MM::Polarity::bipolar;

    // Unipolar is anything that rests at nothing and goes up: an envelope, a
    // slope, a pedal, a position somebody set. Bipolar is anything with a
    // middle: a wave about a centre, a distance from middle C, a bend, a
    // draw either side of zero.
    const struct { const char* id; MM::Polarity polarity; } expectedPolarity[]
    {
        { "env1", uni }, { "env2", uni }, { "env3", uni }, { "env4", uni },
        { "lfo1", bi },  { "lfo2", bi },  { "lfo3", bi },  { "lfo4", bi },
        { "slope1", uni }, { "slope2", uni },
        { "velocity", uni },
        { "keytrack", bi },
        { "modwheel", uni },
        { "pressure", uni },
        { "bend", bi },
        { "random", bi },
        { "unison", bi },
        { "gate", uni },
        { "macro1", uni }, { "macro2", uni }, { "macro3", uni }, { "macro4", uni },
    };

    const int expectedSources = (int) (sizeof (expectedPolarity) / sizeof (expectedPolarity[0]));

    if (expectedSources != MM::numSources())
        fail ("the source table has " + std::to_string (MM::numSources())
              + " entries and this check names " + std::to_string (expectedSources)
              + ", so a source has been added or removed without its polarity "
                "being stated here");

    for (const auto& e : expectedPolarity)
    {
        const int i = MM::sourceFromId (e.id);

        if (i < 0)
        {
            fail (std::string ("the source ") + e.id + " has gone");
            continue;
        }

        if (MM::sources()[(std::size_t) i].polarity != e.polarity)
            fail (std::string ("the source ") + e.id + " is declared "
                  + (e.polarity == uni ? "bipolar where it should be unipolar"
                                       : "unipolar where it should be bipolar"));
    }

    const struct { const char* id; float range; } expectedRange[]
    {
        { "osc1pitch", 48.0f }, { "osc2pitch", 48.0f },
        { "osc1pos", 1.0f },    { "osc2pos", 1.0f },
        { "osc1level", 1.0f },  { "osc2level", 1.0f }, { "sublevel", 1.0f },
        { "osc1phase", 1.0f },  { "osc2phase", 1.0f },
        { "fm", 1.0f },
        { "noiselevel", 1.0f }, { "noisecolour", 1.0f },
        { "cutoff", 8.0f },
        { "resonance", 8.0f },
        { "drive", 8.0f },      { "drivebias", 1.0f },
        { "fold", 8.0f },       { "foldsymmetry", 1.0f }, { "foldmix", 1.0f },
        { "gateopen", 1.0f },   { "gaterange", 4.0f },
        { "amp", 1.0f },        { "pan", 1.0f },
        { "unisondetune", 1.0f }, { "unisonspread", 1.0f },
        { "lfo1rate", 6.0f },   { "lfo2rate", 6.0f },
        { "lfo3rate", 6.0f },   { "lfo4rate", 6.0f },
        { "slope1rate", 6.0f }, { "slope2rate", 6.0f },
        { "glide", 1.0f },
    };

    const int expectedDestinations = (int) (sizeof (expectedRange) / sizeof (expectedRange[0]));

    if (expectedDestinations != MM::numDestinations())
        fail ("the destination table has " + std::to_string (MM::numDestinations())
              + " entries and this check names " + std::to_string (expectedDestinations)
              + ", so a destination has been added or removed without its range "
                "being stated here");

    for (const auto& e : expectedRange)
    {
        const int i = MM::destinationFromId (e.id);

        if (i < 0)
        {
            fail (std::string ("the destination ") + e.id + " has gone");
            continue;
        }

        expectNear (MM::destinations()[(std::size_t) i].range, e.range, 1e-6,
                    std::string ("the declared range of ") + e.id);
    }

    //==========================================================================
    // Where an id carries a number, the name a person reads has to carry the
    // same one.
    //
    // This is what catches the mistake the whole file is built to prevent,
    // and it is the one shape of it a round trip through the id cannot see:
    // swap the ids of `osc1pitch` and `osc2pitch` and every id still resolves
    // to an index that resolves back to the same id, no id is duplicated or
    // empty, and every patch anybody saved is now modulating the other
    // oscillator. What does change is that the row reading "Osc 1 pitch"
    // carries the id `osc2pitch`, so the menu and the project file have
    // stopped agreeing, and that is checkable.
    const auto firstDigit = [] (const char* s)
    {
        for (const char* p = s; *p != '\0'; ++p)
            if (*p >= '0' && *p <= '9')
                return *p;

        return '\0';
    };

    int numbered = 0;

    for (int i = 0; i < MM::numSources(); ++i)
    {
        const auto& s = MM::sources()[(std::size_t) i];
        const char inId = firstDigit (s.id);

        if (inId != '\0')
            ++numbered;

        if (inId != firstDigit (s.name))
            fail (std::string ("the source id ") + s.id + " and the name "
                  + s.name + " do not carry the same number");
    }

    for (int i = 0; i < MM::numDestinations(); ++i)
    {
        const auto& d = MM::destinations()[(std::size_t) i];
        const char inId = firstDigit (d.id);

        if (inId != '\0')
            ++numbered;

        if (inId != firstDigit (d.name))
            fail (std::string ("the destination id ") + d.id + " and the name "
                  + d.name + " do not carry the same number");
    }

    std::printf ("    %d sources and destinations carry a number in their id\n", numbered);
}

} // namespace

int main()
{
    std::printf ("Synth::ModMatrix\n\n");

    theTablesAreAContract();
    theTablesSayWhatTheyMean();
    anUnknownIdIsNotAnIndex();
    theShapesAreWhatTheySay();
    foldingDoesNotDoubleTheRate();
    sourcesOutsideTheirRangeAreHeldIn();
    routingsOntoOneDestinationAddUp();
    theSumsAreWrittenNotAccumulated();
    aViaScalesAndNeverInverts();
    rowsThatCannotBeResolvedAreDropped();
    aSwitchedOffRowIsNotARouting();
    theTableFillsAndThenRefuses();
    theBoundsAreTriedAtTheirEdge();
    applyingUsesTheDestinationsRange();
    aWholePatchMeansWhatItReads();
    nothingInAPatchCanBreakIt();

    std::printf ("\n");

    if (failures > 0)
    {
        std::printf ("%d check(s) failed.\n", failures);
        return 1;
    }

    std::printf ("All checks passed.\n");
    return 0;
}
