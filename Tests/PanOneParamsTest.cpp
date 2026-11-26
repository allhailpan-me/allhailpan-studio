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
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
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

        // Both turned up first, which is the whole point of the two checks
        // below. A fresh patch rests with the fold mixed out and the drive at
        // nothing, so asking a patch that has never had either whether it has
        // either is a check that cannot fail: deleting both lines from
        // applyLegacy passed it. The patch now arrives at the translation with
        // a folder in circuit and the drive up, the way a project loaded over
        // a patch somebody was already playing would.
        patch.foldMix = 0.8;
        patch.driveAmount = 20.0;

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
        {
            check (second / first < 0.05,
                   "a square from version one arrived with even harmonics: second at "
                     + show (second / first));

            // And it has to be a square and not a triangle, which is the other
            // waveform with no even harmonics at all. A square's third
            // harmonic is a third of its first and a triangle's is a ninth, so
            // the third is what tells them apart: it measures 0.34 against
            // 0.11. Without this a square from version one arriving on the
            // start of the analogue table, which is a triangle, passed.
            check (third / first > 0.2,
                   "a square from version one has a triangle's spectrum rather"
                   " than a square's: third at " + show (third / first));
        }

        if (shape == 3)     // a triangle falls away far faster than a square
            check (third / first < 0.2,
                   "a triangle from version one has a square's spectrum: third at "
                     + show (third / first));

        if (shape == 1)     // a saw has both, the second at about a half
            check (second / first > 0.2,
                   "a saw from version one has no even harmonics: second at "
                     + show (second / first));
    }

    // The two oscillators are translated from their own shapes.
    //
    // The loop above gives both the same shape and silences oscillator two, so
    // it cannot see oscillator two being handed oscillator one's shape. Each
    // pair is compared against the single shape case rather than against the
    // arithmetic again: where one shape on its own lands is what the spectrum
    // checks above have already established, so if a mixed pair puts each
    // oscillator where its own shape goes on its own, both are right.
    {
        const auto alone = [] (int shape)
        {
            PanOneParams::Legacy both;
            both.present = true;
            both.osc1Shape = shape;
            both.osc2Shape = shape;
            both.pulseWidth = 0.5f;

            Patch patch;
            PanOneParams::applyLegacy (both, patch);

            return patch;
        };

        for (int a = 0; a < 4; ++a)
            for (int b = 0; b < 4; ++b)
            {
                if (a == b)
                    continue;

                PanOneParams::Legacy mixed;
                mixed.present = true;
                mixed.osc1Shape = a;
                mixed.osc2Shape = b;
                mixed.pulseWidth = 0.5f;

                Patch patch;
                PanOneParams::applyLegacy (mixed, patch);

                const auto first1 = alone (a);
                const auto second2 = alone (b);

                check (patch.osc1Table == first1.osc1Table
                         && near ((float) patch.osc1Position, (float) first1.osc1Position),
                       std::string ("with a ") + called[a] + " and a " + called[b]
                         + ", oscillator one is not where a " + called[a]
                         + " goes on its own");

                check (patch.osc2Table == second2.osc2Table
                         && near ((float) patch.osc2Position, (float) second2.osc2Position),
                       std::string ("with a ") + called[a] + " and a " + called[b]
                         + ", oscillator two is not where a " + called[b]
                         + " goes on its own: it has been handed oscillator one's shape");
            }

        std::printf ("    all 12 mixed pairs of shapes land where each shape goes"
                     " on its own\n");
    }

    // A shape number version one never wrote, which is what a file from a
    // version that had more of them, or a corrupted one, would carry. The
    // documented fallback is the saw, because arriving at a plain waveform is
    // the least wrong answer and arriving nowhere leaves whatever the patch
    // happened to hold.
    {
        const auto sawLike = [] (int shape)
        {
            PanOneParams::Legacy old;
            old.present = true;
            old.osc1Shape = shape;
            old.osc2Shape = shape;
            old.pulseWidth = 0.5f;

            Patch patch;
            PanOneParams::applyLegacy (old, patch);

            return patch;
        };

        const auto saw = sawLike (1);

        for (int shape : { -1, 4, 97 })
        {
            const auto odd = sawLike (shape);

            check (odd.osc1Table == saw.osc1Table
                     && near ((float) odd.osc1Position, (float) saw.osc1Position),
                   "a shape of " + std::to_string (shape)
                     + " does not fall back to the saw");
        }

        std::printf ("    a shape version one never had falls back to the saw\n");
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

        // And a depth deeper than the destination's whole range is held to
        // full depth rather than being written past it. Version one's control
        // went to twelve octaves and the cutoff destination is worth eight, so
        // this is a value real projects carry, not an invented one.
        {
            PanOneParams::Legacy deep;
            deep.present = true;
            deep.envOctaves = 12.0f;

            Patch patch12;
            PanOneParams::applyLegacy (deep, patch12);

            bool wired = false;

            for (int r = 0; r < patch12.matrix.count; ++r)
                if (patch12.matrix.routes[r].source == env2
                      && patch12.matrix.routes[r].destination == cutoff)
                {
                    wired = true;

                    check (near (patch12.matrix.routes[r].depth, 1.0f),
                           "twelve octaves of filter envelope became a depth of "
                             + show (patch12.matrix.routes[r].depth)
                             + " rather than being held to full depth");
                }

            check (wired, "twelve octaves of filter envelope did not become a"
                          " routing at all");
        }

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

/** The id contract, checked against a string that is not the table's own.

    This is the premise of the whole file: a project stores the id of a choice
    so that reordering a list cannot silently repoint every patch anybody ever
    saved. The reversibility check above cannot establish it, and the reason is
    worth keeping. It does

        const char* id = PanOneParams::idOfChoice (list, i);
        choiceFromId (list, id)

    and `idOfChoice` hands back `list[i].id`, which is the table's own pointer.
    So `choiceFromId` comparing `list[i].id == id`, a pointer comparison, comes
    back to the same index and passes. It also correctly refuses an invented
    literal, which is a different pointer again.

    The real caller is not reversible that way. `PanOne::setStateInformation`
    calls `choiceFromId (list, saved.toRawUTF8())`, where `saved` is a
    juce::String read out of the project file, so the pointer is into that
    string's buffer and never into the table. Under a pointer comparison every
    choice in every saved project would come back unrecognised and land on the
    first entry of its list: every wavetable the first wavetable, every filter
    a low pass, every routing dropped, and every knob and every envelope time
    exactly right.

    So every lookup here goes through a copy of the characters, the way a
    project's does.
*/
void theIdsAreMatchedByTheirCharacters()
{
    std::printf ("ids matched by their characters\n");

    int looked = 0;

    for (const auto& spec : PanOneParams::all())
    {
        if (spec.choices == nullptr)
            continue;

        const auto& list = spec.choices();

        for (int i = 0; i < (int) list.size(); ++i)
        {
            // A copy, so the pointer handed over is nothing the table has
            // ever seen. Through a std::string rather than a char array so
            // that nothing can fold it back to the original literal.
            const std::string copy { list[(std::size_t) i].id };

            if (PanOneParams::choiceFromId (list, copy.c_str()) != i)
            {
                fail (std::string (spec.id) + " entry " + std::to_string (i)
                      + " (" + copy + ") is not found from a copy of its own id:"
                        " the lookup is comparing pointers, not characters");
                return;
            }

            ++looked;

            // And it is the whole id that has to match, not a start of it.
            // A prefix match would make a list containing both "lowpass" and
            // "lowpassq" resolve the longer one to the shorter.
            const std::string shorter = copy.substr (0, copy.size() - 1);
            const std::string longer  = copy + "x";

            const int fromShorter = PanOneParams::choiceFromId (list, shorter.c_str());
            const int fromLonger  = PanOneParams::choiceFromId (list, longer.c_str());

            // A shorter string may legitimately be another entry's id. What
            // it must not be is this one.
            if (fromShorter == i)
                fail (std::string (spec.id) + ": " + shorter + " resolved to "
                      + copy + ", so the match is on a prefix");

            if (fromLonger == i)
                fail (std::string (spec.id) + ": " + longer + " resolved to "
                      + copy + ", so the match ignores the tail");
        }
    }

    std::printf ("    %d ids found from a copy of their characters, none from a"
                 " prefix\n", looked);

    // The two guards on the conversion, each of which protects a case nothing
    // else here reaches. A null id is what a missing attribute would give, and
    // an empty string is what juce::String::toRawUTF8 gives for an empty one.
    const auto& list = PanOneParams::filterTypes();

    check (PanOneParams::choiceFromId (list, nullptr) == -1,
           "a null id is not refused");
    check (PanOneParams::choiceFromId (list, "") == -1,
           "an empty id is not refused");

    // idOfChoice's own out of range cases. An index exactly equal to the size
    // is the one the existing check does not try: it tries 9999, which is far
    // enough out that a comparison written as > rather than >= still catches
    // it, and reading list[size] is inside the vector's own allocation often
    // enough that it is not reliably a crash either.
    check (std::string (PanOneParams::idOfChoice (list, (int) list.size()))
             == list.front().id,
           "an index equal to the list size does not fall back to the first entry");
    check (std::string (PanOneParams::idOfChoice (list, -1)) == list.front().id,
           "an index of minus one does not fall back to the first entry");
    check (std::string (PanOneParams::idOfChoice (list, (int) list.size() - 1))
             == list.back().id,
           "the last index does not give the last entry");

    // And an empty list, which no list in the table is, so this is the only
    // place the guard is exercised at all.
    const std::vector<PanOneParams::Choice> nothing;

    check (std::string (PanOneParams::idOfChoice (nothing, 0)).empty(),
           "an empty list does not give an empty id");
    check (PanOneParams::choiceFromId (nothing, "anything") == -1,
           "an empty list claims to recognise an id");

    std::printf ("    a null id, an empty id, both ends of the range and an"
                 " empty list all behave\n");
}

/** What a choice control does with a value that is not already an exact index.

    `toIndex` exists to round a float onto a choice index, and the round trip
    check above cannot see whether it does: it builds its choice values by
    calling `toIndex` first, so every value it feeds back in is an exact
    integer, and rounding and truncating agree on all of those. A host hands
    over something else. A choice parameter is held as a normalised float and
    converted back through a NormalisableRange, so what arrives at the setter
    is a value like 2.9999998, and truncating there leaves the control one
    entry low.

    Both clamps are unexercised for the same reason, so they are included:
    a value past either end of the list has to land on that end, which also
    pins every call site's hand written count against its own list. The via
    column is the one that needs that: it clamps against modSources(), and
    clamping against modDestinations() instead is invisible from inside the
    range because the destination list is the longer of the two.
*/
void aChoiceRoundsToTheNearestEntry()
{
    std::printf ("choices round rather than truncate\n");

    const auto& specs = PanOneParams::all();

    int checked = 0;

    for (std::size_t c = 0; c < specs.size(); ++c)
    {
        const auto& spec = specs[c];

        if (spec.choices == nullptr)
            continue;

        const int size = (int) spec.choices().size();

        for (int i = 0; i < size; ++i)
        {
            // Just inside each half of the entry's own cell, plus the two
            // values a float conversion actually lands on either side of it.
            const float around[]
            {
                (float) i - 0.49f,
                std::nextafter ((float) i, -1.0f),
                (float) i,
                std::nextafter ((float) i, (float) size),
                (float) i + 0.49f
            };

            for (float value : around)
            {
                // The first and last entries have no cell on one side of
                // them, and the clamp is what is being checked there instead.
                const float wanted = value < 0.0f ? 0.0f
                                                  : (value > (float) (size - 1)
                                                       ? (float) (size - 1)
                                                       : (float) i);

                Patch patch;
                spec.set (patch, value);

                if (! near (spec.get (patch), wanted))
                {
                    fail (std::string (spec.id) + " set to " + show (value)
                          + " came back as " + show (spec.get (patch))
                          + " where entry " + std::to_string ((int) wanted)
                          + " was wanted: the value is being truncated rather"
                            " than rounded");
                    return;
                }
            }

            ++checked;
        }

        // Past both ends. These have to land on the ends, and nothing in the
        // table may store an index outside its own list.
        for (float value : { -1.0f, -0.6f, -1000.0f,
                             (float) size, (float) size + 0.6f, 1000.0f })
        {
            const float wanted = value < 0.0f ? 0.0f : (float) (size - 1);

            Patch patch;
            spec.set (patch, value);

            if (! near (spec.get (patch), wanted))
            {
                fail (std::string (spec.id) + " set to " + show (value)
                      + " came back as " + show (spec.get (patch))
                      + " where its list has " + std::to_string (size)
                      + " entries: the clamp is not against its own list");
                return;
            }
        }
    }

    std::printf ("    %d entries round from either side, and both clamps hold\n",
                 checked);
}

/** A synced LFO's beats per cycle back into a menu entry, including the
    values no entry is anywhere near.

    `divisionFromBeats` starts its search from index zero rather than from
    nothing, so that a value no entry is within a billion of still lands on a
    real entry. Nothing else here produces such a value, and under a search
    started from minus one the caller reads table[-1]. A NaN is the case to
    worry about in earnest: every comparison against it is false, so a search
    that relies on finding something finds nothing.
*/
void everyBeatsValueLandsOnARealDivision()
{
    std::printf ("divisions from beats\n");

    const auto& table = PanOneParams::divisionTable();

    // Every entry has to come back as itself, which is what makes saving a
    // division and loading it a round trip.
    for (int i = 0; i < (int) table.size(); ++i)
        check (PanOneParams::divisionFromBeats (table[(std::size_t) i].beats) == i,
               std::string ("the division ") + table[(std::size_t) i].name
                 + " does not come back from its own length in beats");

    // And a value that is not an entry lands on the nearest one rather than on
    // the first. Two per cent either side, which is well inside the smallest
    // gap in the table: the closest pair is a sixteenth triplet at a third of
    // a beat and a dotted sixteenth at 0.375, twelve per cent apart. Asserted
    // as a property rather than against hand picked numbers, because an exact
    // midpoint is a tie and a tie goes to whichever comes first in the table,
    // which is a menu order and not worth pinning.
    for (int i = 0; i < (int) table.size(); ++i)
        for (double nudge : { 0.98, 1.02 })
        {
            const double beats = table[(std::size_t) i].beats * nudge;
            const int found = PanOneParams::divisionFromBeats (beats);

            check (found == i,
                   std::string ("a cycle of ") + show (beats) + " beats resolved to "
                     + table[(std::size_t) found].name + " where "
                     + table[(std::size_t) i].name + " is nearer");
        }

    for (double beats : { std::nan (""), 1.0e300, -1.0e300,
                          std::numeric_limits<double>::infinity(),
                          -std::numeric_limits<double>::infinity() })
    {
        const int found = PanOneParams::divisionFromBeats (beats);

        check (found >= 0 && found < (int) table.size(),
               "a beats value nothing is near gave the index "
                 + std::to_string (found) + ", which is not in the table");
    }

    std::printf ("    all %zu round trip, and a NaN or an infinity still lands"
                 " in the table\n", table.size());
}

/** Writing a patch twice through the same object.

    `toPatch` resets the matrix row count before working it out, and every
    caller today hands it a patch straight off the stack, where the count is
    already nothing. So the reset guards the second write and nothing does a
    second write. It is pinned because the fault it prevents is one this
    project has already had: a row switched off that still counted as a
    routing silenced the voice, and that was found in the matrix itself on 24
    November.

    The short vector in the second half is the other guard in the same
    function: a vector of fewer values than the table has controls leaves the
    rest of the patch alone rather than reading past the end of it.
*/
void aPatchCanBeWrittenTwice()
{
    std::printf ("writing a patch twice\n");

    const auto& specs = PanOneParams::all();

    auto values = PanOneParams::fromPatch (Patch {});

    const std::size_t first = PanOneParams::firstRouteControl();

    const auto column = [&] (int row, int which) { return first + (std::size_t) (row * 6 + which); };

    // Four rows wired, then one, into the same patch.
    const auto wire = [&] (int rows)
    {
        for (int r = 0; r < 8; ++r)
        {
            const bool on = r < rows;

            values[column (r, 0)] = on ? 1.0f : 0.0f;   // source
            values[column (r, 1)] = on ? 1.0f : 0.0f;   // destination
            values[column (r, 2)] = on ? 0.5f : 0.0f;   // depth
            values[column (r, 5)] = on ? 1.0f : 0.0f;   // enabled
        }
    };

    Patch patch;

    wire (4);
    PanOneParams::toPatch (values, patch);
    check (patch.matrix.count == 4, "four wired rows gave a count of "
                                      + std::to_string (patch.matrix.count));

    wire (1);
    PanOneParams::toPatch (values, patch);
    check (patch.matrix.count == 1,
           "after four rows were switched off the count is still "
             + std::to_string (patch.matrix.count)
             + ": the row count is not being worked out from nothing");

    wire (0);
    PanOneParams::toPatch (values, patch);
    check (patch.matrix.count == 0,
           "with nothing wired the count is " + std::to_string (patch.matrix.count));

    // And fewer values than there are controls. Everything the vector reaches
    // is written and everything past it keeps what the patch already had.
    {
        auto full = PanOneParams::fromPatch (Patch {});

        for (std::size_t i = 0; i < full.size(); ++i)
        {
            const auto& spec = specs[i];

            full[i] = spec.kind == PanOneParams::Kind::choice
                        ? (near (full[i], 0.0f) ? 1.0f : 0.0f)
                        : (near (full[i], spec.minimum) ? spec.maximum : spec.minimum);
        }

        const std::size_t howMany = 20;
        const std::vector<float> few (full.begin(), full.begin() + (long) howMany);

        Patch written;
        PanOneParams::toPatch (few, written);

        const auto after = PanOneParams::fromPatch (written);
        const auto resting = PanOneParams::fromPatch (Patch {});

        for (std::size_t i = 0; i < howMany; ++i)
            check (near (after[i], full[i]),
                   std::string (specs[i].id) + " was inside the values handed over"
                     + " and did not take them");

        for (std::size_t i = howMany; i < specs.size(); ++i)
            if (specs[i].kind == PanOneParams::Kind::continuous
                  && ! near (after[i], resting[i]))
            {
                fail (std::string (specs[i].id) + " was past the end of the values"
                      + " handed over and moved anyway, from " + show (resting[i])
                      + " to " + show (after[i]));
                break;
            }

        std::printf ("    %zu values of %zu written, the rest left alone\n",
                     howMany, specs.size());
    }
}

/** Which group each control is laid out under.

    The group check above adds the counts up and compares the total, so moving
    one control from the end of a group to the start of the next one, which is
    what a wrong count is, leaves the total and the knob count exactly right
    and puts a control under the wrong heading on the panel. The first id of
    each group is pinned so that cannot happen quietly. This is the one check
    here about what somebody sees rather than what they hear, and it is cheap.
*/
void eachGroupStartsWhereItShould()
{
    std::printf ("group boundaries\n");

    const char* const starts[]
    {
        "osc1table", "folddrive", "filtertype", "ampattack", "env3attack",
        "slope1rise", "glide", "lfo1shape", "lfo3shape", "macro1", "mod1src"
    };

    const auto& all = PanOneParams::all();
    const auto& g = PanOneParams::groups();

    check (g.size() == std::size (starts),
           "there are " + std::to_string (g.size()) + " groups and "
             + std::to_string (std::size (starts)) + " pinned first controls");

    for (std::size_t i = 0; i < g.size() && i < std::size (starts); ++i)
    {
        check (g[i].first < all.size(), std::string (g[i].heading)
                                          + " starts past the end of the table");

        if (g[i].first < all.size())
            check (std::string (all[g[i].first].id) == starts[i],
                   std::string (g[i].heading) + " starts at "
                     + all[g[i].first].id + " where it should start at "
                     + starts[i]);
    }

    std::printf ("    all %zu groups start on the control they should\n", g.size());
}

/** The rule that makes the documented fallback true, which nothing stated.

    The header says an id nobody recognises falls back to the first entry of
    its list. `choiceFromId` does not do that: it returns -1, as its own doc
    comment says. What turns that into the first entry is the real caller,
    `PanOne::setStateInformation`:

        const int found = choiceFromId (list, saved.toRawUTF8());
        setParameterValue (i, (float) (found >= 0
                                         ? found
                                         : juce::jlimit (0, (int) list.size() - 1,
                                                         saved.getIntValue())));

    which reads an unrecognised id as a number, because version one wrote
    numbers in these attributes and a project from it has to keep working.
    `juce::String::getIntValue` on text that is not a number gives zero, which
    is the first entry, which is the documented fallback.

    So the fallback holds only for ids that a number parser reads as zero. An
    id beginning with a digit lands on that digit instead: were "8bars" ever
    to stop being recognised, it would arrive at entry 8 of its list rather
    than at entry 0. That is not a fault today, and it is a rule nothing
    enforced.

    The division list is the one that breaks it, with "1bar" through "8bars",
    and it is the one list whose own comment declares it exempt: there is no
    division that is safe to fall back to, so arriving at eight bars is no
    more or less wrong than arriving at a sixteenth. Every other list is
    checked, because the lists where the rule earns its keep are exactly the
    ones that genuinely change: the wavetables and the matrix.
*/
void anUnknownIdCanOnlyLandOnTheFirstEntry()
{
    std::printf ("the fallback is reachable\n");

    const auto& divisions = PanOneParams::lfoDivisions();

    const auto isADivisionList = [&] (const std::vector<PanOneParams::Choice>& list)
    {
        return list.size() == divisions.size()
                 && ! list.empty()
                 && std::string (list.front().id) == divisions.front().id;
    };

    std::set<std::string> checked;
    int exempt = 0;

    for (const auto& spec : PanOneParams::all())
    {
        if (spec.choices == nullptr)
            continue;

        const auto& list = spec.choices();

        if (isADivisionList (list))
        {
            ++exempt;
            continue;
        }

        for (const auto& choice : list)
        {
            const std::string id = choice.id;

            if (id.empty())
                continue;

            // What a number parser makes of the first character is the whole
            // of it: a digit is read as a value, and a leading sign is how a
            // negative one starts.
            const bool reads = std::isdigit ((unsigned char) id[0]) != 0
                                 || id[0] == '-' || id[0] == '+';

            if (reads)
                fail (std::string (spec.id) + "'s id " + id
                      + " begins with a character a number parser reads, so an"
                        " unrecognised id would land on that number rather than"
                        " on the first entry of its list");

            checked.insert (id);
        }
    }

    std::printf ("    %zu ids across every list but the divisions read as the"
                 " first entry, %d division controls exempt\n",
                 checked.size(), exempt);
}

/** Every choice list that is cast to something has to be in that thing's own
    order, and the tie is checked by name at both ends.

    This is the gap a fresh batch of mutations found, and it is one fault in
    three places. A choice is stored in the patch as an index, and for five of
    these lists that index is cast straight to an enum declared in another
    header; for one it is an index into another table in this one; for one it
    is a bool. Nothing anywhere said the orders had to agree, and nothing could
    see it when they did not, because every check of a choice goes out and
    comes back through the same convention and so cannot tell a consistent
    mistake from a correct answer.

    Reversing `driveCurves` passed the whole suite. Choosing "Soft" would then
    give the hard clip, and a patch saved on the soft curve would reload on the
    hard one: an instrument that gets brighter and more distorted every time
    somebody opens their project, with the knob still reading Soft. Reversing
    `offOrOn` passed too, which inverts every switch in the instrument.
    Reversing `lfoDivisions` against the table it indexes passed, and that
    saves a synced LFO's rate as the mirror of what it is set to, so a 1/32
    comes back as eight bars.

    So each pair is pinned by the names at both ends rather than by its order,
    which is what lets either side be reordered deliberately: moving an entry
    means moving its line here too, and that is a line somebody has to read.
*/
void everyChoiceListAgreesWithWhatItIsCastTo()
{
    std::printf ("the lists agree with what they are cast to\n");

    const auto at = [] (const std::vector<PanOneParams::Choice>& list, const char* id)
    {
        return PanOneParams::choiceFromId (list, std::string (id).c_str());
    };

    const auto pin = [&] (const char* what, const std::vector<PanOneParams::Choice>& list,
                          const char* id, int wanted)
    {
        check (at (list, id) == wanted,
               std::string (what) + ": " + id + " sits at entry "
                 + std::to_string (at (list, id)) + " of its list where the value it"
                   " is cast to is " + std::to_string (wanted));
    };

    // The filter's responses against Fx::Svf::Type. The eleventh value of that
    // enum, the matched bell, is deliberately not offered, which is why the
    // list is ten long and the clamp beside it is ten.
    {
        using T = Fx::Svf::Type;
        const auto& list = PanOneParams::filterTypes();

        pin ("filter", list, "lowpass",   (int) T::lowpass);
        pin ("filter", list, "highpass",  (int) T::highpass);
        pin ("filter", list, "bandpass",  (int) T::bandpass);
        pin ("filter", list, "bandpassq", (int) T::bandpassQ);
        pin ("filter", list, "notch",     (int) T::notch);
        pin ("filter", list, "peak",      (int) T::peak);
        pin ("filter", list, "allpass",   (int) T::allpass);
        pin ("filter", list, "bell",      (int) T::bell);
        pin ("filter", list, "lowshelf",  (int) T::lowShelf);
        pin ("filter", list, "highshelf", (int) T::highShelf);
    }

    {
        using C = Fx::Saturator::Curve;
        const auto& list = PanOneParams::driveCurves();

        pin ("drive", list, "soft",  (int) C::soft);
        pin ("drive", list, "cubic", (int) C::cubic);
        pin ("drive", list, "hard",  (int) C::hard);
    }

    {
        using M = Synth::LowPassGate::Mode;
        const auto& list = PanOneParams::gateModes();

        pin ("gate", list, "both",      (int) M::both);
        pin ("gate", list, "lowpass",   (int) M::lowpass);
        pin ("gate", list, "amplitude", (int) M::amplitude);
    }

    {
        using S = Synth::Lfo::Shape;
        const auto& list = PanOneParams::lfoShapes();

        pin ("LFO", list, "sine",     (int) S::sine);
        pin ("LFO", list, "triangle", (int) S::triangle);
        pin ("LFO", list, "sawup",    (int) S::sawUp);
        pin ("LFO", list, "sawdown",  (int) S::sawDown);
        pin ("LFO", list, "square",   (int) S::square);
        pin ("LFO", list, "pulse",    (int) S::pulse);
        pin ("LFO", list, "sandh",    (int) S::sampleAndHold);
        pin ("LFO", list, "random",   (int) S::smoothRandom);
    }

    {
        using S = Synth::ModMatrix::Shape;
        const auto& list = PanOneParams::modShapes();

        pin ("routing", list, "asis",     (int) S::asIs);
        pin ("routing", list, "unipolar", (int) S::unipolar);
        pin ("routing", list, "bipolar",  (int) S::bipolar);
        pin ("routing", list, "inverted", (int) S::inverted);
    }

    // A switch is a bool, so entry zero has to be the false one: the setters
    // read `v > 0.5f`.
    {
        const auto& list = PanOneParams::offOrOn();

        pin ("switch", list, "off", 0);
        pin ("switch", list, "on",  1);

        // And through a real control, so the convention is tied to a field and
        // not only to the list.
        for (const auto& spec : PanOneParams::all())
        {
            if (spec.choices != &PanOneParams::offOrOn)
                continue;

            Patch off, on;
            spec.set (off, (float) at (list, "off"));
            spec.set (on,  (float) at (list, "on"));

            check (near (spec.get (off), 0.0f) && near (spec.get (on), 1.0f),
                   std::string (spec.id) + " does not read as off when it is set"
                     " to off and on when it is set to on");
        }
    }

    // The synced LFO's menu against the table of lengths it indexes. The
    // control stores the beats from divisionTable()[index] and the project
    // saves lfoDivisions()[index].id, so the two orders being different means
    // a division is saved as a different division. Nothing in the patch can
    // see it, because both directions go through divisionFromBeats.
    {
        const auto& menu = PanOneParams::lfoDivisions();
        const auto& table = PanOneParams::divisionTable();

        check (menu.size() == table.size(),
               "the division menu has " + std::to_string (menu.size())
                 + " entries and the table of lengths has "
                 + std::to_string (table.size()));

        for (std::size_t i = 0; i < menu.size() && i < table.size(); ++i)
            check (std::string (menu[i].id) == table[i].id,
                   "division entry " + std::to_string (i) + " is "
                     + menu[i].id + " in the menu and " + table[i].id
                     + " in the table of lengths");
    }

    // And the two lists built from the matrix, where the index is the matrix's
    // own index plus one because "none" is in front.
    {
        const auto& sources = PanOneParams::modSources();

        for (std::size_t i = 0; i < Synth::ModMatrix::sources().size(); ++i)
            check (i + 1 < sources.size()
                     && std::string (sources[i + 1].id) == Synth::ModMatrix::sources()[i].id,
                   "source entry " + std::to_string (i + 1)
                     + " is not the matrix's source " + std::to_string (i));

        const auto& destinations = PanOneParams::modDestinations();

        for (std::size_t i = 0; i < Synth::ModMatrix::destinations().size(); ++i)
            check (i + 1 < destinations.size()
                     && std::string (destinations[i + 1].id)
                          == Synth::ModMatrix::destinations()[i].id,
                   "destination entry " + std::to_string (i + 1)
                     + " is not the matrix's destination " + std::to_string (i));

        const auto& tables = PanOneParams::waveTables();

        check (tables.size() == Synth::WaveBank::catalogue().size(),
               "the wavetable menu and the catalogue are different lengths");

        for (std::size_t i = 0; i < tables.size()
                                  && i < Synth::WaveBank::catalogue().size(); ++i)
            check (std::string (tables[i].id) == Synth::WaveBank::catalogue()[i].id,
                   "wavetable entry " + std::to_string (i) + " is " + tables[i].id
                     + " in the menu and " + Synth::WaveBank::catalogue()[i].id
                     + " in the catalogue");
    }

    std::printf ("    five enums, the switch, the divisions, the sources, the"
                 " destinations and the wavetables all line up\n");
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
    theIdsAreMatchedByTheirCharacters();
    aChoiceRoundsToTheNearestEntry();
    everyBeatsValueLandsOnARealDivision();
    anUnknownIdCanOnlyLandOnTheFirstEntry();
    everyChoiceListAgreesWithWhatItIsCastTo();
    ranges();
    theGroupsCoverEverything();
    eachControlIsItsOwn();
    theRoundTripIsLossless();
    theRowCountIsWorkedOut();
    aPatchCanBeWrittenTwice();
    eachGroupStartsWhereItShould();
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
