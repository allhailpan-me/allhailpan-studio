#pragma once
#include "Synth/Voice.h"

#include <cmath>
#include <cstddef>
#include <deque>
#include <functional>
#include <string>
#include <vector>

//==============================================================================
/** Every control PAN One has, as data rather than as code.

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

    **The modulation matrix is in the table too, all thirty two rows of it.**
    That is a decision worth stating, because the alternative looks tidier. A
    routing's source and destination are of no use as automation lanes, and a
    host showing "Mod 7 destination" is noise. But keeping them out of the
    table would mean a second mechanism for saving them, restoring them,
    writing them when a preset is chosen and handing them to the audio thread,
    and the whole argument for the table is that one mechanism cannot be half
    implemented. A preset that moved every knob and left the previous patch's
    wiring in place is exactly the kind of fault this file exists to prevent,
    and it is what a split would invite.

    **Every choice is saved by a stable string id, never by its index.** This
    is the rule `Synth/ModMatrix.h` is built on and the reason is the same: a
    choice parameter holds an index, and if an index were what a project
    stored, inserting one wavetable or one destination in the middle of a list
    would leave every patch anybody had ever saved pointing at the next one
    along. The patch would still load and still play. So `Choice` carries an
    id beside the name, `idOfChoice` and `choiceFromId` convert, and a
    project writes the id.

    An id nobody recognises falls back to the **first** entry of its list, and
    every list here is ordered so that its first entry is the one that is safe
    to arrive at: "none" for a source or a destination, which drops the
    routing the way the matrix says an unresolvable routing should be dropped,
    and the plainest setting for everything else. Losing a routing is a patch
    that is less than it was; pointing it somewhere else is a patch that is
    wrong, and the second is much worse because nothing about it looks broken.
*/
namespace PanOneParams
{
    using Patch = Synth::Voice::Patch;
    using Route = Synth::ModMatrix::Route;

    enum class Kind
    {
        continuous,   /**< a value anywhere in the range */
        choice        /**< one of a list, stored as its index, saved as its id */
    };

    /** One entry of a choice control.

        The id is what a project stores and must never be reused for anything
        else. The name is what a person reads and can change freely.
    */
    struct Choice
    {
        const char* id;
        const char* name;
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

        /** The list a choice draws from, or null for a continuous control.
            A function rather than a reference so that the lists which are
            built from other tables, the wavetables and the matrix, are built
            on first use rather than at static initialisation time, where the
            order between translation units is not decided. */
        const std::vector<Choice>& (*choices)();

        std::function<float (const Patch&)> get;
        std::function<void (Patch&, float)> set;
    };

    //==========================================================================
    /** Somewhere stable to keep a generated name.

        The matrix rows are generated rather than written out, because
        thirty two rows of six controls is a hundred and ninety two entries
        and a hand written list of those is exactly the kind of thing one
        line of which would be wrong. Their ids and names therefore have to
        be built at run time and then live as long as the table does.

        A deque and not a vector: a vector moves its contents when it grows,
        which would leave every `const char*` already handed out pointing at
        freed memory. That would be a dangling pointer in a table the whole
        instrument reads, and it would work perfectly until the table grew
        past its first allocation.
    */
    inline const char* keep (std::string text)
    {
        static std::deque<std::string> held;
        held.push_back (std::move (text));
        return held.back().c_str();
    }

    //==========================================================================
    // The choice lists. Each one's first entry is what an unrecognised id
    // falls back to, so each is ordered with the safe answer first.

    inline const std::vector<Choice>& waveTables()
    {
        static const std::vector<Choice> list = []
        {
            std::vector<Choice> out;

            for (const auto& entry : Synth::WaveBank::catalogue())
                out.push_back ({ entry.id, entry.name });

            return out;
        }();

        return list;
    }

    /** The matrix's sources, with "none" first so that index zero is an empty
        row and an unknown id lands on it. */
    inline const std::vector<Choice>& modSources()
    {
        static const std::vector<Choice> list = []
        {
            std::vector<Choice> out { { "none", "None" } };

            for (const auto& entry : Synth::ModMatrix::sources())
                out.push_back ({ entry.id, entry.name });

            return out;
        }();

        return list;
    }

    inline const std::vector<Choice>& modDestinations()
    {
        static const std::vector<Choice> list = []
        {
            std::vector<Choice> out { { "none", "None" } };

            for (const auto& entry : Synth::ModMatrix::destinations())
                out.push_back ({ entry.id, entry.name });

            return out;
        }();

        return list;
    }

    inline const std::vector<Choice>& modShapes()
    {
        static const std::vector<Choice> list
        {
            { "asis",     "As is" },
            { "unipolar", "Unipolar" },
            { "bipolar",  "Bipolar" },
            { "inverted", "Inverted" }
        };

        return list;
    }

    inline const std::vector<Choice>& offOrOn()
    {
        static const std::vector<Choice> list { { "off", "Off" }, { "on", "On" } };
        return list;
    }

    /** The filter's responses. Lowpass first: it is both the commonest and
        the one that is safe to arrive at, since a response nobody recognises
        becoming a lowpass is a patch that is darker than it was rather than
        one that is suddenly a notch. */
    inline const std::vector<Choice>& filterTypes()
    {
        static const std::vector<Choice> list
        {
            { "lowpass",   "Low pass" },
            { "highpass",  "High pass" },
            { "bandpass",  "Band pass" },
            { "bandpassq", "Band pass Q" },
            { "notch",     "Notch" },
            { "peak",      "Peak" },
            { "allpass",   "All pass" },
            { "bell",      "Bell" },
            { "lowshelf",  "Low shelf" },
            { "highshelf", "High shelf" }
        };

        return list;
    }

    inline const std::vector<Choice>& driveCurves()
    {
        static const std::vector<Choice> list
        {
            { "soft",  "Soft" },
            { "cubic", "Cubic" },
            { "hard",  "Hard" }
        };

        return list;
    }

    inline const std::vector<Choice>& lfoShapes()
    {
        static const std::vector<Choice> list
        {
            { "sine",     "Sine" },
            { "triangle", "Triangle" },
            { "sawup",    "Saw up" },
            { "sawdown",  "Saw down" },
            { "square",   "Square" },
            { "pulse",    "Pulse" },
            { "sandh",    "Sample and hold" },
            { "random",   "Smooth random" }
        };

        return list;
    }

    /** How long one cycle of a synced LFO takes, in beats.

        Listed longest first, which is a menu order rather than a safety
        order, and that is a deliberate exception to the rule the other
        lists follow. There is no entry here that is safe to fall back to:
        if an id is not recognised then the file came from a version that
        had a division this one does not, and arriving at eight bars is no
        more or less wrong than arriving at a sixteenth. The rule earns its
        keep on the lists that genuinely change, the wavetables and the
        matrix, where "none" and "the first table" are real answers.
    */
    struct Division
    {
        const char* id;
        const char* name;
        double beats;       /**< a beat being a quarter note */
    };

    inline const std::vector<Division>& divisionTable()
    {
        static const std::vector<Division> list
        {
            { "8bars",  "8 bars",  32.0 },
            { "4bars",  "4 bars",  16.0 },
            { "2bars",  "2 bars",   8.0 },
            { "1bar",   "1 bar",    4.0 },
            { "half",   "1/2",      2.0 },
            { "halft",  "1/2 T",    4.0 / 3.0 },
            { "quartd", "1/4 D",    1.5 },
            { "quart",  "1/4",      1.0 },
            { "quartt", "1/4 T",    2.0 / 3.0 },
            { "eighd",  "1/8 D",    0.75 },
            { "eigh",   "1/8",      0.5 },
            { "eight",  "1/8 T",    1.0 / 3.0 },
            { "sixtd",  "1/16 D",   0.375 },
            { "sixt",   "1/16",     0.25 },
            { "sixtt",  "1/16 T",   1.0 / 6.0 },
            { "thirty", "1/32",     0.125 }
        };

        return list;
    }

    inline const std::vector<Choice>& lfoDivisions()
    {
        static const std::vector<Choice> list = []
        {
            std::vector<Choice> out;

            for (const auto& d : divisionTable())
                out.push_back ({ d.id, d.name });

            return out;
        }();

        return list;
    }

    /** The division nearest a number of beats.

        Nearest rather than exact, so that a patch carrying a value from
        somewhere else, or one that has been through a float parameter,
        lands on a real entry instead of on the first one.
    */
    inline int divisionFromBeats (double beats)
    {
        const auto& table = divisionTable();

        int best = 0;
        double closest = 1.0e9;

        for (int i = 0; i < (int) table.size(); ++i)
        {
            const double away = std::fabs (table[(std::size_t) i].beats - beats);

            if (away < closest)
            {
                closest = away;
                best = i;
            }
        }

        return best;
    }

    inline const std::vector<Choice>& gateModes()
    {
        static const std::vector<Choice> list
        {
            { "both",      "Both" },
            { "lowpass",   "Low pass" },
            { "amplitude", "Amplitude" }
        };

        return list;
    }

    //==========================================================================
    /** The index for an id, or -1 if the list has never heard of it. */
    inline int choiceFromId (const std::vector<Choice>& list, const char* id)
    {
        if (id == nullptr)
            return -1;

        for (std::size_t i = 0; i < list.size(); ++i)
            if (std::string (list[i].id) == id)
                return (int) i;

        return -1;
    }

    /** The id at an index, or the first entry's id for anything out of range,
        which is the fallback this file's comment describes. */
    inline const char* idOfChoice (const std::vector<Choice>& list, int index)
    {
        if (list.empty())
            return "";

        if (index < 0 || index >= (int) list.size())
            return list.front().id;

        return list[(std::size_t) index].id;
    }

    //==========================================================================
    /** Clamps a float to a choice list and hands back an index. */
    inline int toIndex (float value, int count)
    {
        const int index = (int) (value + 0.5f);
        return index < 0 ? 0 : (index >= count ? count - 1 : index);
    }

    template <typename Enum>
    Enum toEnum (float value, int count)
    {
        return (Enum) toIndex (value, count);
    }

    //==========================================================================
    /** The table. Adding a control means adding a line here and nothing else,
        which is the point of it. */
    inline const std::vector<Spec>& all()
    {
        static const std::vector<Spec> specs = []
        {
            std::vector<Spec> s;

            const auto continuous = [&s] (const char* id, const char* name,
                                          const char* unit,
                                          float low, float high, float centre,
                                          std::function<float (const Patch&)> get,
                                          std::function<void (Patch&, float)> set)
            {
                s.push_back ({ id, name, unit, Kind::continuous, low, high, centre,
                               nullptr, std::move (get), std::move (set) });
            };

            const auto pick = [&s] (const char* id, const char* name,
                                    const std::vector<Choice>& (*list)(),
                                    std::function<float (const Patch&)> get,
                                    std::function<void (Patch&, float)> set)
            {
                s.push_back ({ id, name, "", Kind::choice,
                               0.0f, (float) (list().size() - 1), 0.0f,
                               list, std::move (get), std::move (set) });
            };

            //------------------------------------------------------------------
            // Oscillators.

            pick ("osc1table", "Osc 1", waveTables,
                  [] (const Patch& p) { return (float) p.osc1Table; },
                  [] (Patch& p, float v) { p.osc1Table = toIndex (v, (int) waveTables().size()); });

            // Where along the row of waveforms. The whole point of a
            // wavetable is that this is a continuous control and not a
            // choice between shapes.
            continuous ("osc1pos", "Pos 1", "", 0.0f, 1.0f, 0.5f,
                        [] (const Patch& p) { return (float) p.osc1Position; },
                        [] (Patch& p, float v) { p.osc1Position = v; });

            continuous ("osc1level", "Level 1", "", 0.0f, 1.0f, 0.5f,
                        [] (const Patch& p) { return (float) p.osc1Level; },
                        [] (Patch& p, float v) { p.osc1Level = v; });

            pick ("osc2table", "Osc 2", waveTables,
                  [] (const Patch& p) { return (float) p.osc2Table; },
                  [] (Patch& p, float v) { p.osc2Table = toIndex (v, (int) waveTables().size()); });

            continuous ("osc2pos", "Pos 2", "", 0.0f, 1.0f, 0.5f,
                        [] (const Patch& p) { return (float) p.osc2Position; },
                        [] (Patch& p, float v) { p.osc2Position = v; });

            continuous ("osc2level", "Level 2", "", 0.0f, 1.0f, 0.5f,
                        [] (const Patch& p) { return (float) p.osc2Level; },
                        [] (Patch& p, float v) { p.osc2Level = v; });

            // Two octaves either way, so it covers both a detune of a few
            // cents and a deliberate interval. The centre is the middle of
            // the travel, so the control is linear in semitones: a skew here
            // would make the cents end usable and the interval end a lie,
            // since nobody wants a seventh to be hard to dial exactly.
            //
            // The id is the one version one used for a detune in semitones,
            // because it means the same thing and a project that saved it
            // should get it back.
            continuous ("detune", "Detune", " st", -24.0f, 24.0f, 0.0f,
                        [] (const Patch& p) { return (float) p.osc2Semitones; },
                        [] (Patch& p, float v) { p.osc2Semitones = v; });

            continuous ("fm", "FM", "", 0.0f, 8.0f, 1.0f,
                        [] (const Patch& p) { return (float) p.fmAmount; },
                        [] (Patch& p, float v) { p.fmAmount = v; });

            continuous ("sublevel", "Sub", "", 0.0f, 1.0f, 0.5f,
                        [] (const Patch& p) { return (float) p.subLevel; },
                        [] (Patch& p, float v) { p.subLevel = v; });

            continuous ("noiselevel", "Noise", "", 0.0f, 1.0f, 0.5f,
                        [] (const Patch& p) { return (float) p.noiseLevel; },
                        [] (Patch& p, float v) { p.noiseLevel = v; });

            //------------------------------------------------------------------
            // The folder.

            // One is the edge of folding at all, and the skew puts the first
            // few folds, which is where the character is, across most of the
            // travel rather than in the first tenth of it.
            continuous ("folddrive", "Fold", "", 0.0f, 64.0f, 4.0f,
                        [] (const Patch& p) { return (float) p.foldDrive; },
                        [] (Patch& p, float v) { p.foldDrive = v; });

            continuous ("foldsym", "Symmetry", "", -1.0f, 1.0f, 0.0f,
                        [] (const Patch& p) { return (float) p.foldSymmetry; },
                        [] (Patch& p, float v) { p.foldSymmetry = v; });

            continuous ("foldmix", "Fold mix", "", 0.0f, 1.0f, 0.5f,
                        [] (const Patch& p) { return (float) p.foldMix; },
                        [] (Patch& p, float v) { p.foldMix = v; });

            //------------------------------------------------------------------
            // The drive, into the filter.

            pick ("drivecurve", "Curve", driveCurves,
                  [] (const Patch& p) { return (float) (int) p.driveCurve; },
                  [] (Patch& p, float v)
                  {
                      p.driveCurve = toEnum<Fx::Saturator::Curve> (v, 3);
                  });

            continuous ("drive", "Drive", "", 0.0f, 32.0f, 3.0f,
                        [] (const Patch& p) { return (float) p.driveAmount; },
                        [] (Patch& p, float v) { p.driveAmount = v; });

            continuous ("drivebias", "Bias", "", -1.0f, 1.0f, 0.0f,
                        [] (const Patch& p) { return (float) p.driveBias; },
                        [] (Patch& p, float v) { p.driveBias = v; });

            //------------------------------------------------------------------
            // The filter. Both ids are the ones version one used, because
            // both mean exactly what they meant.

            pick ("filtertype", "Filter", filterTypes,
                  [] (const Patch& p) { return (float) (int) p.filterType; },
                  [] (Patch& p, float v)
                  {
                      p.filterType = toEnum<Fx::Svf::Type> (v, 10);
                  });

            // A kilohertz in the middle of the travel, so the knob spends as
            // much of itself on the two octaves either side of where most
            // filtering happens as it does on the top three.
            continuous ("cutoff", "Cutoff", " Hz", 20.0f, 20000.0f, 1000.0f,
                        [] (const Patch& p) { return (float) p.cutoff; },
                        [] (Patch& p, float v) { p.cutoff = v; });

            // The filter clamps to the range it can actually hold. This stops
            // short of the top of that so the knob does not end in a region
            // where the only thing it does is ring.
            continuous ("resonance", "Reso", "", 0.5f, 20.0f, 2.0f,
                        [] (const Patch& p) { return (float) p.resonance; },
                        [] (Patch& p, float v) { p.resonance = v; });

            continuous ("filterkey", "Key", "", 0.0f, 1.0f, 0.5f,
                        [] (const Patch& p) { return (float) p.filterKeyTrack; },
                        [] (Patch& p, float v) { p.filterKeyTrack = v; });

            //------------------------------------------------------------------
            // The gate.

            pick ("gatemode", "Gate", gateModes,
                  [] (const Patch& p) { return (float) (int) p.gateMode; },
                  [] (Patch& p, float v)
                  {
                      p.gateMode = toEnum<Synth::LowPassGate::Mode> (v, 3);
                  });

            continuous ("gaterange", "Range", " oct", 0.0f, 10.0f, 5.0f,
                        [] (const Patch& p) { return (float) p.gateRange; },
                        [] (Patch& p, float v) { p.gateRange = v; });

            continuous ("gateresting", "Resting", " Hz", 20.0f, 2000.0f, 200.0f,
                        [] (const Patch& p) { return (float) p.gateResting; },
                        [] (Patch& p, float v) { p.gateResting = v; });

            // How much of the vactrol to use. One is the measured part, and
            // nothing is an instant gate, which is sometimes what a patch
            // wants and is never what a low pass gate does on its own.
            continuous ("gatelag", "Lag", "", 0.0f, 1.0f, 0.5f,
                        [] (const Patch& p) { return (float) p.gateLag; },
                        [] (Patch& p, float v) { p.gateLag = v; });

            //------------------------------------------------------------------
            // The four envelopes.
            //
            // Envelopes one and two keep the ids version one used for its
            // amplitude and filter envelopes, because that is what they still
            // are on any patch that wants them to be: envelope one always
            // scales the output, and the first preset points envelope two at
            // the cutoff. Nothing is soldered, so a patch can point either
            // anywhere, but a project saved before this change gets its
            // envelope times back rather than a default.
            //
            // Times are skewed hard towards the short end because that is
            // where the difference between a pluck and a stab lives, and a
            // linear control there would put every percussive setting in the
            // first millimetre.
            const char* const envIds[4][4]
            {
                { "ampattack",    "ampdecay",    "ampsustain",    "amprelease" },
                { "filterattack", "filterdecay", "filtersustain", "filterrelease" },
                { "env3attack",   "env3decay",   "env3sustain",   "env3release" },
                { "env4attack",   "env4decay",   "env4sustain",   "env4release" }
            };

            for (int e = 0; e < 4; ++e)
            {
                const std::string which = "Env " + std::to_string (e + 1) + " ";

                continuous (envIds[e][0], keep (which + "A"), " s", 0.0005f, 5.0f, 0.05f,
                            [e] (const Patch& p)
                            { return (float) p.envelopes[e].attackSeconds; },
                            [e] (Patch& p, float v)
                            { p.envelopes[e].attackSeconds = v; });

                continuous (envIds[e][1], keep (which + "D"), " s", 0.001f, 8.0f, 0.200f,
                            [e] (const Patch& p)
                            { return (float) p.envelopes[e].decaySeconds; },
                            [e] (Patch& p, float v)
                            { p.envelopes[e].decaySeconds = v; });

                continuous (envIds[e][2], keep (which + "S"), "", 0.0f, 1.0f, 0.5f,
                            [e] (const Patch& p)
                            { return (float) p.envelopes[e].sustainLevel; },
                            [e] (Patch& p, float v)
                            { p.envelopes[e].sustainLevel = v; });

                continuous (envIds[e][3], keep (which + "R"), " s", 0.001f, 10.0f, 0.300f,
                            [e] (const Patch& p)
                            { return (float) p.envelopes[e].releaseSeconds; },
                            [e] (Patch& p, float v)
                            { p.envelopes[e].releaseSeconds = v; });
            }

            //------------------------------------------------------------------
            // The two slopes.
            //
            // The times run from well under a millisecond to twenty seconds,
            // which is what makes one module an envelope, an LFO and an
            // oscillator depending only on where it is set. Twenty seconds is
            // a drone that moves across a whole section; half a millisecond
            // cycling is an audio rate tone.

            for (int i = 0; i < 2; ++i)
            {
                const std::string prefix = "slope" + std::to_string (i + 1);
                const std::string which = "Slope " + std::to_string (i + 1) + " ";

                continuous (keep (prefix + "rise"), keep (which + "rise"), " s",
                            0.0002f, 20.0f, 0.1f,
                            [i] (const Patch& p)
                            { return (float) p.slopes[i].riseSeconds; },
                            [i] (Patch& p, float v) { p.slopes[i].riseSeconds = v; });

                continuous (keep (prefix + "fall"), keep (which + "fall"), " s",
                            0.0002f, 20.0f, 0.1f,
                            [i] (const Patch& p)
                            { return (float) p.slopes[i].fallSeconds; },
                            [i] (Patch& p, float v) { p.slopes[i].fallSeconds = v; });

                continuous (keep (prefix + "shape"), keep (which + "shape"), "",
                            -1.0f, 1.0f, 0.0f,
                            [i] (const Patch& p) { return (float) p.slopes[i].shape; },
                            [i] (Patch& p, float v) { p.slopes[i].shape = v; });

                pick (keep (prefix + "cycle"), keep (which + "cycling"), offOrOn,
                      [i] (const Patch& p) { return p.slopes[i].cycling ? 1.0f : 0.0f; },
                      [i] (Patch& p, float v) { p.slopes[i].cycling = v > 0.5f; });
            }

            //------------------------------------------------------------------
            // The four LFOs.

            for (int i = 0; i < 4; ++i)
            {
                const std::string prefix = "lfo" + std::to_string (i + 1);
                const std::string which = "LFO " + std::to_string (i + 1) + " ";

                pick (keep (prefix + "shape"), keep (which + "shape"), lfoShapes,
                      [i] (const Patch& p) { return (float) (int) p.lfos[i].shape; },
                      [i] (Patch& p, float v)
                      {
                          p.lfos[i].shape = toEnum<Synth::Lfo::Shape> (v, 8);
                      });

                // A hundredth of a hertz to a hundred, which is a cycle
                // every hundred seconds at one end and well into the
                // audible at the other, and the matrix can drive it six
                // octaves past that.
                continuous (keep (prefix + "rate"), keep (which + "rate"), " Hz",
                            0.01f, 100.0f, 2.0f,
                            [i] (const Patch& p) { return (float) p.lfos[i].rateHz; },
                            [i] (Patch& p, float v) { p.lfos[i].rateHz = v; });

                pick (keep (prefix + "sync"), keep (which + "sync"), offOrOn,
                      [i] (const Patch& p) { return p.lfos[i].synced ? 1.0f : 0.0f; },
                      [i] (Patch& p, float v) { p.lfos[i].synced = v > 0.5f; });

                pick (keep (prefix + "div"), keep (which + "division"), lfoDivisions,
                      [i] (const Patch& p)
                      { return (float) divisionFromBeats (p.lfos[i].beatsPerCycle); },
                      [i] (Patch& p, float v)
                      {
                          const auto& table = divisionTable();
                          const int index = toIndex (v, (int) table.size());

                          p.lfos[i].beatsPerCycle = table[(std::size_t) index].beats;
                      });

                continuous (keep (prefix + "width"), keep (which + "width"), "",
                            0.02f, 0.98f, 0.5f,
                            [i] (const Patch& p) { return (float) p.lfos[i].pulseWidth; },
                            [i] (Patch& p, float v) { p.lfos[i].pulseWidth = v; });

                continuous (keep (prefix + "phase"), keep (which + "phase"), "",
                            0.0f, 1.0f, 0.5f,
                            [i] (const Patch& p) { return (float) p.lfos[i].phaseOffset; },
                            [i] (Patch& p, float v) { p.lfos[i].phaseOffset = v; });

                pick (keep (prefix + "trig"), keep (which + "retrigger"), offOrOn,
                      [i] (const Patch& p) { return p.lfos[i].retrigger ? 1.0f : 0.0f; },
                      [i] (Patch& p, float v) { p.lfos[i].retrigger = v > 0.5f; });

                continuous (keep (prefix + "fade"), keep (which + "fade"), " s",
                            0.0f, 20.0f, 1.0f,
                            [i] (const Patch& p) { return (float) p.lfos[i].fadeSeconds; },
                            [i] (Patch& p, float v) { p.lfos[i].fadeSeconds = v; });
            }

            //------------------------------------------------------------------
            // The four macros.
            //
            // In the patch and therefore in the table, because where a
            // macro is sitting is part of a sound. The mod wheel and the
            // pressure are not: they are what a player is doing at the
            // time, so they arrive from MIDI and are never saved.

            for (int i = 0; i < 4; ++i)
                continuous (keep ("macro" + std::to_string (i + 1)),
                            keep ("Macro " + std::to_string (i + 1)), "",
                            0.0f, 1.0f, 0.5f,
                            [i] (const Patch& p) { return (float) p.macros[i]; },
                            [i] (Patch& p, float v) { p.macros[i] = v; });

            //------------------------------------------------------------------
            // The matrix, generated rather than written out.
            //
            // A row's source and destination are stored as a matrix index,
            // where minus one is an empty row, and offered as a choice whose
            // first entry is "none". So the parameter is the index plus one,
            // and that is the whole of the conversion.

            for (int r = 0; r < Synth::ModMatrix::maxRoutes; ++r)
            {
                const std::string prefix = "mod" + std::to_string (r + 1);
                const std::string which = "Mod " + std::to_string (r + 1) + " ";

                pick (keep (prefix + "src"), keep (which + "source"), modSources,
                      [r] (const Patch& p) { return (float) (p.matrix.routes[r].source + 1); },
                      [r] (Patch& p, float v)
                      {
                          p.matrix.routes[r].source
                              = toIndex (v, (int) modSources().size()) - 1;
                      });

                pick (keep (prefix + "dst"), keep (which + "destination"), modDestinations,
                      [r] (const Patch& p)
                      { return (float) (p.matrix.routes[r].destination + 1); },
                      [r] (Patch& p, float v)
                      {
                          p.matrix.routes[r].destination
                              = toIndex (v, (int) modDestinations().size()) - 1;
                      });

                // Minus one to one, in units of whatever the destination
                // declares a depth of one to be worth. That is what makes the
                // same gesture mean a comparable amount of movement wherever
                // it is pointed.
                continuous (keep (prefix + "depth"), keep (which + "depth"), "",
                            -1.0f, 1.0f, 0.0f,
                            [r] (const Patch& p) { return p.matrix.routes[r].depth; },
                            [r] (Patch& p, float v) { p.matrix.routes[r].depth = v; });

                pick (keep (prefix + "via"), keep (which + "via"), modSources,
                      [r] (const Patch& p) { return (float) (p.matrix.routes[r].via + 1); },
                      [r] (Patch& p, float v)
                      {
                          p.matrix.routes[r].via = toIndex (v, (int) modSources().size()) - 1;
                      });

                pick (keep (prefix + "shape"), keep (which + "shape"), modShapes,
                      [r] (const Patch& p)
                      { return (float) (int) p.matrix.routes[r].shape; },
                      [r] (Patch& p, float v)
                      {
                          p.matrix.routes[r].shape
                              = toEnum<Synth::ModMatrix::Shape> (v, 4);
                      });

                pick (keep (prefix + "on"), keep (which + "enabled"), offOrOn,
                      [r] (const Patch& p) { return p.matrix.routes[r].enabled ? 1.0f : 0.0f; },
                      [r] (Patch& p, float v) { p.matrix.routes[r].enabled = v > 0.5f; });
            }

            return s;
        }();

        return specs;
    }

    inline std::size_t count() { return all().size(); }

    /** Where the matrix rows start in the table, and how many controls each
        row has. For the editor, and for anything that wants to show the knobs
        without the wiring. */
    inline constexpr int controlsPerRoute = 6;

    inline std::size_t firstRouteControl()
    {
        static const std::size_t first = count()
                                           - (std::size_t) (Synth::ModMatrix::maxRoutes
                                                              * controlsPerRoute);
        return first;
    }

    // A field added to Synth::Voice::Patch and not added to the table above is
    // the one failure this cannot test for: the control simply is not there,
    // the presets still load, and nothing reports anything. So the size of the
    // struct is pinned, and growing it is a compile error that says to come
    // and look here. Update the number once the new field has a line in the
    // table, or a comment saying why it deliberately has none.
    static_assert (sizeof (Patch) == 1440,
                   "Synth::Voice::Patch has changed. Does the new field need a line in "
                   "PanOneParams::all()? If not, say why here and update the size.");

    //==========================================================================
    /** Reads a patch out into one float per control. */
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
        table mentions everything.

        The matrix's own row count is worked out here rather than being a
        control of its own. It is not a thing anybody sets: it is how far down
        the table the engine has to look, and the engine walks it per sample,
        so leaving it at thirty two would cost every voice thirty two checks a
        sample to find nothing. One past the last wired row is both correct
        and the cheapest it can be.
    */
    inline void toPatch (const std::vector<float>& values, Patch& patch)
    {
        const auto& specs = all();

        for (std::size_t i = 0; i < specs.size() && i < values.size(); ++i)
            specs[i].set (patch, values[i]);

        patch.matrix.count = 0;

        for (int r = 0; r < Synth::ModMatrix::maxRoutes; ++r)
            if (patch.matrix.routes[r].isWired())
                patch.matrix.count = r + 1;
    }

    //==========================================================================
    /** What version one of the instrument saved that this version no longer
        has a control for.

        A project written before the engine changed carries four controls that
        have no direct equivalent: two oscillator shapes chosen from a list of
        four, a pulse width, and a filter envelope depth in octaves. Left
        alone they would be dropped, and the project would open with the right
        envelope times and the wrong sound, which is the quiet kind of wrong
        this codebase keeps meeting.

        So they are translated. The shapes become a wavetable and a position
        in it, the width becomes a position in the pulse table, and the
        envelope depth becomes what it always was in effect: a routing from
        envelope two to the cutoff.

        Collected as a struct rather than applied one at a time because the
        shape and the width together decide one position, and because this
        way the whole translation is one function with no JUCE in it and can
        be tested.
    */
    struct Legacy
    {
        bool  present = false;      /**< whether an old project was recognised */

        int   osc1Shape = 1;        /**< 0 sine, 1 saw, 2 square, 3 triangle */
        int   osc2Shape = 1;
        float pulseWidth = 0.5f;
        float envOctaves = 0.0f;
    };

    /** Where a version one shape sits in the wavetables this version has.

        The analogue table runs triangle, saw, square, narrow pulse, so three
        of the four are positions in it. A sine is not in that table at all
        and is the first frame of the harmonics table, which is a single
        partial, so that one changes table as well as position.
    */
    inline void applyLegacyShape (int shape, float pulseWidth,
                                  int& table, double& position)
    {
        switch (shape)
        {
            case 0:     // sine
                table = Synth::WaveBank::indexFromId ("harmonics");
                position = 0.0;
                return;

            case 2:     // square, and the width control went with it
            {
                table = Synth::WaveBank::indexFromId ("pulse");

                // The pulse table sweeps from a half down to a sliver, and
                // geometrically rather than linearly, so a width is found by
                // undoing that rather than by scaling it. Half is the start
                // of the table and 0.03 is the end of it.
                const double width = pulseWidth < 0.03f ? 0.03
                                                        : (pulseWidth > 0.5f ? 0.5
                                                                             : (double) pulseWidth);

                position = std::log (width / 0.5) / std::log (0.06);
                position = position < 0.0 ? 0.0 : (position > 1.0 ? 1.0 : position);
                return;
            }

            case 3:     // triangle: the start of the analogue table
                table = Synth::WaveBank::indexFromId ("analogue");
                position = 0.0;
                return;

            case 1:     // saw
            default:
                // A third of the way along the analogue table is exactly the
                // sawtooth: the first third of it blends a triangle into one.
                table = Synth::WaveBank::indexFromId ("analogue");
                position = 1.0 / 3.0;
                return;
        }
    }

    inline void applyLegacy (const Legacy& old, Patch& patch)
    {
        if (! old.present)
            return;

        applyLegacyShape (old.osc1Shape, old.pulseWidth,
                          patch.osc1Table, patch.osc1Position);
        applyLegacyShape (old.osc2Shape, old.pulseWidth,
                          patch.osc2Table, patch.osc2Position);

        // Version one had no folder and no drive, so a project from it must
        // not arrive with either in circuit.
        patch.foldMix = 0.0;
        patch.driveAmount = 0.0;

        // And its filter envelope, which was an amount in octaves wired
        // permanently from the filter envelope to the cutoff. Here that is a
        // routing like any other, and the cutoff destination declares eight
        // octaves at full depth, so the depth is the octaves over eight.
        if (old.envOctaves > 0.0f)
        {
            const int env2 = Synth::ModMatrix::sourceFromId ("env2");
            const int cutoff = Synth::ModMatrix::destinationFromId ("cutoff");

            const float depth = old.envOctaves / 8.0f;

            patch.matrix.add (env2, cutoff, depth > 1.0f ? 1.0f : depth);
        }
    }

    //==========================================================================
    /** The presets, as data, for the same reason the controls are: a test can
        then check that every value in every preset is inside the range of the
        parameter that will hold it. A preset value outside its range is
        quietly clamped, so the preset ships sounding like something nobody
        designed, which is exactly the failure that is impossible to notice by
        reading the code. */
    struct Preset
    {
        const char* name;
        const char* category;
        Patch patch;
    };

    inline const std::vector<Preset>& presets();

    /** Where each group of controls starts, for laying the editor out and for
        nothing else. Kept here so that reordering the table cannot leave the
        editor drawing the wrong headings over the wrong knobs. */
    struct Group
    {
        /** Whether a group is knobs on a panel or the wiring behind it.

            The matrix is a hundred and ninety two controls, which is not
            something to lay out in rows beside the oscillators: it needs a
            view of its own, shaped like a routing table rather than like a
            row of knobs. Marking it here rather than having the panel match
            on the heading means renaming the heading cannot change which
            controls appear on the panel.
        */
        enum class Kind { knobs, matrix };

        const char* heading;
        std::size_t first, count;
        Kind kind = Kind::knobs;
    };

    inline const std::vector<Group>& groups()
    {
        static const std::vector<Group> g = []
        {
            std::vector<Group> out;

            std::size_t at = 0;

            const auto add = [&] (const char* heading, std::size_t howMany,
                                  Group::Kind kind = Group::Kind::knobs)
            {
                out.push_back ({ heading, at, howMany, kind });
                at += howMany;
            };

            // Six rows of controls rather than eleven. The headings are
            // broader for it, and the reason is the panel: a row is a
            // hundred pixels tall, and eleven of them is a window twelve
            // hundred pixels high, which is taller than the screen on a
            // laptop. Six is seven hundred, and a row of ten knobs across
            // eight hundred is seventy four pixels each, which is what one
            // of them needs.
            add ("Oscillators", 10);
            add ("Folder and drive", 6);
            add ("Filter and gate", 8);
            add ("Envelopes 1 and 2", 8);
            add ("Envelopes 3 and 4", 8);
            add ("Slopes", 8);
            add ("LFO 1 and 2", 16);
            add ("LFO 3 and 4", 16);
            add ("Macros", 4);
            add ("Matrix", (std::size_t) (Synth::ModMatrix::maxRoutes * controlsPerRoute),
                 Group::Kind::matrix);

            return out;
        }();

        return g;
    }
}

//==============================================================================
/** The preset bank.

    Written as data for the reason the controls are: a test walks every value
    of every preset against the range of the parameter that will hold it. A
    value outside its range is quietly clamped when the preset is chosen, so
    the preset ships sounding like something nobody designed, and reading the
    code will never tell you.

    They are grouped by what somebody reaches for rather than by what part of
    the engine they use, because that is how anybody looks for a sound. The
    engine is the same in all of them: there is no bass oscillator or pad
    filter, only a patch.
*/
namespace PanOneParams
{
    inline const std::vector<Preset>& presets()
    {
        static const std::vector<Preset> list = []
        {
            using Shape = Synth::ModMatrix::Shape;
            using Mode = Synth::LowPassGate::Mode;
            using Curve = Fx::Saturator::Curve;

            // The ids, resolved once, so that a preset says what it means
            // rather than carrying a number nobody can read.
            const int env1 = Synth::ModMatrix::sourceFromId ("env1");
            const int env2 = Synth::ModMatrix::sourceFromId ("env2");
            const int env3 = Synth::ModMatrix::sourceFromId ("env3");
            const int slope1 = Synth::ModMatrix::sourceFromId ("slope1");
            const int slope2 = Synth::ModMatrix::sourceFromId ("slope2");
            const int velocity = Synth::ModMatrix::sourceFromId ("velocity");
            const int keyTrack = Synth::ModMatrix::sourceFromId ("keytrack");
            const int random = Synth::ModMatrix::sourceFromId ("random");

            const int toCutoff = Synth::ModMatrix::destinationFromId ("cutoff");
            const int toResonance = Synth::ModMatrix::destinationFromId ("resonance");
            const int toFold = Synth::ModMatrix::destinationFromId ("fold");
            const int toFoldMix = Synth::ModMatrix::destinationFromId ("foldmix");
            const int toFoldSym = Synth::ModMatrix::destinationFromId ("foldsymmetry");
            const int toDrive = Synth::ModMatrix::destinationFromId ("drive");
            const int toGate = Synth::ModMatrix::destinationFromId ("gateopen");
            const int toAmp = Synth::ModMatrix::destinationFromId ("amp");
            const int toOsc1Pos = Synth::ModMatrix::destinationFromId ("osc1pos");
            const int toOsc2Pos = Synth::ModMatrix::destinationFromId ("osc2pos");
            const int toOsc2Pitch = Synth::ModMatrix::destinationFromId ("osc2pitch");
            const int toOsc1Pitch = Synth::ModMatrix::destinationFromId ("osc1pitch");
            const int toSlope1Rate = Synth::ModMatrix::destinationFromId ("slope1rate");
            const int toFm = Synth::ModMatrix::destinationFromId ("fm");
            const int toNoise = Synth::ModMatrix::destinationFromId ("noiselevel");

            const int analogue = Synth::WaveBank::indexFromId ("analogue");
            const int pulse = Synth::WaveBank::indexFromId ("pulse");
            const int harmonics = Synth::WaveBank::indexFromId ("harmonics");
            const int odd = Synth::WaveBank::indexFromId ("odd");
            const int comb = Synth::WaveBank::indexFromId ("comb");
            const int phase = Synth::WaveBank::indexFromId ("phase");
            const int sync = Synth::WaveBank::indexFromId ("sync");
            const int bits = Synth::WaveBank::indexFromId ("bits");
            const int foldTable = Synth::WaveBank::indexFromId ("fold");
            const int cluster = Synth::WaveBank::indexFromId ("cluster");

            // A plain amplifier and a plain filter, which is where an east
            // coast patch starts: the gate as an ordinary amplitude control
            // and the resonant filter doing the shaping. A west coast patch
            // overrides the gate instead.
            const auto eastCoast = [&]
            {
                Patch p;
                p.gateMode = Mode::amplitude;
                p.gateRange = 0.0;

                // No vactrol. That is the whole difference between the gate
                // as an ordinary amplifier and the gate as a low pass gate:
                // the lamp and the light dependent resistor take about twelve
                // milliseconds to light and a quarter of a second to let go,
                // and an envelope faster than that cannot get through them.
                // Left at one, a hat with a half millisecond attack and a
                // seventy millisecond decay measured 26 dB below where it
                // should have been, because the gate had barely begun to open
                // before the envelope had finished.
                p.gateLag = 0.0;

                p.cutoff = 20000.0;
                p.resonance = 0.7071;
                return p;
            };

            // And the other tradition: the gate taking brightness and level
            // together through its vactrol, with the filter left open.
            const auto westCoast = [&]
            {
                Patch p;
                p.gateMode = Mode::both;
                p.gateRange = 5.0;
                p.gateResting = 90.0;
                p.gateLag = 1.0;
                p.cutoff = 20000.0;
                return p;
            };

            std::vector<Preset> out;

            const auto add = [&out] (const char* name, const char* category,
                                     const Patch& patch)
            {
                out.push_back ({ name, category, patch });
            };

            //------------------------------------------------------------------
            // Bass.

            {
                auto p = eastCoast();
                p.osc1Table = analogue;  p.osc1Position = 0.40; p.osc1Level = 0.55;
                p.osc2Table = analogue;  p.osc2Position = 0.66; p.osc2Level = 0.25;
                p.osc2Semitones = -0.06;
                p.subLevel = 0.50;
                p.cutoff = 220.0; p.resonance = 1.6; p.filterKeyTrack = 0.4;
                p.driveAmount = 2.2; p.driveCurve = Curve::soft;
                p.envelopes[0] = { 0.002, 0.250, 0.55, 0.120 };
                p.envelopes[1] = { 0.001, 0.140, 0.10, 0.120 };
                p.matrix.add (env2, toCutoff, 0.34f);
                p.matrix.add (velocity, toCutoff, 0.12f);
                add ("Deep Sub", "Bass", p);
            }

            {
                auto p = eastCoast();
                p.osc1Table = analogue; p.osc1Position = 0.42; p.osc1Level = 0.30;
                p.osc2Table = analogue; p.osc2Position = 0.42; p.osc2Level = 0.30;
                p.osc2Semitones = 0.11;
                p.subLevel = 0.18;
                p.cutoff = 300.0; p.resonance = 1.7; p.filterKeyTrack = 0.2;
                p.driveAmount = 4.0;
                p.envelopes[0] = { 0.004, 0.400, 0.80, 0.200 };
                p.slopes[0] = { 0.9, 0.9, 0.0, true };
                p.matrix.add (slope1, toCutoff, 0.16f, Shape::bipolar);
                p.matrix.add (slope1, toOsc2Pitch, 0.004f, Shape::bipolar);
                add ("Wide Growl", "Bass", p);
            }

            {
                auto p = eastCoast();
                p.osc1Table = pulse; p.osc1Position = 0.25; p.osc1Level = 0.34;
                p.subLevel = 0.14;
                p.cutoff = 160.0; p.resonance = 3.5; p.filterKeyTrack = 0.5;
                p.driveAmount = 3.0; p.driveCurve = Curve::cubic;
                p.envelopes[0] = { 0.001, 0.180, 0.00, 0.110 };
                p.envelopes[1] = { 0.001, 0.120, 0.00, 0.100 };
                p.matrix.add (env2, toCutoff, 0.52f);
                p.matrix.add (velocity, toCutoff, 0.18f);
                add ("Squelch", "Bass", p);
            }

            //------------------------------------------------------------------
            // Lead.

            {
                auto p = eastCoast();
                p.osc1Table = pulse; p.osc1Position = 0.35; p.osc1Level = 0.6;
                p.osc2Table = analogue; p.osc2Position = 0.40; p.osc2Level = 0.3;
                p.osc2Semitones = 0.05;
                p.subLevel = 0.2;
                p.cutoff = 1400.0; p.resonance = 2.2; p.filterKeyTrack = 0.6;
                p.driveAmount = 2.0;
                p.envelopes[0] = { 0.006, 0.300, 0.85, 0.160 };
                p.envelopes[1] = { 0.004, 0.260, 0.45, 0.180 };
                p.slopes[0] = { 0.18, 0.18, 0.0, true };
                p.matrix.add (env2, toCutoff, 0.26f);
                // The slope's depth scaled by envelope three, so the vibrato
                // arrives as the note is held rather than being there from
                // the attack, which is what a player actually does.
                p.envelopes[2] = { 0.450, 1.000, 1.00, 0.300 };
                p.matrix.add (slope1, toOsc1Pitch, 0.006f, Shape::bipolar, env3);
                p.matrix.add (slope1, toOsc2Pitch, 0.006f, Shape::bipolar, env3);
                add ("Reed Lead", "Lead", p);
            }

            {
                auto p = eastCoast();
                p.osc1Table = sync; p.osc1Position = 0.30; p.osc1Level = 0.45;
                p.osc2Table = analogue; p.osc2Position = 0.45; p.osc2Level = 0.18;
                p.osc2Semitones = 7.0;
                p.cutoff = 2600.0; p.resonance = 1.2; p.filterKeyTrack = 0.7;
                p.driveAmount = 3.5; p.driveCurve = Curve::cubic;
                p.envelopes[0] = { 0.003, 0.260, 0.90, 0.130 };
                p.envelopes[1] = { 0.002, 0.300, 0.30, 0.150 };
                p.matrix.add (env2, toOsc1Pos, 0.40f);
                p.matrix.add (velocity, toDrive, 0.25f);
                add ("Hard Sync", "Lead", p);
            }

            //------------------------------------------------------------------
            // Keys.

            {
                auto p = eastCoast();
                p.osc1Table = analogue; p.osc1Position = 0.36; p.osc1Level = 0.5;
                p.osc2Table = analogue; p.osc2Position = 0.36; p.osc2Level = 0.5;
                p.osc2Semitones = 0.09;
                p.subLevel = 0.15;
                p.cutoff = 1100.0; p.resonance = 0.9; p.filterKeyTrack = 0.5;
                p.envelopes[0] = { 0.008, 0.900, 0.55, 0.350 };
                p.envelopes[1] = { 0.010, 0.600, 0.25, 0.300 };
                p.matrix.add (env2, toCutoff, 0.30f);
                p.matrix.add (velocity, toAmp, -0.35f, Shape::inverted);
                add ("Soft Keys", "Keys", p);
            }

            {
                auto p = eastCoast();
                p.osc1Table = harmonics; p.osc1Position = 0.30; p.osc1Level = 0.6;
                p.osc2Table = harmonics; p.osc2Position = 0.12; p.osc2Level = 0.35;
                p.osc2Semitones = 12.0;
                p.fmAmount = 0.9;
                p.cutoff = 3200.0; p.resonance = 0.8; p.filterKeyTrack = 0.8;
                p.envelopes[0] = { 0.002, 1.200, 0.20, 0.400 };
                p.envelopes[1] = { 0.001, 0.500, 0.00, 0.250 };
                p.matrix.add (env2, toFm, 0.50f);
                p.matrix.add (velocity, toFm, 0.30f);
                add ("Bell Tine", "Keys", p);
            }

            //------------------------------------------------------------------
            // Pad.

            {
                auto p = eastCoast();
                p.osc1Table = analogue; p.osc1Position = 0.30; p.osc1Level = 0.45;
                p.osc2Table = analogue; p.osc2Position = 0.55; p.osc2Level = 0.45;
                p.osc2Semitones = 0.17;
                p.subLevel = 0.20;
                p.cutoff = 700.0; p.resonance = 0.9; p.filterKeyTrack = 0.3;
                p.envelopes[0] = { 0.600, 1.400, 0.85, 1.200 };
                p.envelopes[1] = { 1.100, 2.000, 0.55, 1.000 };
                p.slopes[0] = { 3.2, 4.1, 0.0, true };
                p.slopes[1] = { 5.3, 2.9, 0.0, true };
                p.matrix.add (env2, toCutoff, 0.32f);
                p.matrix.add (slope1, toOsc1Pos, 0.18f, Shape::bipolar);
                p.matrix.add (slope2, toOsc2Pos, 0.22f, Shape::bipolar);
                p.matrix.add (random, toOsc2Pitch, 0.012f);
                add ("Slow Pad", "Pad", p);
            }

            {
                auto p = westCoast();
                p.osc1Table = cluster; p.osc1Position = 0.25; p.osc1Level = 0.5;
                p.osc2Table = harmonics; p.osc2Position = 0.0; p.osc2Level = 0.3;
                p.osc2Semitones = -12.0;
                p.noiseLevel = 0.04;
                p.gateRange = 3.5; p.gateResting = 180.0;
                p.foldDrive = 2.0; p.foldMix = 0.35;
                p.envelopes[0] = { 1.200, 2.000, 0.90, 2.400 };
                p.slopes[0] = { 6.0, 7.5, 0.0, true };
                p.slopes[1] = { 2.1, 3.4, 0.4, true };
                p.matrix.add (slope1, toFoldMix, 0.35f);
                p.matrix.add (slope2, toOsc1Pos, 0.30f, Shape::bipolar);
                p.matrix.add (slope1, toGate, 0.55f);
                p.matrix.add (env1, toGate, 0.50f);
                add ("Glass Shelf", "Pad", p);
            }

            //------------------------------------------------------------------
            // Pluck.

            {
                auto p = westCoast();
                p.osc1Table = harmonics; p.osc1Position = 0.20; p.osc1Level = 0.7;
                p.osc2Table = analogue; p.osc2Position = 0.0; p.osc2Level = 0.2;
                p.osc2Semitones = 12.02;
                p.gateRange = 6.0; p.gateResting = 70.0; p.gateLag = 0.45;
                p.foldDrive = 2.4; p.foldMix = 0.30;

                // Slower than a pluck on a filter would be, and
                // deliberately. The amplitude is the envelope times the
                // gate's own square of it, so a decay shorter than the
                // vactrol's rise is a sound that has finished before the gate
                // is open: at a millisecond of attack and a third of a second
                // of decay this preset measured 42 dB down.
                p.envelopes[0] = { 0.004, 0.800, 0.00, 0.450 };
                // Envelope one has to be here as well as the velocity, and
                // that is not a detail. The moment anything at all is
                // pointed at the gate, the voice stops using the amplitude
                // envelope to open it, because a patch that has wired the
                // gate deliberately does not want an envelope fighting it.
                // So a routing from the velocity on its own leaves the gate
                // sitting at one fixed opening for the whole note: this
                // preset measured 46 dB down, with a gate held at 0.18 and
                // its amplitude the square of that.
                p.matrix.add (env1, toGate, 0.80f);
                p.matrix.add (velocity, toGate, 0.20f);
                p.matrix.add (velocity, toFoldMix, 0.35f);
                add ("Struck", "Pluck", p);
            }

            {
                auto p = eastCoast();
                p.osc1Table = comb; p.osc1Position = 0.35; p.osc1Level = 0.7;
                p.subLevel = 0.1;
                p.cutoff = 2400.0; p.resonance = 1.2; p.filterKeyTrack = 0.9;
                p.envelopes[0] = { 0.0008, 0.220, 0.00, 0.160 };
                p.envelopes[1] = { 0.0008, 0.090, 0.00, 0.090 };
                p.matrix.add (env2, toCutoff, 0.44f);
                p.matrix.add (env2, toOsc1Pos, -0.30f);
                add ("Tine Pluck", "Pluck", p);
            }

            //------------------------------------------------------------------
            // Drone and ritual, which is where this engine is most itself.

            {
                // Built from the recording this engine was extended for: a
                // low drone around 52 Hz, almost entirely odd harmonics, with
                // a sweep going round about every three seconds.
                //
                // A sine into the folder is what makes the harmonics odd, and
                // the sweep is a cycling slope on the fold rather than an
                // envelope, because the movement has no beginning or end. The
                // slope's two times add to 2.86 seconds, which is 0.35 times
                // a second, read off the recording.
                auto p = westCoast();
                p.osc1Table = harmonics; p.osc1Position = 0.0; p.osc1Level = 0.85;
                p.osc2Table = harmonics; p.osc2Position = 0.0; p.osc2Level = 0.12;
                p.osc2Semitones = -12.0;
                p.noiseLevel = 0.015;

                p.foldDrive = 3.0; p.foldMix = 1.0; p.foldSymmetry = 0.0;
                p.driveAmount = 1.4; p.driveCurve = Curve::soft;

                p.gateRange = 2.2; p.gateResting = 150.0; p.gateLag = 1.0;

                p.envelopes[0] = { 0.900, 2.000, 1.00, 2.800 };

                p.slopes[0] = { 1.43, 1.43, 0.0, true };
                p.slopes[1] = { 9.0, 11.0, 0.0, true };

                p.matrix.add (slope1, toFold, 0.42f);
                p.matrix.add (slope1, toGate, 0.30f);
                p.matrix.add (env1, toGate, 0.70f);
                p.matrix.add (slope2, toFoldSym, 0.25f, Shape::bipolar);
                p.matrix.add (slope2, toSlope1Rate, 0.06f, Shape::bipolar);
                add ("Folded Drone", "Drone", p);
            }

            {
                auto p = westCoast();
                p.osc1Table = odd; p.osc1Position = 0.30; p.osc1Level = 0.7;
                p.osc2Table = cluster; p.osc2Position = 0.55; p.osc2Level = 0.25;
                p.osc2Semitones = -5.0;
                p.noiseLevel = 0.03;
                p.gateRange = 3.0; p.gateResting = 110.0;
                p.foldDrive = 5.0; p.foldMix = 0.6;
                p.envelopes[0] = { 1.600, 2.000, 1.00, 3.600 };
                p.slopes[0] = { 4.7, 6.3, -0.4, true };
                p.slopes[1] = { 13.0, 17.0, 0.0, true };
                p.matrix.add (slope1, toFold, 0.30f);
                p.matrix.add (slope2, toOsc2Pos, 0.40f, Shape::bipolar);
                p.matrix.add (slope1, toGate, 0.35f);
                p.matrix.add (env1, toGate, 0.65f);
                add ("Long Vigil", "Drone", p);
            }

            {
                auto p = westCoast();
                p.osc1Table = harmonics; p.osc1Position = 0.0; p.osc1Level = 0.8;
                p.osc2Table = harmonics; p.osc2Position = 0.0; p.osc2Level = 0.5;
                p.osc2Semitones = 7.02;
                p.fmAmount = 1.6;
                p.gateRange = 2.0; p.gateResting = 200.0;
                p.foldDrive = 6.0; p.foldMix = 0.8; p.foldSymmetry = 0.25;
                p.envelopes[0] = { 2.400, 2.000, 1.00, 4.000 };
                p.slopes[0] = { 2.3, 2.3, 0.0, true };
                p.slopes[1] = { 7.0, 9.0, 0.0, true };
                p.matrix.add (slope1, toFm, 0.22f, Shape::bipolar);
                p.matrix.add (slope2, toFold, 0.35f);
                p.matrix.add (env1, toGate, 1.00f);
                add ("Bowed Metal", "Drone", p);
            }

            //------------------------------------------------------------------
            // Texture.

            {
                auto p = westCoast();
                p.osc1Table = bits; p.osc1Position = 0.45; p.osc1Level = 0.55;
                p.osc2Table = phase; p.osc2Position = 0.60; p.osc2Level = 0.35;
                p.osc2Semitones = 0.3;
                p.noiseLevel = 0.10;
                p.gateRange = 4.0; p.gateResting = 140.0; p.gateLag = 0.9;
                p.foldDrive = 3.5; p.foldMix = 0.5;
                p.envelopes[0] = { 0.800, 2.000, 0.90, 2.000 };
                p.slopes[0] = { 0.6, 0.9, 0.0, true };
                p.slopes[1] = { 8.0, 5.0, 0.0, true };
                p.matrix.add (slope1, toGate, 0.45f);
                p.matrix.add (env1, toGate, 0.55f);
                p.matrix.add (slope2, toOsc1Pos, 0.45f, Shape::bipolar);
                p.matrix.add (slope1, toNoise, 0.10f);
                p.matrix.add (random, toOsc1Pos, 0.20f);
                add ("Static Field", "Texture", p);
            }

            {
                auto p = westCoast();
                p.osc1Table = comb; p.osc1Position = 0.70; p.osc1Level = 0.6;
                p.osc2Table = foldTable; p.osc2Position = 0.40; p.osc2Level = 0.4;
                p.osc2Semitones = 19.02;
                p.gateRange = 5.5; p.gateResting = 90.0; p.gateLag = 0.5;
                p.foldDrive = 8.0; p.foldMix = 0.7;
                p.envelopes[0] = { 0.004, 1.800, 0.35, 1.400 };
                p.slopes[0] = { 0.08, 0.26, 0.5, true };
                p.slopes[1] = { 3.0, 4.0, 0.0, true };
                p.matrix.add (slope1, toGate, 0.60f);
                p.matrix.add (env1, toGate, 0.40f);
                p.matrix.add (slope2, toSlope1Rate, 0.30f, Shape::bipolar);
                p.matrix.add (keyTrack, toFold, 0.20f);
                add ("Shimmer Rattle", "Texture", p);
            }

            //------------------------------------------------------------------
            // Percussion, which is a gate struck hard and almost nothing else.

            {
                auto p = westCoast();
                p.osc1Table = harmonics; p.osc1Position = 0.0; p.osc1Level = 0.9;
                p.noiseLevel = 0.06;
                p.gateRange = 7.5; p.gateResting = 55.0; p.gateLag = 0.35;
                p.foldDrive = 6.0; p.foldMix = 0.55;
                p.envelopes[0] = { 0.0005, 0.120, 0.00, 0.080 };
                p.envelopes[1] = { 0.0005, 0.030, 0.00, 0.030 };
                p.matrix.add (env2, toOsc1Pitch, 0.35f);
                p.matrix.add (velocity, toFoldMix, 0.40f);
                add ("Tom Strike", "Percussion", p);
            }

            {
                auto p = eastCoast();
                p.osc1Table = bits; p.osc1Position = 0.8; p.osc1Level = 0.3;
                p.noiseLevel = 0.85;
                p.cutoff = 6000.0; p.resonance = 1.1;
                p.envelopes[0] = { 0.0005, 0.070, 0.00, 0.050 };
                p.envelopes[1] = { 0.0005, 0.040, 0.00, 0.040 };
                p.matrix.add (env2, toCutoff, 0.30f);
                p.matrix.add (velocity, toCutoff, 0.20f);
                add ("Metal Hat", "Percussion", p);
            }

            //------------------------------------------------------------------
            // Risers and effects.

            {
                auto p = eastCoast();
                p.osc1Table = analogue; p.osc1Position = 0.45; p.osc1Level = 0.38;
                p.osc2Table = analogue; p.osc2Position = 0.45; p.osc2Level = 0.38;
                p.osc2Semitones = 0.25;
                p.noiseLevel = 0.10;
                p.cutoff = 400.0; p.resonance = 1.8;
                p.driveAmount = 2.0;
                p.envelopes[0] = { 0.100, 2.000, 0.95, 0.600 };
                // Envelope three is the riser itself: a single very long
                // sweep that never comes back while the note is held.
                p.envelopes[2] = { 4.000, 2.000, 1.00, 0.400 };
                p.matrix.add (env3, toCutoff, 0.70f);
                p.matrix.add (env3, toResonance, 0.14f);
                p.matrix.add (env3, toOsc1Pitch, 0.08f);
                p.matrix.add (env3, toOsc2Pitch, 0.08f);
                add ("Long Riser", "Riser", p);
            }

            {
                auto p = westCoast();
                p.osc1Table = sync; p.osc1Position = 0.50; p.osc1Level = 0.7;
                p.osc2Table = phase; p.osc2Position = 0.30; p.osc2Level = 0.3;
                p.osc2Semitones = -24.0;
                p.gateRange = 4.0; p.gateResting = 120.0;
                p.foldDrive = 10.0; p.foldMix = 0.9;
                p.envelopes[0] = { 0.010, 1.200, 0.60, 0.800 };
                p.slopes[0] = { 0.03, 0.03, 0.0, true };
                p.slopes[1] = { 1.4, 1.4, 0.0, true };
                p.matrix.add (slope1, toFoldSym, 0.50f, Shape::bipolar);
                p.matrix.add (slope2, toSlope1Rate, 0.60f, Shape::bipolar);
                p.matrix.add (env1, toGate, 1.00f);
                add ("Siren Fold", "Riser", p);
            }

            //------------------------------------------------------------------
            // Experimental: the patches that exist to show what the matrix
            // can do rather than to be reached for.

            {
                auto p = westCoast();
                p.osc1Table = harmonics; p.osc1Position = 0.0; p.osc1Level = 0.8;
                p.osc2Table = harmonics; p.osc2Position = 0.0; p.osc2Level = 0.6;
                p.osc2Semitones = 0.0;
                p.fmAmount = 3.0;
                p.gateRange = 3.0; p.gateResting = 160.0;
                p.foldDrive = 4.0; p.foldMix = 0.6;
                p.envelopes[0] = { 0.300, 2.000, 0.80, 1.600 };
                p.slopes[0] = { 0.012, 0.012, 0.0, true };   // audio rate
                p.slopes[1] = { 4.0, 6.0, 0.0, true };
                // A slope cycling fast enough to be a tone in its own right,
                // pointed at the modulation index, which is the trick the
                // module exists for: the same object is an envelope slowed
                // down and an oscillator sped up.
                p.matrix.add (slope1, toFm, 0.30f, Shape::bipolar);
                p.matrix.add (slope2, toSlope1Rate, 0.45f, Shape::bipolar);
                p.matrix.add (env1, toGate, 1.00f);
                add ("Audio Rate Slope", "Experimental", p);
            }

            {
                auto p = westCoast();
                p.osc1Table = foldTable; p.osc1Position = 0.60; p.osc1Level = 0.75;
                p.osc2Table = odd; p.osc2Position = 0.80; p.osc2Level = 0.45;
                p.osc2Semitones = 6.03;
                p.noiseLevel = 0.05;
                p.gateRange = 6.0; p.gateResting = 75.0; p.gateLag = 0.6;
                p.foldDrive = 14.0; p.foldMix = 1.0; p.foldSymmetry = -0.35;
                p.driveAmount = 6.0; p.driveCurve = Curve::hard; p.driveBias = 0.2;
                p.envelopes[0] = { 0.002, 0.900, 0.45, 0.700 };
                p.slopes[0] = { 0.22, 0.14, 0.6, true };
                p.slopes[1] = { 1.1, 0.7, -0.5, true };
                p.matrix.add (slope1, toFold, 0.45f, Shape::bipolar);
                p.matrix.add (slope2, toFoldSym, 0.60f, Shape::bipolar);
                p.matrix.add (random, toSlope1Rate, 0.30f);
                p.matrix.add (slope1, toGate, 0.50f);
                p.matrix.add (env1, toGate, 0.50f);
                add ("Broken Machine", "Experimental", p);
            }

            return out;
        }();

        return list;
    }
}
