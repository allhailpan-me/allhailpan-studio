// ---------------------------------------------------------------------------
// Checks the controls and the presets of the built-in instrument.
//
// The table in PanOneParams.h is walked in four places: building the
// parameters, reading them into a patch once per block, writing a preset out
// to them, and saving and loading a project. None of the ways it can be wrong
// announce themselves:
//
//   two entries writing the same field   one control moves another, and the
//                                        patch is never what the panel says
//   get and set on different fields      the control does nothing, or it does
//                                        something and then springs back
//   a preset value outside its range     the parameter clamps it, and the
//                                        preset ships sounding like something
//                                        nobody designed
//   an id that changed                   every project saved before the
//                                        change silently loads a default
//   a choice saved as its index          inserting one entry in the middle of
//                                        a list repoints every patch anybody
//                                        ever saved at the next one along
//
// All of them are things you would eventually hear and never be able to
// explain, which is the class of bug this directory exists for. The one that
// cannot be tested from here, a field added to the patch and not to the
// table, is pinned by a static_assert on the size of the struct instead.
//
// And one check that is not about the table at all: every preset is played,
// and has to make a sound. A patch whose gate nobody opens, or whose
// amplitude is routed to nothing, loads without complaint and is silent, and
// a silent preset in a shipped bank is the most embarrassing kind of bug
// there is.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include "PanOneParams.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void fail (const std::string& what)
{
    std::printf ("  FAIL  %s\n", what.c_str());
    ++failures;
}

void check (bool ok, const std::string& what)
{
    if (! ok)
        fail (what);
}

std::string show (double v)
{
    char buffer[40];
    std::snprintf (buffer, sizeof (buffer), "%.4f", v);
    return buffer;
}

/** Float precision, not an opinion about audio: the patch holds doubles and a
    parameter holds a float, so a value that has been through one is equal to
    the original only to about seven digits. */
bool near (float a, float b)
{
    const float scale = std::max (1.0f, std::max (std::abs (a), std::abs (b)));
    return std::abs (a - b) <= 1.0e-6f * scale;
}

using Patch = PanOneParams::Patch;

//==============================================================================
void identifiers()
{
    std::printf ("identifiers\n");

    std::set<std::string> seen;

    for (const auto& spec : PanOneParams::all())
    {
        check (spec.id != nullptr && *spec.id != '\0',
               "a control has no id");
        check (spec.name != nullptr && *spec.name != '\0',
               std::string ("control ") + spec.id + " has no name");

        if (! seen.insert (spec.id).second)
            fail (std::string ("two controls share the id ") + spec.id);
    }

    std::printf ("    %zu controls, every id distinct\n", PanOneParams::count());

    // Every id a project written by version one could carry, and which still
    // means the same thing, has to still be here. A test is the only thing
    // that stops one of these being renamed for tidiness.
    for (const char* kept : { "osc1level", "osc2level", "sublevel", "detune",
                              "cutoff", "resonance",
                              "ampattack", "ampdecay", "ampsustain", "amprelease",
                              "filterattack", "filterdecay", "filtersustain",
                              "filterrelease" })
    {
        bool found = false;

        for (const auto& spec : PanOneParams::all())
            if (std::string (spec.id) == kept)
                found = true;

        check (found, std::string ("the id ") + kept
                        + " was in version one and means the same thing, so a project"
                          " that saved it has to get it back");
    }

    std::printf ("    every id version one saved and still means is present\n");
}

/** Every choice list is well formed, and its first entry is the one that is
    safe to fall back to. */
void choiceLists()
{
    std::printf ("choice lists\n");

    int lists = 0;

    for (const auto& spec : PanOneParams::all())
    {
        if (spec.choices == nullptr)
        {
            check (spec.kind == PanOneParams::Kind::continuous,
                   std::string (spec.id) + " is a choice with no list");
            continue;
        }

        ++lists;

        const auto& list = spec.choices();

        check (! list.empty(), std::string (spec.id) + "'s list is empty");

        // The range has to cover exactly the list, or the top or bottom entry
        // is unreachable from the control, which is the kind of fault nobody
        // finds until they go looking for the one waveform that will not
        // select.
        check (near (spec.minimum, 0.0f),
               std::string (spec.id) + " does not start at zero");
        check (near (spec.maximum, (float) (list.size() - 1)),
               std::string (spec.id) + " stops at " + show (spec.maximum)
                 + " where its list has " + std::to_string (list.size()) + " entries");

        std::set<std::string> ids, names;

        for (const auto& choice : list)
        {
            check (choice.id != nullptr && *choice.id != '\0',
                   std::string (spec.id) + " has an entry with no id");

            if (! ids.insert (choice.id).second)
                fail (std::string (spec.id) + " has two entries with the id "
                      + choice.id);

            if (! names.insert (choice.name).second)
                fail (std::string (spec.id) + " has two entries named " + choice.name);
        }

        // Round tripping an index through its id has to come back to itself,
        // which is the whole basis of saving a choice by name.
        for (int i = 0; i < (int) list.size(); ++i)
        {
            const char* id = PanOneParams::idOfChoice (list, i);

            if (PanOneParams::choiceFromId (list, id) != i)
                fail (std::string (spec.id) + " entry " + std::to_string (i)
                      + " does not come back from its own id");
        }

        // And an id nobody knows has to land on the first entry rather than
        // anywhere else, because the first entry of every list here is chosen
        // to be the one that is safe to arrive at.
        check (PanOneParams::choiceFromId (list, "no-such-id-anywhere") == -1,
               std::string (spec.id) + " claims to recognise an invented id");

        check (std::string (PanOneParams::idOfChoice (list, 9999))
                 == list.front().id,
               std::string (spec.id) + " does not fall back to its first entry");
    }

    std::printf ("    %d choice controls, every entry named, every id reversible\n", lists);

    // The two lists that come from somewhere else have to have "none" first,
    // because a routing that cannot be resolved has to be dropped and not
    // pointed at whatever happens to be first in the matrix.
    check (std::string (PanOneParams::modSources().front().id) == "none",
           "the source list does not start with none");
    check (std::string (PanOneParams::modDestinations().front().id) == "none",
           "the destination list does not start with none");

    std::printf ("    sources and destinations both start at none\n");
}

void ranges()
{
    std::printf ("ranges\n");

    for (const auto& spec : PanOneParams::all())
    {
        check (spec.minimum < spec.maximum,
               std::string (spec.id) + " has a minimum that is not below its maximum");

        if (spec.kind == PanOneParams::Kind::continuous)
            check (spec.centre > spec.minimum && spec.centre < spec.maximum,
                   std::string (spec.id) + " wants its centre at " + show (spec.centre)
                     + ", outside " + show (spec.minimum) + " to " + show (spec.maximum));
    }

    std::printf ("    every range is ordered and every centre is inside its own\n");
}

/** The groups cover the whole table, once each, in order.

    The editor lays itself out from them, so a group boundary that is wrong
    draws the wrong heading over the wrong knobs, and a gap between two
    groups is a control that exists, works, saves and loads, and is simply
    not on the panel anywhere. Both are silent.
*/
void theGroupsCoverEverything()
{
    std::printf ("groups\n");

    std::size_t expected = 0;
    std::size_t knobs = 0;

    for (const auto& group : PanOneParams::groups())
    {
        check (group.heading != nullptr && *group.heading != '\0',
               "a group has no heading");

        if (group.first != expected)
            fail (std::string ("the group ") + group.heading + " starts at "
                  + std::to_string (group.first) + " where the one before it ended at "
                  + std::to_string (expected));

        expected = group.first + group.count;

        if (group.kind == PanOneParams::Group::Kind::knobs)
            knobs += group.count;
    }

    if (expected != PanOneParams::count())
        fail ("the groups cover " + std::to_string (expected) + " controls of "
              + std::to_string (PanOneParams::count()));

    // Every control in the matrix group and no control outside it, which is
    // what the panel relies on when it leaves that group off.
    check (knobs == PanOneParams::firstRouteControl(),
           "the knob groups cover " + std::to_string (knobs)
             + " controls where the matrix starts at "
             + std::to_string (PanOneParams::firstRouteControl()));

    std::printf ("    %zu groups covering all %zu controls, %zu of them knobs\n",
                 PanOneParams::groups().size(), expected, knobs);
}

/** The important one. Setting a control has to change that control's field
    and nothing else. Two entries that write the same field, or a getter and
    a setter that disagree about which field they are about, both come out
    here and nowhere else. */
void eachControlIsItsOwn()
{
    std::printf ("each control is its own\n");

    const auto& specs = PanOneParams::all();

    Patch patch;
    const auto resting = PanOneParams::fromPatch (patch);

    for (std::size_t i = 0; i < specs.size(); ++i)
    {
        const auto& spec = specs[i];

        // Somewhere else in the range, chosen so that it really is a
        // different value whatever the control happens to be resting on.
        const float target = spec.kind == PanOneParams::Kind::choice
                               ? (resting[i] < 0.5f ? 1.0f : 0.0f)
                               : (std::abs (resting[i] - spec.minimum)
                                    > std::abs (resting[i] - spec.maximum)
                                      ? spec.minimum : spec.maximum);

        Patch moved;
        spec.set (moved, target);

        const auto after = PanOneParams::fromPatch (moved);

        if (near (after[i], resting[i]))
        {
            fail (std::string (spec.id) + " did not move when it was set to "
                  + show (target) + ": its getter and its setter are not about"
                    " the same field");
            continue;
        }

        for (std::size_t j = 0; j < specs.size(); ++j)
        {
            if (j == i)
                continue;

            if (! near (after[j], resting[j]))
            {
                fail (std::string ("setting ") + spec.id + " also moved " + specs[j].id
                      + ", from " + show (resting[j]) + " to " + show (after[j]));
                break;
            }
        }
    }

    std::printf ("    all %zu move only themselves\n", specs.size());
}

/** A patch written out through the table and read back has to be the same
    patch, which is what catches a field the table does not mention. */
void theRoundTripIsLossless()
{
    std::printf ("the round trip\n");

    std::mt19937 rng (20261013);
    std::uniform_real_distribution<float> unit (0.0f, 1.0f);

    const auto& specs = PanOneParams::all();

    for (int attempt = 0; attempt < 200; ++attempt)
    {
        std::vector<float> wanted;
        wanted.reserve (specs.size());

        for (const auto& spec : specs)
        {
            const float t = unit (rng);
            float value = spec.minimum + t * (spec.maximum - spec.minimum);

            if (spec.kind == PanOneParams::Kind::choice)
                value = (float) PanOneParams::toIndex (value,
                                                       (int) spec.choices().size());

            wanted.push_back (value);
        }

        Patch patch;
        PanOneParams::toPatch (wanted, patch);

        const auto readBack = PanOneParams::fromPatch (patch);

        for (std::size_t i = 0; i < specs.size(); ++i)
            if (! near (readBack[i], wanted[i]))
            {
                fail (std::string (specs[i].id) + " went in at " + show (wanted[i])
                      + " and came back as " + show (readBack[i]));
                return;
            }
    }

    std::printf ("    200 random patches survive going out and coming back\n");
}

/** The matrix's row count is how far the engine looks, not a control, and it
    has to be one past the last wired row: too small loses routings, too
    large costs every voice a check per row per sample to find nothing. */
void theRowCountIsWorkedOut()
{
    std::printf ("the matrix row count\n");

    const auto& specs = PanOneParams::all();

    const auto indexOf = [&] (const std::string& id)
    {
        for (std::size_t i = 0; i < specs.size(); ++i)
            if (id == specs[i].id)
                return i;

        fail ("no control called " + id);
        return (std::size_t) 0;
    };

    const int env1 = PanOneParams::choiceFromId (PanOneParams::modSources(), "env1");
    const int cutoff = PanOneParams::choiceFromId (PanOneParams::modDestinations(),
                                                    "cutoff");

    for (int lastWired : { -1, 0, 4, 17, Synth::ModMatrix::maxRoutes - 1 })
    {
        auto values = PanOneParams::fromPatch (Patch {});

        if (lastWired >= 0)
        {
            const std::string row = "mod" + std::to_string (lastWired + 1);
            values[indexOf (row + "src")] = (float) env1;
            values[indexOf (row + "dst")] = (float) cutoff;
        }

        Patch patch;
        PanOneParams::toPatch (values, patch);

        const int expected = lastWired + 1;

        if (patch.matrix.count != expected)
            fail ("with row " + std::to_string (lastWired + 1) + " wired the count is "
                  + std::to_string (patch.matrix.count) + " rather than "
                  + std::to_string (expected));
    }

    std::printf ("    the count follows the last wired row\n");
}

/** Every value in every preset is inside the range of the parameter that will
    hold it, because anything outside is quietly clamped when the preset is
    chosen and the preset then ships as a sound nobody designed. */
void everyPresetFitsItsControls()
{
    std::printf ("presets against their ranges\n");

    const auto& specs = PanOneParams::all();

    check (! PanOneParams::presets().empty(), "there are no presets");

    std::set<std::string> names;

    for (const auto& preset : PanOneParams::presets())
    {
        check (preset.name != nullptr && *preset.name != '\0', "a preset has no name");
        check (preset.category != nullptr && *preset.category != '\0',
               std::string ("preset ") + preset.name + " has no category");

        if (! names.insert (preset.name).second)
            fail (std::string ("two presets are called ") + preset.name);

        const auto values = PanOneParams::fromPatch (preset.patch);

        for (std::size_t i = 0; i < specs.size(); ++i)
        {
            const auto& spec = specs[i];

            if (values[i] < spec.minimum - 1e-6f || values[i] > spec.maximum + 1e-6f)
                fail (std::string (preset.name) + " sets " + spec.id + " to "
                      + show (values[i]) + ", outside " + show (spec.minimum) + " to "
                      + show (spec.maximum));
        }

        // And its routings have to point at things that exist. A routing that
        // does not is dropped at load, so a preset carrying one is a preset
        // that is quietly less than it looks.
        for (int r = 0; r < preset.patch.matrix.count; ++r)
        {
            const auto& route = preset.patch.matrix.routes[r];

            if (route.source < -1 || route.source >= Synth::ModMatrix::numSources())
                fail (std::string (preset.name) + " row " + std::to_string (r + 1)
                      + " has a source that does not exist");

            if (route.destination < -1
                  || route.destination >= Synth::ModMatrix::numDestinations())
                fail (std::string (preset.name) + " row " + std::to_string (r + 1)
                      + " has a destination that does not exist");

            if (route.via < -1 || route.via >= Synth::ModMatrix::numSources())
                fail (std::string (preset.name) + " row " + std::to_string (r + 1)
                      + " has a via that does not exist");
        }
    }

    std::printf ("    %zu presets, every value inside its range, every routing"
                 " resolvable\n", PanOneParams::presets().size());
}

/** And a preset survives being written out to the controls and read back,
    which is the path choosing one actually takes. */
void everyPresetSurvivesTheControls()
{
    std::printf ("presets through the controls\n");

    for (const auto& preset : PanOneParams::presets())
    {
        const auto values = PanOneParams::fromPatch (preset.patch);

        Patch rebuilt;
        PanOneParams::toPatch (values, rebuilt);

        const auto again = PanOneParams::fromPatch (rebuilt);

        for (std::size_t i = 0; i < values.size(); ++i)
            if (! near (again[i], values[i]))
            {
                fail (std::string (preset.name) + " does not survive the controls: "
                      + PanOneParams::all()[i].id + " went out at " + show (values[i])
                      + " and came back as " + show (again[i]));
                break;
            }
    }

    std::printf ("    every preset comes back the same\n");
}

//==============================================================================
/** The bank, built once. */
const Synth::WaveBank& sharedBank()
{
    static const Synth::WaveBank bank = []
    {
        Synth::WaveBank b;
        b.buildAll();
        return b;
    }();

    return bank;
}

/** Every preset has to make a sound.

    Not a check on the table at all, and the one most likely to catch a
    preset written by hand that does not work: a gate nobody opens, an
    amplitude routed to nothing, a fold mix at zero on a patch whose whole
    character is the fold, an oscillator left at no level. Every one of those
    loads without complaint and plays silence.
*/
void everyPresetMakesASound()
{
    std::printf ("presets, played\n");

    constexpr double rate = 48000.0;

    for (const auto& preset : PanOneParams::presets())
    {
        Synth::Voice voice;
        voice.setSampleRate (rate);
        voice.setBank (&sharedBank());
        voice.setPatch (preset.patch);

        // A low note, because several of these are drones and basses, and
        // held for long enough that a patch with a slow attack has arrived.
        voice.start (45, 0.9f);

        const int samples = (int) (rate * 3.0);

        std::vector<double> rendered;
        rendered.reserve ((std::size_t) samples);

        double peak = 0.0;

        for (int i = 0; i < samples; ++i)
        {
            const double s = (double) voice.nextSample();

            if (! std::isfinite (s))
            {
                fail (std::string (preset.name) + " produced something that is not a"
                                                  " number");
                break;
            }

            rendered.push_back (s);
            peak = std::max (peak, std::fabs (s));
        }

        // The loudest tenth of a second anywhere in the three, rather than
        // the level over some fixed stretch of it.
        //
        // That distinction is the whole measurement. A fixed stretch cannot
        // suit both kinds of patch here: the second half of three seconds
        // misses a struck sound entirely, because a gate with no sustain has
        // finished long before, and the first half misses a drone whose
        // attack is two and a half seconds. Measured over the second half,
        // five of these presets read as silent and none of them was.
        const int window = (int) (rate * 0.1);

        double level = 0.0;
        double running = 0.0;

        for (std::size_t i = 0; i < rendered.size(); ++i)
        {
            running += rendered[i] * rendered[i];

            if ((int) i >= window)
                running -= rendered[i - (std::size_t) window] * rendered[i - (std::size_t) window];

            if ((int) i >= window)
                level = std::max (level, std::sqrt (running / (double) window));
        }

        std::printf ("    %-18s %-12s loudest tenth %.4f  peak %.4f\n",
                     preset.name, preset.category, level, peak);

        // A thousandth of full scale, which is 60 dB down. Anything quieter
        // than that at its own loudest is not a quiet preset, it is a broken
        // one.
        // Two hundredths of full scale at its own loudest, which is 34 dB
        // down. Deliberately not lower: a preset can be quiet on purpose,
        // but one that is thirty decibels quieter than the rest of the bank
        // is almost always a patch with something wired wrong rather than a
        // patch somebody voiced that way. Both of the presets this caught
        // were faults, and one of them was the gate being held at a fixed
        // opening because the velocity was the only thing pointed at it.
        if (! (level > 0.02))
            fail (std::string (preset.name) + " is far quieter than a preset should"
                                              " be: its loudest tenth of a second is "
                  + show (level));

        // And not so loud that a chord cannot be summed. The instrument
        // divides by four to make room for its sixteen voices, so a voice
        // peaking at one and a half leaves room for about ten of them at
        // once before the sum reaches full scale, and anything hotter than
        // that is a preset that clips on a chord while sounding fine on one
        // note.
        if (! (peak < 1.5))
            fail (std::string (preset.name) + " peaks at " + show (peak)
                  + ", which will clip once more than a few notes are held");
    }
}

/** A project written by version one has to open as something musical.

    Four of its controls have no direct equivalent any more. Dropped, the
    project would open with the right envelope times and the wrong sound,
    which is the quiet kind of wrong this codebase keeps meeting, so they are
    translated instead and the translation is checked here.
*/
void aVersionOneProjectTranslates()
{
    std::printf ("a project from version one\n");

    // Nothing to do when there is nothing to translate.
    {
        Patch untouched;
        const auto before = PanOneParams::fromPatch (untouched);

        PanOneParams::Legacy nothing;
        PanOneParams::applyLegacy (nothing, untouched);

        const auto after = PanOneParams::fromPatch (untouched);

        for (std::size_t i = 0; i < before.size(); ++i)
            if (! near (before[i], after[i]))
            {
                fail ("a patch with nothing to translate was changed anyway, at "
                      + std::string (PanOneParams::all()[i].id));
                break;
            }
    }

    // Each of version one's four shapes has to arrive somewhere that sounds
    // like what it was called, which is a claim about the spectrum and not
    // about the numbers. A sine has one partial; a triangle has few; a saw
    // and a square have many, and the square has almost nothing even.
    const char* const called[4] { "sine", "saw", "square", "triangle" };

    for (int shape = 0; shape < 4; ++shape)
    {
        PanOneParams::Legacy old;
        old.present = true;
        old.osc1Shape = shape;
        old.osc2Shape = shape;
        old.pulseWidth = 0.5f;
        old.envOctaves = 0.0f;

        Patch patch;
        patch.osc1Level = 0.9;
        patch.osc2Level = 0.0;
        patch.gateMode = Synth::LowPassGate::Mode::amplitude;
        patch.gateRange = 0.0;
        patch.cutoff = 20000.0;
        patch.envelopes[0] = { 0.002, 0.050, 1.0, 0.100 };

        PanOneParams::applyLegacy (old, patch);

        // Nothing version one had could fold or drive, so a project from it
        // must not arrive with either in circuit.
        check (patch.foldMix == 0.0,
               std::string ("a ") + called[shape]
                 + " from version one arrived with the folder mixed in");
        check (patch.driveAmount == 0.0,
               std::string ("a ") + called[shape]
                 + " from version one arrived with the drive turned up");

        Synth::Voice voice;
        voice.setSampleRate (48000.0);
        voice.setBank (&sharedBank());
        voice.setPatch (patch);
        voice.start (45, 1.0f);

        std::vector<double> rendered;

        for (int i = 0; i < 8192; ++i)
            rendered.push_back ((double) voice.nextSample());

        const double hz = 110.0;

        const auto harmonic = [&] (int h)
        {
            double real = 0.0, imaginary = 0.0;

            for (std::size_t i = 0; i < rendered.size(); ++i)
            {
                const double angle = 2.0 * 3.14159265358979323846 * (double) h * hz
                                       * (double) i / 48000.0;

                real += rendered[i] * std::cos (angle);
                imaginary -= rendered[i] * std::sin (angle);
            }

            return std::sqrt (real * real + imaginary * imaginary)
                     / (double) rendered.size();
        };

        const double first = harmonic (1);
        const double second = harmonic (2);
        const double third = harmonic (3);

        std::printf ("    %-9s second harmonic %.3f of the first, third %.3f\n",
                     called[shape], second / first, third / first);

        check (first > 1e-4, std::string ("a ") + called[shape]
                               + " from version one makes almost no sound");

        if (shape == 0)     // a sine has nothing but its fundamental
            check (third / first < 0.02,
                   "a sine from version one arrived with harmonics on it: third at "
                     + show (third / first));

        if (shape == 2)     // a square at half width has no even harmonics
            check (second / first < 0.05,
                   "a square from version one arrived with even harmonics: second at "
                     + show (second / first));

        if (shape == 1)     // a saw has both, the second at about a half
            check (second / first > 0.2,
                   "a saw from version one has no even harmonics: second at "
                     + show (second / first));
    }

    // The width control, which only meant anything on a square, has to come
    // back as a narrower pulse rather than being ignored.
    {
        double wide = 0.0, narrow = 0.0;

        for (float width : { 0.5f, 0.08f })
        {
            PanOneParams::Legacy old;
            old.present = true;
            old.osc1Shape = 2;
            old.pulseWidth = width;

            Patch patch;
            PanOneParams::applyLegacy (old, patch);

            (width > 0.3f ? wide : narrow) = patch.osc1Position;
        }

        std::printf ("    a width of a half sits at %.3f along the pulse table,"
                     " 0.08 at %.3f\n", wide, narrow);

        check (wide < narrow,
               "a narrower pulse width did not move further along the pulse table: "
                 + show (wide) + " against " + show (narrow));

        check (wide < 0.05, "a width of exactly a half is not the start of the pulse"
                            " table: " + show (wide));
    }

    // And the filter envelope depth, which was a permanent wire from the
    // filter envelope to the cutoff and is now a routing like any other.
    {
        PanOneParams::Legacy old;
        old.present = true;
        old.envOctaves = 4.0f;

        Patch patch;
        PanOneParams::applyLegacy (old, patch);

        const int env2 = Synth::ModMatrix::sourceFromId ("env2");
        const int cutoff = Synth::ModMatrix::destinationFromId ("cutoff");

        bool found = false;

        for (int r = 0; r < patch.matrix.count; ++r)
        {
            const auto& route = patch.matrix.routes[r];

            if (route.source == env2 && route.destination == cutoff)
            {
                found = true;

                // The cutoff destination is worth eight octaves at full
                // depth, so four octaves is a depth of a half.
                check (near (route.depth, 0.5f),
                       "four octaves of filter envelope became a depth of "
                         + show (route.depth) + " where eight octaves is full depth");
            }
        }

        check (found, "a filter envelope depth from version one did not become a"
                      " routing from envelope two to the cutoff");

        // And nothing is wired when there was no depth to carry.
        PanOneParams::Legacy none;
        none.present = true;
        none.envOctaves = 0.0f;

        Patch bare;
        PanOneParams::applyLegacy (none, bare);

        check (bare.matrix.count == 0,
               "a project with no filter envelope depth arrived with a routing anyway");

        std::printf ("    four octaves of filter envelope became a routing at half"
                     " depth\n");
    }
}

/** A project stores the index of the preset it was on, and one saved before
    the controls existed stores nothing else. So the bank only ever grows at
    the end, and the first twenty two keep their places for good: a preset
    inserted among them would open every such project on its neighbour. */
void theFirstBankKeepsItsPlaces()
{
    std::printf ("preset places\n");

    const char* const first[]
    {
        "Deep Sub", "Wide Growl", "Squelch", "Reed Lead", "Hard Sync", "Soft Keys",
        "Bell Tine", "Slow Pad", "Glass Shelf", "Struck", "Tine Pluck", "Folded Drone",
        "Long Vigil", "Bowed Metal", "Static Field", "Shimmer Rattle", "Tom Strike",
        "Metal Hat", "Long Riser", "Siren Fold", "Audio Rate Slope", "Broken Machine"
    };

    const auto& bank = PanOneParams::presets();

    for (std::size_t i = 0; i < std::size (first); ++i)
        check (i < bank.size() && std::string (bank[i].name) == first[i],
               "preset " + std::to_string (i) + " should be " + first[i]
                   + ": the bank has to grow at the end");

    std::set<std::string> names;

    for (const auto& preset : bank)
        check (names.insert (preset.name).second,
               std::string ("two presets are called ") + preset.name);

    std::printf ("    %zu presets, the first %zu where they always were\n",
                 bank.size(), std::size (first));
}

/** From the second bank on, the macros mean the same on every patch: Tone,
    Movement, Grit and Shape, each resting at a half, and the mod wheel is
    vibrato. A convention nothing checks is a convention the next preset
    forgets, and a macro that does something different on one patch out of
    sixty is worse than no convention at all. */
void theSecondBankKeepsTheConventions()
{
    std::printf ("macro conventions\n");

    const auto& bank = PanOneParams::presets();
    const int macro[] { Synth::ModMatrix::sourceFromId ("macro1"),
                        Synth::ModMatrix::sourceFromId ("macro2"),
                        Synth::ModMatrix::sourceFromId ("macro3"),
                        Synth::ModMatrix::sourceFromId ("macro4") };
    const int wheel = Synth::ModMatrix::sourceFromId ("modwheel");
    const int cutoff = Synth::ModMatrix::destinationFromId ("cutoff");
    const int gateRange = Synth::ModMatrix::destinationFromId ("gaterange");
    const int drive = Synth::ModMatrix::destinationFromId ("drive");
    const int osc1Pos = Synth::ModMatrix::destinationFromId ("osc1pos");

    int checked = 0;

    for (std::size_t i = 22; i < bank.size(); ++i)
    {
        const auto& p = bank[i].patch;
        const std::string name = bank[i].name;

        bool tone = false, grit = false, shape = false, vibrato = false, movement = false;

        for (int r = 0; r < p.matrix.count; ++r)
        {
            const auto& route = p.matrix.routes[r];

            tone = tone || (route.source == macro[0]
                            && (route.destination == cutoff || route.destination == gateRange));
            grit = grit || (route.source == macro[2] && route.destination == drive);
            shape = shape || (route.source == macro[3] && route.destination == osc1Pos);
            vibrato = vibrato || route.via == wheel;
            movement = movement || route.via == macro[1] || route.source == macro[1];
        }

        for (int m = 0; m < 4; ++m)
            check (p.macros[m] == 0.5, name + "'s macro " + std::to_string (m + 1)
                                           + " does not rest at a half");

        check (tone, name + " has no Tone on macro 1");
        check (movement, name + " has no Movement on macro 2");
        check (grit, name + " has no Grit on macro 3");
        check (shape, name + " has no Shape on macro 4");
        check (vibrato, name + " has no vibrato on the mod wheel");
        ++checked;
    }

    std::printf ("    %d presets wire all four macros and the wheel\n", checked);
}

} // namespace

int main()
{
    std::printf ("PanOneParams\n\n");

    identifiers();
    choiceLists();
    ranges();
    theGroupsCoverEverything();
    eachControlIsItsOwn();
    theRoundTripIsLossless();
    theRowCountIsWorkedOut();
    everyPresetFitsItsControls();
    everyPresetSurvivesTheControls();
    everyPresetMakesASound();
    theFirstBankKeepsItsPlaces();
    theSecondBankKeepsTheConventions();
    aVersionOneProjectTranslates();

    std::printf ("\n");

    if (failures > 0)
    {
        std::printf ("%d check(s) failed.\n", failures);
        return 1;
    }

    std::printf ("All checks passed.\n");
    return 0;
}
