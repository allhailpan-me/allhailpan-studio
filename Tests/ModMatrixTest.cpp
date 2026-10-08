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
    std::uniform_int_distribution<int> anyIndex (-2000, 2000);
    std::uniform_int_distribution<int> anyCount (-50, MM::maxRoutes * 4);
    std::uniform_int_distribution<int> anyShape (0, 3);
    std::uniform_real_distribution<float> anyDepth (-1000.0f, 1000.0f);
    std::uniform_real_distribution<float> anyValue (-1000.0f, 1000.0f);

    for (int trial = 0; trial < 20000; ++trial)
    {
        MM::Patch patch;

        for (int i = 0; i < MM::maxRoutes; ++i)
        {
            patch.routes[i].source = anyIndex (rng);
            patch.routes[i].destination = anyIndex (rng);
            patch.routes[i].via = anyIndex (rng);
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

} // namespace

int main()
{
    std::printf ("Synth::ModMatrix\n\n");

    theTablesAreAContract();
    anUnknownIdIsNotAnIndex();
    theShapesAreWhatTheySay();
    foldingDoesNotDoubleTheRate();
    sourcesOutsideTheirRangeAreHeldIn();
    routingsOntoOneDestinationAddUp();
    theSumsAreWrittenNotAccumulated();
    aViaScalesAndNeverInverts();
    rowsThatCannotBeResolvedAreDropped();
    theTableFillsAndThenRefuses();
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
