// ---------------------------------------------------------------------------
// Checks the knobs and the presets of the built-in instrument.
//
// The table in PanOneParams.h is walked in four places: building the
// parameters, reading them into a patch once per block, writing a preset out
// to them, and saving and loading a project. None of the ways it can be wrong
// announce themselves:
//
//   two entries writing the same field   one knob moves another, and the
//                                        patch is never what the panel says
//   get and set on different fields      the knob does nothing, or it does
//                                        something and then springs back
//   a preset value outside its range     the parameter clamps it, and the
//                                        preset ships sounding like something
//                                        nobody designed
//   an id that changed                   every project saved before the
//                                        change silently loads a default
//
// All four are things you would eventually hear and never be able to explain,
// which is the class of bug this directory exists for. The fifth, a field
// added to the patch and not to the table, cannot be tested from here and is
// pinned by a static_assert on the size of the struct instead.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include "PanOneParams.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <set>
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

/** Float precision, not an opinion about audio: the patch holds doubles and a
    parameter holds a float, so a value that has been through one is equal to
    the original only to about seven digits. */
static bool near (float a, float b)
{
    const float scale = std::max (1.0f, std::max (std::abs (a), std::abs (b)));
    return std::abs (a - b) <= 1.0e-6f * scale;
}

//==============================================================================
static void identifiers()
{
    std::set<std::string> seen;

    for (const auto& spec : PanOneParams::all())
    {
        const std::string id (spec.id);

        check (! id.empty(), "a parameter has no id");
        check (seen.insert (id).second, "two parameters share the id \"" + id + "\"");
        check (spec.name != nullptr && *spec.name != '\0',
               "parameter \"" + id + "\" has no name");

        // Ids go into a project file as they are, and are matched on reload.
        // Keeping them to this alphabet means no escaping to get wrong and no
        // case folding to disagree about.
        for (char c : id)
            check ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'),
                   "id \"" + id + "\" has a character that does not belong in one");
    }

    check (PanOneParams::count() > 0, "there are no parameters at all");
}

static void ranges()
{
    for (const auto& spec : PanOneParams::all())
    {
        const std::string id (spec.id);

        check (spec.minimum < spec.maximum,
               "\"" + id + "\" has a range that is empty or backwards");

        if (spec.kind == PanOneParams::Kind::continuous)
        {
            // NormalisableRange::setSkewForCentre asserts on both of these,
            // which in a release build is not an assert at all: it is a
            // logarithm of zero or of a negative number, and a control that
            // does nothing.
            check (spec.centre > spec.minimum && spec.centre < spec.maximum,
                   "\"" + id + "\" has a centre that is not inside its range, "
                   "which is an invalid skew rather than a bad choice");
        }
        else
        {
            check (spec.minimum == 0.0f,
                   "choice \"" + id + "\" does not start at zero, so its stored "
                   "index is not its index");
        }
    }
}

static void choicesMatchTheShapes()
{
    // A choice parameter saves the index, so the list it indexes has to stay
    // the same length as the enum it stands for and in the same order. A
    // shape added to Oscillator::Shape without a name here would be
    // unreachable; a name added without the shape would select the wrong one.
    for (const auto& spec : PanOneParams::all())
        if (spec.kind == PanOneParams::Kind::choice)
            check ((int) spec.maximum == PanOneParams::numShapes - 1,
                   std::string ("choice \"") + spec.id + "\" does not cover every shape");

    for (int i = 0; i < PanOneParams::numShapes; ++i)
        check (PanOneParams::shapeNames[i] != nullptr
                 && *PanOneParams::shapeNames[i] != '\0',
               "shape " + std::to_string (i) + " has no name");

    // Round trip through the index, including out of range values, which is
    // what a project saved by a later version would hand back.
    for (int i = 0; i < PanOneParams::numShapes; ++i)
        check ((int) PanOneParams::toShape ((float) i) == i,
               "shape index " + std::to_string (i) + " does not survive the round trip");

    check (PanOneParams::toShape (-5.0f) == (Oscillator::Shape) 0,
           "a negative shape index should clamp to the first shape");
    check (PanOneParams::toShape (99.0f)
             == (Oscillator::Shape) (PanOneParams::numShapes - 1),
           "an impossible shape index should clamp to the last shape");
}

//==============================================================================
/** The important one. Setting a parameter has to change that parameter's
    reading and nothing else's.

    This is what catches two entries pointing at the same field of the patch,
    which is the mistake the shape of this table invites: the lines are near
    identical and the amplitude and filter envelopes have the same four field
    names under different parents. */
static void everyKnobIsItsOwnKnob()
{
    const auto& specs = PanOneParams::all();

    for (std::size_t i = 0; i < specs.size(); ++i)
    {
        PanOneParams::Patch patch;

        const auto before = PanOneParams::fromPatch (patch);

        // Somewhere in range and not where it already was. Three eighths of
        // the way up is chosen so it is not the middle, not an end, and not
        // a round number that might coincide with a default.
        const float target = specs[i].kind == PanOneParams::Kind::choice
                               ? (before[i] == 0.0f ? 1.0f : 0.0f)
                               : specs[i].minimum
                                   + 0.375f * (specs[i].maximum - specs[i].minimum);

        specs[i].set (patch, target);

        const auto after = PanOneParams::fromPatch (patch);

        check (near (after[i], target),
               std::string ("setting \"") + specs[i].id + "\" did not change what it reads back");

        for (std::size_t k = 0; k < specs.size(); ++k)
        {
            if (k == i)
                continue;

            if (! near (before[k], after[k]))
                fail (std::string ("setting \"") + specs[i].id
                        + "\" also moved \"" + specs[k].id
                        + "\": the two are writing the same field of the patch");
        }
    }
}

static void patchesSurviveTheTable()
{
    std::mt19937 rng (20260204);
    const auto& specs = PanOneParams::all();

    for (int trial = 0; trial < 2000; ++trial)
    {
        // Built through the table so that every value is in range, then read
        // back out. Writing into a patch that starts somewhere else is the
        // part that matters: anything the table does not reach keeps the
        // wrong value and the comparison fails.
        PanOneParams::Patch source;
        std::vector<float> wanted;
        wanted.reserve (specs.size());

        for (const auto& spec : specs)
        {
            std::uniform_real_distribution<float> anywhere (spec.minimum, spec.maximum);
            const float value = spec.kind == PanOneParams::Kind::choice
                                  ? (float) (int) anywhere (rng)
                                  : anywhere (rng);
            wanted.push_back (value);
            spec.set (source, value);
        }

        PanOneParams::Patch destination;
        destination.cutoff = 12345.0;        // so an unreached field shows up
        destination.resonance = 7.77;
        destination.amp = { 1.1, 2.2, 0.33, 4.4 };
        destination.filter = { 5.5, 6.6, 0.77, 8.8 };

        PanOneParams::toPatch (PanOneParams::fromPatch (source), destination);

        const auto got = PanOneParams::fromPatch (destination);

        for (std::size_t i = 0; i < specs.size(); ++i)
            if (! near (got[i], wanted[i]))
            {
                fail (std::string ("\"") + specs[i].id + "\" did not survive a patch round trip");
                return;
            }
    }
}

//==============================================================================
static void presetsFitTheirKnobs()
{
    const auto& specs = PanOneParams::all();
    std::set<std::string> names;

    check (! PanOneParams::presets().empty(), "there are no presets");

    for (const auto& preset : PanOneParams::presets())
    {
        const std::string name (preset.name != nullptr ? preset.name : "");

        check (! name.empty(), "a preset has no name");
        check (names.insert (name).second, "two presets are called \"" + name + "\"");

        const auto values = PanOneParams::fromPatch (preset.patch);

        for (std::size_t i = 0; i < specs.size(); ++i)
        {
            if (values[i] < specs[i].minimum || values[i] > specs[i].maximum)
                fail ("preset \"" + name + "\" sets \"" + std::string (specs[i].id)
                        + "\" to " + std::to_string (values[i])
                        + ", outside its range of " + std::to_string (specs[i].minimum)
                        + " to " + std::to_string (specs[i].maximum)
                        + ", so it will be clamped and the preset will not be the preset");
        }
    }
}

static void groupsCoverEveryKnobOnce()
{
    // The editor draws its headings from these, so a gap means a knob that
    // exists, works, saves and is on no panel anywhere.
    std::vector<int> timesListed (PanOneParams::count(), 0);

    for (const auto& group : PanOneParams::groups())
    {
        check (group.heading != nullptr && *group.heading != '\0', "a group has no heading");
        check (group.count > 0, std::string ("group \"") + group.heading + "\" is empty");

        for (std::size_t i = group.first; i < group.first + group.count; ++i)
        {
            if (i >= timesListed.size())
            {
                fail (std::string ("group \"") + group.heading + "\" runs past the last parameter");
                return;
            }

            ++timesListed[i];
        }
    }

    for (std::size_t i = 0; i < timesListed.size(); ++i)
    {
        if (timesListed[i] == 0)
            fail (std::string ("\"") + PanOneParams::all()[i].id + "\" is in no group, so the editor will not show it");
        else if (timesListed[i] > 1)
            fail (std::string ("\"") + PanOneParams::all()[i].id + "\" is in more than one group");
    }
}

//==============================================================================
int main()
{
    std::printf ("pan one parameters (%zu of them, %zu presets)\n",
                 PanOneParams::count(), PanOneParams::presets().size());

    identifiers();
    ranges();
    choicesMatchTheShapes();
    everyKnobIsItsOwnKnob();
    patchesSurviveTheTable();
    presetsFitTheirKnobs();
    groupsCoverEveryKnobOnce();

    if (failures != 0)
    {
        std::printf ("PanOneParamsTest: %d failure(s)\n", failures);
        return 1;
    }

    std::printf ("all pan one parameter checks passed\n");
    return 0;
}
