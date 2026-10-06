#pragma once
#include "SynthVoice.h"

#include <cstddef>
#include <vector>

//==============================================================================
/** Every knob PAN One has, as data rather than as code.

    This is a table and not a set of member functions for two reasons.

    The first is that the same list has to be walked in four places: building
    the parameters when the instrument is created, reading them back into a
    patch once per block, writing a preset out to them when one is chosen, and
    saving and loading them in a project. Four hand written lists is four
    chances for one of them to be missing an entry, and a missing entry is
    silent: the knob works, the preset sounds almost right, and nothing ever
    reports an error. A patch that is present, plausible and wrong is the
    hardest kind of bug to find by ear.

    The second is that a table can be tested. There is no JUCE in here, so a
    standalone test can check that writing a patch out through the table and
    reading it back gives the same patch, which is the one property that
    catches a field somebody forgot to list.

    Ranges are given as a minimum, a maximum, and where the middle of the
    control should land. The skew that produces is computed by JUCE from those
    three numbers, in NormalisableRange::setSkewForCentre, rather than being
    worked out here: a control for cutoff has to travel logarithmically or the
    bottom four octaves are crammed into the first few degrees of the knob,
    and the centre is the honest way to say so.
*/
namespace PanOneParams
{
    using Patch = SynthVoice::Patch;

    enum class Kind
    {
        continuous,   /**< a value anywhere in the range */
        choice        /**< one of a list, stored as its index */
    };

    struct Spec
    {
        const char* id;       /**< stable, saved in projects, never reused for anything else */
        const char* name;     /**< what a person reads */
        const char* unit;     /**< "" where the number speaks for itself */

        Kind  kind;
        float minimum;
        float maximum;
        float centre;         /**< ignored for a choice */

        float (*get) (const Patch&);
        void  (*set) (Patch&, float);
    };

    /** In the order Oscillator::Shape declares them, because a choice
        parameter stores its index and the index has to mean the same thing
        next time the project is opened. */
    inline const char* const shapeNames[] = { "Sine", "Saw", "Square", "Triangle" };
    inline constexpr int numShapes = 4;

    inline Oscillator::Shape toShape (float value) noexcept
    {
        const int index = value < 0.0f ? 0
                        : (value > (float) (numShapes - 1) ? numShapes - 1 : (int) (value + 0.5f));
        return (Oscillator::Shape) index;
    }

    /** The table. Adding a knob means adding a line here and nothing else,
        which is the point of it. */
    inline const std::vector<Spec>& all()
    {
        static const std::vector<Spec> specs
        {
            { "osc1shape", "Osc 1", "", Kind::choice, 0.0f, (float) numShapes - 1, 0.0f,
              [] (const Patch& p) { return (float) (int) p.osc1Shape; },
              [] (Patch& p, float v) { p.osc1Shape = toShape (v); } },

            { "osc2shape", "Osc 2", "", Kind::choice, 0.0f, (float) numShapes - 1, 0.0f,
              [] (const Patch& p) { return (float) (int) p.osc2Shape; },
              [] (Patch& p, float v) { p.osc2Shape = toShape (v); } },

            // Detune is in semitones so that it reads the same as it would on
            // any other instrument, but almost all of its useful travel is in
            // the first semitone, where it is a beating rather than a chord.
            { "detune", "Detune", " st", Kind::continuous, 0.0f, 12.0f, 0.5f,
              [] (const Patch& p) { return (float) p.osc2Detune; },
              [] (Patch& p, float v) { p.osc2Detune = v; } },

            { "pulsewidth", "Width", "", Kind::continuous, 0.05f, 0.95f, 0.5f,
              [] (const Patch& p) { return (float) p.pulseWidth; },
              [] (Patch& p, float v) { p.pulseWidth = v; } },

            { "osc1level", "Level 1", "", Kind::continuous, 0.0f, 1.0f, 0.5f,
              [] (const Patch& p) { return (float) p.osc1Level; },
              [] (Patch& p, float v) { p.osc1Level = v; } },

            { "osc2level", "Level 2", "", Kind::continuous, 0.0f, 1.0f, 0.5f,
              [] (const Patch& p) { return (float) p.osc2Level; },
              [] (Patch& p, float v) { p.osc2Level = v; } },

            { "sublevel", "Sub", "", Kind::continuous, 0.0f, 1.0f, 0.5f,
              [] (const Patch& p) { return (float) p.subLevel; },
              [] (Patch& p, float v) { p.subLevel = v; } },

            // A kilohertz in the middle of the travel, so the knob spends as
            // much of itself on the two octaves either side of where most
            // filtering happens as it does on the top three.
            { "cutoff", "Cutoff", " Hz", Kind::continuous, 20.0f, 20000.0f, 1000.0f,
              [] (const Patch& p) { return (float) p.cutoff; },
              [] (Patch& p, float v) { p.cutoff = v; } },

            // Filter::setResonance clamps to the half to twenty it can
            // actually hold, and this stops short of the top of that so the
            // knob does not end in a region where it is self oscillating.
            { "resonance", "Reso", "", Kind::continuous, 0.5f, 12.0f, 2.0f,
              [] (const Patch& p) { return (float) p.resonance; },
              [] (Patch& p, float v) { p.resonance = v; } },

            { "envoctaves", "Env amount", " oct", Kind::continuous, 0.0f, 8.0f, 2.5f,
              [] (const Patch& p) { return (float) p.envOctaves; },
              [] (Patch& p, float v) { p.envOctaves = v; } },

            // Both envelopes get the same ranges. Times are skewed hard
            // towards the short end because that is where the difference
            // between a pluck and a stab lives, and a linear control there
            // would put every percussive setting in the first millimetre.
            { "ampattack", "A", " s", Kind::continuous, 0.0005f, 5.0f, 0.05f,
              [] (const Patch& p) { return (float) p.amp.attackSeconds; },
              [] (Patch& p, float v) { p.amp.attackSeconds = v; } },

            { "ampdecay", "D", " s", Kind::continuous, 0.001f, 8.0f, 0.200f,
              [] (const Patch& p) { return (float) p.amp.decaySeconds; },
              [] (Patch& p, float v) { p.amp.decaySeconds = v; } },

            { "ampsustain", "S", "", Kind::continuous, 0.0f, 1.0f, 0.5f,
              [] (const Patch& p) { return (float) p.amp.sustainLevel; },
              [] (Patch& p, float v) { p.amp.sustainLevel = v; } },

            { "amprelease", "R", " s", Kind::continuous, 0.001f, 10.0f, 0.300f,
              [] (const Patch& p) { return (float) p.amp.releaseSeconds; },
              [] (Patch& p, float v) { p.amp.releaseSeconds = v; } },

            { "filterattack", "A", " s", Kind::continuous, 0.0005f, 5.0f, 0.05f,
              [] (const Patch& p) { return (float) p.filter.attackSeconds; },
              [] (Patch& p, float v) { p.filter.attackSeconds = v; } },

            { "filterdecay", "D", " s", Kind::continuous, 0.001f, 8.0f, 0.200f,
              [] (const Patch& p) { return (float) p.filter.decaySeconds; },
              [] (Patch& p, float v) { p.filter.decaySeconds = v; } },

            { "filtersustain", "S", "", Kind::continuous, 0.0f, 1.0f, 0.5f,
              [] (const Patch& p) { return (float) p.filter.sustainLevel; },
              [] (Patch& p, float v) { p.filter.sustainLevel = v; } },

            { "filterrelease", "R", " s", Kind::continuous, 0.001f, 10.0f, 0.300f,
              [] (const Patch& p) { return (float) p.filter.releaseSeconds; },
              [] (Patch& p, float v) { p.filter.releaseSeconds = v; } },
        };

        return specs;
    }

    inline std::size_t count() { return all().size(); }

    // A field added to SynthVoice::Patch and not added to the table above is
    // the one failure this cannot test for: the knob simply is not there, the
    // presets still load, and nothing reports anything. So the size of the
    // struct is pinned, and growing it is a compile error that says to come
    // and look here. Update the number once the new field has a line in the
    // table, or a comment saying why it deliberately has no knob.
    static_assert (sizeof (Patch) == 136,
                   "SynthVoice::Patch has changed. Does the new field need a line in "
                   "PanOneParams::all()? If not, say why here and update the size.");

    //==========================================================================
    /** The presets, as data, for the same reason the knobs are: a test can
        then check that every value in every preset is inside the range of the
        parameter that will hold it. A preset value outside its range is
        quietly clamped, so the preset ships sounding like something nobody
        designed, which is exactly the failure that is impossible to notice by
        reading the code. */
    struct Preset
    {
        const char* name;
        Patch patch;
    };

    inline const std::vector<Preset>& presets()
    {
        static const std::vector<Preset> list = []
        {
            using Shape = Oscillator::Shape;

            Patch bass;
            bass.osc1Shape = Shape::saw;   bass.osc2Shape = Shape::square;
            bass.osc2Detune = 0.04;        bass.subLevel = 0.45;
            bass.cutoff = 240.0;           bass.resonance = 2.2;  bass.envOctaves = 2.6;
            bass.amp    = { 0.002, 0.200, 0.55, 0.120 };
            bass.filter = { 0.001, 0.140, 0.18, 0.120 };

            Patch keys;
            keys.osc1Shape = Shape::saw;   keys.osc2Shape = Shape::saw;
            keys.osc2Detune = 0.11;        keys.subLevel = 0.18;
            keys.cutoff = 1100.0;          keys.resonance = 1.1;  keys.envOctaves = 2.2;
            keys.amp    = { 0.010, 0.400, 0.70, 0.350 };
            keys.filter = { 0.020, 0.500, 0.40, 0.300 };

            Patch pad;
            pad.osc1Shape = Shape::saw;    pad.osc2Shape = Shape::triangle;
            pad.osc2Detune = 0.19;         pad.subLevel = 0.22;
            pad.cutoff = 500.0;            pad.resonance = 0.9;   pad.envOctaves = 2.8;
            pad.amp    = { 0.450, 1.000, 0.85, 0.900 };
            pad.filter = { 0.700, 1.400, 0.55, 0.800 };

            Patch pluck;
            pluck.osc1Shape = Shape::square; pluck.osc2Shape = Shape::saw;
            pluck.osc2Detune = 0.07;         pluck.subLevel = 0.12;
            pluck.pulseWidth = 0.32;
            pluck.cutoff = 420.0;            pluck.resonance = 3.4; pluck.envOctaves = 4.0;
            pluck.amp    = { 0.001, 0.260, 0.00, 0.180 };
            pluck.filter = { 0.001, 0.160, 0.00, 0.150 };

            Patch lead;
            lead.osc1Shape = Shape::square; lead.osc2Shape = Shape::saw;
            lead.osc2Detune = 0.05;         lead.subLevel = 0.30;
            lead.pulseWidth = 0.42;
            lead.cutoff = 900.0;            lead.resonance = 2.8;  lead.envOctaves = 3.2;
            lead.amp    = { 0.004, 0.300, 0.80, 0.160 };
            lead.filter = { 0.003, 0.240, 0.45, 0.180 };

            return std::vector<Preset> { { "Sub Bass",    bass },
                                         { "Soft Keys",   keys },
                                         { "Slow Pad",    pad },
                                         { "Short Pluck", pluck },
                                         { "Reed Lead",   lead } };
        }();

        return list;
    }

    /** Where each group of knobs starts, for laying the editor out and for
        nothing else. Kept here so that reordering the table cannot leave the
        editor drawing the wrong headings over the wrong knobs. */
    struct Group
    {
        const char* heading;
        std::size_t first, count;
    };

    inline const std::vector<Group>& groups()
    {
        static const std::vector<Group> g
        {
            { "Oscillators",      0, 7 },
            { "Filter",           7, 3 },
            { "Amplitude",       10, 4 },
            { "Filter envelope", 14, 4 },
        };

        return g;
    }

    /** Reads a patch out into one float per parameter. */
    inline std::vector<float> fromPatch (const Patch& patch)
    {
        std::vector<float> values;
        values.reserve (count());

        for (const auto& spec : all())
            values.push_back (spec.get (patch));

        return values;
    }

    /** And writes them back. Anything the table does not mention keeps
        whatever the patch already had, which is why the test checks that the
        table mentions everything. */
    inline void toPatch (const std::vector<float>& values, Patch& patch)
    {
        const auto& specs = all();

        for (std::size_t i = 0; i < specs.size() && i < values.size(); ++i)
            specs[i].set (patch, values[i]);
    }
}
