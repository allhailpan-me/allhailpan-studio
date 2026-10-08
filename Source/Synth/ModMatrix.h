#pragma once
#include <cstddef>
#include <cstring>
#include <vector>

//==============================================================================
/** What drives what, and by how much.

    This is the difference between a synthesiser and a sound design engine. An
    instrument with fixed wiring has a filter envelope because somebody soldered
    one in; an instrument with a matrix has whatever the patch says, so a
    performance envelope can sweep a wavetable, an LFO can push the pitch of one
    oscillator into another's modulation input, a random value per note can
    scatter the stereo field, and none of that needed to be anticipated.

    Three decisions here are worth stating, because each of them is a thing that
    goes wrong quietly.

    **Sources and destinations are named, not numbered.** A routing is saved in
    the project, so it has to survive the next release. If a destination were an
    index into a list and somebody inserted a new entry in the middle, every
    patch anybody had ever saved would come back modulating the wrong thing: the
    filter envelope would be on the pan, the vibrato would be on the amplitude.
    The patch would still load, still play, and be wrong, which is the failure
    mode CLAUDE.md is about. So the tables below carry a stable string id for
    every source and destination, the same discipline `PanOneParams` uses for
    knobs, and the id is what a project stores.

    For the same reason an unknown id is survivable rather than fatal. A project
    saved by a later version, or one whose routing referred to something since
    removed, loses that routing and keeps the rest.

    **Every destination declares how far full depth moves it, in its own
    units.** Depth is a number from minus one to one in every slot, which is
    what makes a matrix usable: the same gesture means the same thing wherever
    it is pointed. Without a declared range, depth would be in volts or hertz or
    nothing at all, and a patch would need a different number in every row to
    produce a comparable amount of movement. The ranges are chosen so that full
    depth is the most anybody would ask for rather than the most the parameter
    can take: four octaves of pitch, eight of cutoff.

    **A routing can be scaled by a third source.** The `via` slot means
    "this much, but only as far as that lets it", which is how a mod wheel comes
    to control the depth of a vibrato rather than the vibrato itself, and how an
    envelope can open the door for an LFO that is otherwise silent. Without it
    every routing is a fixed amount and a patch can only be turned on and off.
    The idea is Xfer Serum's, where the matrix has exactly this column.

    The polarity transform is from Will Pirkle's SynthLab, which has the same
    idea under the name of a destination transform:
    https://www.willpirkle.com/synthlab/docs/html/mod_matrix.html
    It earns its place because a bipolar source pointed at something that only
    makes sense in one direction is a common and musical thing to want. An LFO
    forced unipolar on a filter opens it and returns, rather than closing it
    for half of every cycle, which on a patch that is already dark means the
    note disappears every other beat.

    Evaluation allocates nothing and touches no global state, so it can run per
    sample rather than per block. That matters: a modulation source updated once
    a block is a source that steps, and a stepped source on a filter cutoff is
    audible as a buzz at the block rate, which is a sound somebody will report
    as a bug in the filter.

    No JUCE, so all of the above can be tested.
*/
namespace Synth
{
namespace ModMatrix
{

//==============================================================================
/** Whether a source swings both ways around zero or only upward.

    Not cosmetic. It decides what the polarity transform has to do to convert
    one into the other, and getting it wrong for a source turns a vibrato into
    a pitch bend that never comes back.
*/
enum class Polarity
{
    unipolar = 0,   /**< 0 to 1: an envelope, velocity, a macro */
    bipolar         /**< -1 to 1: an LFO, key tracking, a random value */
};

/** What a routing does to its source before using it. */
enum class Shape
{
    asIs = 0,       /**< whatever the source already is */
    unipolar,       /**< fold a bipolar source up into 0 to 1 */
    bipolar,        /**< stretch a unipolar source out into -1 to 1 */
    inverted        /**< as is, then negated: the same as a negative depth,
                         and worth having separately because a patch reads
                         better with the sign on the shape than hidden in a
                         number somebody has to notice is below zero */
};

//==============================================================================
struct SourceInfo
{
    const char* id;       /**< stable, saved in projects, never reused */
    const char* name;     /**< what a person reads */
    Polarity polarity;
};

struct DestinationInfo
{
    const char* id;
    const char* name;
    const char* unit;
    float range;          /**< what a depth of one is worth, in `unit` */
};

//==============================================================================
/** Everything that can drive something.

    The order is the order they appear in a menu, and nothing else depends on
    it: ids are what gets saved. Grouped so that a patch being read by a person
    reads top to bottom in the order the engine runs.
*/
inline const std::vector<SourceInfo>& sources()
{
    static const std::vector<SourceInfo> all
    {
        { "env1",     "Env 1",      Polarity::unipolar },   // wired to the amplifier
        { "env2",     "Env 2",      Polarity::unipolar },   // wired to the filter
        { "env3",     "Env 3",      Polarity::unipolar },
        { "env4",     "Env 4",      Polarity::unipolar },

        { "lfo1",     "LFO 1",      Polarity::bipolar },
        { "lfo2",     "LFO 2",      Polarity::bipolar },
        { "lfo3",     "LFO 3",      Polarity::bipolar },
        { "lfo4",     "LFO 4",      Polarity::bipolar },

        { "velocity", "Velocity",   Polarity::unipolar },
        { "keytrack", "Key",        Polarity::bipolar },    // zero at middle C
        { "modwheel", "Mod wheel",  Polarity::unipolar },
        { "pressure", "Pressure",   Polarity::unipolar },
        { "bend",     "Pitch bend", Polarity::bipolar },

        // Drawn once when the note starts and held for its whole life, so a
        // chord gets a different value per note rather than one shared value.
        // This is what stops a stack of unison voices sounding like one voice
        // played loudly.
        { "random",   "Random",     Polarity::bipolar },

        // Where this voice sits in the unison stack, from the far left of it
        // to the far right. Pointed at pitch it is a detune, at pan a spread,
        // at the wavetable position it is a stack where each voice is a
        // slightly different waveform, which is the thing that makes a big
        // unison sound like an ensemble rather than a chorus.
        { "unison",   "Unison",     Polarity::bipolar },

        { "gate",     "Gate",       Polarity::unipolar },   // 1 while held

        { "macro1",   "Macro 1",    Polarity::unipolar },
        { "macro2",   "Macro 2",    Polarity::unipolar },
        { "macro3",   "Macro 3",    Polarity::unipolar },
        { "macro4",   "Macro 4",    Polarity::unipolar },
    };

    return all;
}

/** Everything that can be driven.

    `range` is what a depth of one is worth. These are chosen as the most
    anybody would reasonably ask for rather than the most the parameter can
    take, because a slot at full depth should be a strong musical move and not
    an unusable one: a patch where every interesting setting lives in the first
    two percent of the control is a patch nobody can dial in.
*/
inline const std::vector<DestinationInfo>& destinations()
{
    static const std::vector<DestinationInfo> all
    {
        // Four octaves each way covers every tuned move anybody makes, and
        // leaves the far end of the control for the deliberately broken ones.
        { "osc1pitch",   "Osc 1 pitch",    " st",  48.0f },
        { "osc2pitch",   "Osc 2 pitch",    " st",  48.0f },

        // The whole table, so full depth sweeps every waveform in it.
        { "osc1pos",     "Osc 1 position", "",      1.0f },
        { "osc2pos",     "Osc 2 position", "",      1.0f },

        { "osc1level",   "Osc 1 level",    "",      1.0f },
        { "osc2level",   "Osc 2 level",    "",      1.0f },
        { "sublevel",    "Sub level",      "",      1.0f },

        // A whole cycle, so full depth is a complete phase rotation.
        { "osc1phase",   "Osc 1 phase",    "",      1.0f },
        { "osc2phase",   "Osc 2 phase",    "",      1.0f },

        { "fm",          "FM amount",      "",      1.0f },

        { "noiselevel",  "Noise level",    "",      1.0f },
        { "noisecolour", "Noise colour",   "",      1.0f },

        // Eight octaves is the whole audible range, so an envelope at full
        // depth takes the filter from shut to open wherever it is resting.
        { "cutoff",      "Cutoff",         " oct",  8.0f },
        { "resonance",   "Resonance",      "",      1.0f },
        { "drive",       "Drive",          "",      1.0f },

        { "amp",         "Amp",            "",      1.0f },
        { "pan",         "Pan",            "",      1.0f },

        { "unisondetune","Unison detune",  "",      1.0f },
        { "unisonspread","Unison spread",  "",      1.0f },

        // Six octaves takes an LFO from a slow sweep to an audio rate, which
        // is where it stops being modulation and becomes a sound of its own.
        { "lfo1rate",    "LFO 1 rate",     " oct",  6.0f },
        { "lfo2rate",    "LFO 2 rate",     " oct",  6.0f },
        { "lfo3rate",    "LFO 3 rate",     " oct",  6.0f },
        { "lfo4rate",    "LFO 4 rate",     " oct",  6.0f },

        { "glide",       "Glide",          "",      1.0f },
    };

    return all;
}

inline int numSources()      { return (int) sources().size(); }
inline int numDestinations() { return (int) destinations().size(); }

//==============================================================================
/** The index for an id, or -1.

    Minus one rather than a default, because a routing that cannot be resolved
    has to be dropped rather than quietly pointed somewhere else. Pointing it
    somewhere else is how a project saved by a later version comes back with
    the vibrato on the amplitude.
*/
inline int sourceFromId (const char* id) noexcept
{
    if (id == nullptr)
        return -1;

    const auto& all = sources();

    for (int i = 0; i < (int) all.size(); ++i)
        if (std::strcmp (all[(std::size_t) i].id, id) == 0)
            return i;

    return -1;
}

inline int destinationFromId (const char* id) noexcept
{
    if (id == nullptr)
        return -1;

    const auto& all = destinations();

    for (int i = 0; i < (int) all.size(); ++i)
        if (std::strcmp (all[(std::size_t) i].id, id) == 0)
            return i;

    return -1;
}

inline const char* sourceId (int index) noexcept
{
    return index >= 0 && index < numSources() ? sources()[(std::size_t) index].id : "";
}

inline const char* destinationId (int index) noexcept
{
    return index >= 0 && index < numDestinations()
         ? destinations()[(std::size_t) index].id : "";
}

//==============================================================================
/** One row of the matrix. */
struct Route
{
    int   source      = -1;    /**< -1 is an empty row */
    int   destination = -1;
    float depth       = 0.0f;  /**< -1 to 1, in units of the destination's range */
    int   via         = -1;    /**< optional: scales `depth`, -1 for none */
    Shape shape       = Shape::asIs;
    bool  enabled     = true;

    bool isWired() const noexcept { return source >= 0 && destination >= 0; }
};

/** How many rows a patch can hold.

    Thirty two is more than any patch anybody has shown me uses, and the whole
    table is walked per sample, so there is a real cost to making it larger.
    A fixed array rather than a vector because this is read on the audio
    thread and must not allocate.
*/
inline constexpr int maxRoutes = 32;

struct Patch
{
    Route routes[maxRoutes];
    int   count = 0;

    void clear() noexcept
    {
        for (int i = 0; i < maxRoutes; ++i)
            routes[i] = Route {};

        count = 0;
    }

    /** Returns false when the table is full, rather than dropping the routing
        on the floor. A patch that silently lost its last connection is worse
        than one that could not be given it. */
    bool add (const Route& route) noexcept
    {
        if (count >= maxRoutes)
            return false;

        routes[count++] = route;
        return true;
    }

    bool add (int source, int destination, float depth,
              Shape shape = Shape::asIs, int via = -1) noexcept
    {
        Route r;
        r.source = source;
        r.destination = destination;
        r.depth = depth;
        r.shape = shape;
        r.via = via;

        return add (r);
    }
};

//==============================================================================
/** A source's value, put into the shape a routing asked for.

    `value` is assumed to be inside the range its declared polarity promises.
    It is clamped rather than trusted, because an envelope that overshoots, or
    a macro arriving from a project file written by hand, would otherwise push
    a destination somewhere no clamp downstream expects.
*/
inline float shaped (float value, Polarity polarity, Shape shape) noexcept
{
    const float low = polarity == Polarity::bipolar ? -1.0f : 0.0f;

    if (value < low)  value = low;
    if (value > 1.0f) value = 1.0f;

    switch (shape)
    {
        case Shape::asIs:
            return value;

        case Shape::inverted:
            return -value;

        case Shape::unipolar:
            // A bipolar source folded up so it only ever adds. Half scale and
            // offset rather than taking the absolute value: the absolute value
            // would double the frequency of an LFO, which is a different
            // sound and not the one anybody asking for a unipolar LFO wants.
            return polarity == Polarity::bipolar ? value * 0.5f + 0.5f : value;

        case Shape::bipolar:
            return polarity == Polarity::unipolar ? value * 2.0f - 1.0f : value;
    }

    return value;
}

//==============================================================================
/** Works out how much each destination is being moved.

    `sourceValues` holds one entry per source, in the order of `sources()`.
    `destinationSums` is written, not accumulated: it is cleared first, so a
    caller need not remember to.

    The answer is in units of each destination's `range`, so a sum of one on
    the cutoff means eight octaves. `apply` below turns that into a value.

    Allocates nothing, reads nothing but its arguments, and is safe to call per
    sample.
*/
inline void evaluate (const Patch& patch,
                      const float* sourceValues, int numSourceValues,
                      float* destinationSums, int numDestinationSums) noexcept
{
    if (destinationSums == nullptr || numDestinationSums <= 0)
        return;

    for (int i = 0; i < numDestinationSums; ++i)
        destinationSums[i] = 0.0f;

    if (sourceValues == nullptr || numSourceValues <= 0)
        return;

    const auto& sourceTable = sources();
    const int rows = patch.count < maxRoutes ? patch.count : maxRoutes;

    for (int i = 0; i < rows; ++i)
    {
        const Route& route = patch.routes[i];

        if (! route.enabled || ! route.isWired())
            continue;

        // Every index is checked against what actually exists rather than
        // trusted. A project written by a later version, or edited by hand,
        // can carry anything at all, and reading past the end of these arrays
        // on the audio thread is a crash rather than a wrong note.
        if (route.source >= numSourceValues || route.source >= (int) sourceTable.size())
            continue;

        if (route.destination >= numDestinationSums)
            continue;

        float amount = shaped (sourceValues[route.source],
                               sourceTable[(std::size_t) route.source].polarity,
                               route.shape);

        if (route.via >= 0 && route.via < numSourceValues
                           && route.via < (int) sourceTable.size())
        {
            // The scaler is always read as unipolar, whatever it is. A via of
            // minus one half would otherwise flip the sign of the routing,
            // which reads as the scaler turning the modulation inside out
            // rather than turning it down, and is not what "only as far as
            // that lets it" means to anybody.
            const float scale = shaped (sourceValues[route.via],
                                        sourceTable[(std::size_t) route.via].polarity,
                                        Shape::unipolar);

            amount *= scale;
        }

        destinationSums[route.destination] += amount * route.depth;
    }
}

/** The value a parameter should take, given where the patch left it and what
    the matrix is doing to it.

    `base` and the result are in the destination's own units. The sum is
    multiplied by the destination's declared range and added, then held inside
    the limits the caller gives, because several routings on one destination
    can easily add up past anything sensible and a filter cutoff below zero is
    not a quiet filter but a broken one.
*/
inline float apply (int destination, float base, float sum,
                    float lowest, float highest) noexcept
{
    if (destination >= 0 && destination < numDestinations())
        base += sum * destinations()[(std::size_t) destination].range;

    if (base < lowest)  return lowest;
    if (base > highest) return highest;

    return base;
}

} // namespace ModMatrix
} // namespace Synth
