#pragma once
#include "PanOneParams.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

//==============================================================================
/** Growing a new patch out of the one that is playing.

    Two instruments do this well and they do it the same way underneath.
    Synplant plants a patch as a seed and grows branches from it, where how far
    a branch is pulled decides how far its sound wanders, and the player keeps
    the ones they like and grows from those: selective breeding with the ear as
    the judge. Omnisphere 3's Mutations makes a variation of any patch in one
    click and is praised for staying musical, which by Spectrasonics' account
    comes from swapping a sound source for a related one rather than throwing
    every parameter at random. Both are proprietary and neither publishes an
    algorithm, so **there is no standard to follow here**, and what follows is
    a design rather than a specification. Where it borrows, it says from what.

    The operator itself is the one evolution strategies have used since
    Rechenberg and Schwefel: every gene that moves takes a normally distributed
    step, and one number, the step size, decides how adventurous a generation
    is (Beyer and Schwefel, "Evolution strategies: a comprehensive
    introduction", Natural Computing 1, 2002). Here that number is the amount
    the player asks for.

    Four decisions in here are what keep it musical, and each one is the
    answer to a way it would otherwise go wrong.

    **Steps are taken along the knob, not along the number.** A cutoff moved
    by a fixed number of hertz is an octave at the bottom and nothing at the
    top, and an envelope time moved by a fixed number of seconds is a pluck
    turned into a pad at one end and no change at the other. Every continuous
    control already says where the middle of its travel should land, which is
    how its knob is skewed, so a step is taken in that same travel. The skew
    is JUCE's own formula from `NormalisableRange::setSkewForCentre`, written
    out here because this file builds without JUCE, and the test checks the
    two agree.

    **Only the sound is bred.** Synplant breeds a curated forty eight genes,
    not everything the engine has, and the reason carries over: a mutation
    that moves the pan, a macro, the glide or the gate's mode is not a new
    sound but a different instrument setup, and one that rewires the matrix
    is a different patch altogether. So pan, glide, the macros, the gate mode,
    every on and off switch and the whole of the matrix's wiring are left
    alone. A wired row's depth is bred, because that is how much of a
    modulation there is rather than whether there is one, and it never
    crosses zero, because a depth that changes sign is an inverted envelope.

    **Pitch stays musical unless asked.** Synplant has an Atonality control
    for how far a mutation may wander from musical pitch, and it is the right
    idea: a detune that lands on a random fraction of a semitone is out of
    tune, which is a different thing from being adventurous. At no atonality
    the second oscillator only ever moves to a consonant interval, keeping
    whatever fine detune the seed already had, and depths pointed at an
    oscillator's pitch do not move at all. Atonality lets both loose in
    proportion.

    **A choice is only swapped for a relative.** This is the Omnisphere
    lesson. A wavetable is swapped within its own category until the amount
    is past halfway, and a filter that is a low pass, a band pass or a high
    pass is only swapped among those three: a notch, a bell or a shelf on a
    seed is a deliberate choice, and turning a low pass into an all pass is
    not a variation, it is a silence.

    None of that can promise the result makes a sound, because whether a
    patch makes a sound is a property of the whole patch and not of any one
    control. So every candidate is played, headless, before the player hears
    it, and is thrown away if it is silent, far louder or quieter than the
    seed, or would clip a chord. That is what Genopatch does at the end of
    its search, render and score, and it is the reason a breed can never land
    on a dud. The thresholds are the preset bank's own, from
    `PanOneParamsTest`, so a bred patch is held to what a shipped one is.

    The random numbers are this file's own and not `<random>`'s. The standard
    engines are specified exactly but the distributions are not: the same
    seed through `std::normal_distribution` gives a different sequence under
    MSVC than under libstdc++. A seed that breeds one patch on Windows and
    another on macOS is a project that sounds different depending on who
    opened it.
*/
namespace PanOneBreed
{
    using Spec = PanOneParams::Spec;
    using Patch = PanOneParams::Patch;

    //==========================================================================
    /** Where a value sits along its knob, from nothing to one.

        JUCE's `setSkewForCentre` picks the exponent that puts `centre`
        halfway along, and `convertTo0to1` raises the proportion to it. That
        is the non symmetric form, which is what every continuous control in
        the table is built with.
    */
    inline double skewOf (const Spec& spec)
    {
        const double span = (double) spec.maximum - (double) spec.minimum;
        const double proportion = ((double) spec.centre - (double) spec.minimum) / span;

        if (! (span > 0.0) || ! (proportion > 0.0) || ! (proportion < 1.0))
            return 1.0;

        return std::log (0.5) / std::log (proportion);
    }

    inline double toTravel (const Spec& spec, double value)
    {
        const double span = (double) spec.maximum - (double) spec.minimum;

        if (! (span > 0.0))
            return 0.0;

        double proportion = (value - (double) spec.minimum) / span;
        proportion = proportion < 0.0 ? 0.0 : (proportion > 1.0 ? 1.0 : proportion);

        return std::pow (proportion, skewOf (spec));
    }

    inline double fromTravel (const Spec& spec, double travel)
    {
        travel = travel < 0.0 ? 0.0 : (travel > 1.0 ? 1.0 : travel);

        const double proportion = travel > 0.0 ? std::exp (std::log (travel) / skewOf (spec))
                                               : 0.0;

        return (double) spec.minimum + ((double) spec.maximum - (double) spec.minimum) * proportion;
    }

    //==========================================================================
    /** Deterministic dice: splitmix64 (Steele, Lea and Flood, OOPSLA 2014)
        for the bits, and Box and Muller (1958) for the normal draws, so that
        the same seed is the same patch on every platform. */
    class Dice
    {
    public:
        explicit Dice (std::uint64_t seed) noexcept : state (seed) {}

        std::uint64_t next() noexcept
        {
            std::uint64_t z = (state += 0x9e3779b97f4a7c15ull);
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
            return z ^ (z >> 31);
        }

        /** In [0, 1), from the top fifty three bits. */
        double uniform() noexcept
        {
            return (double) (next() >> 11) * (1.0 / 9007199254740992.0);
        }

        bool chance (double probability) noexcept { return uniform() < probability; }

        /** Nought to count minus one. */
        int below (int count) noexcept
        {
            if (count <= 1)
                return 0;

            return (int) (next() % (std::uint64_t) count);
        }

        /** One standard normal draw.

            Box and Muller's pair gives two at a time; only one is used,
            which wastes a draw and keeps the sequence a function of the
            number of calls alone, with no hidden spare carried between them.
            The one is taken away from one rather than used directly so the
            logarithm can never see a zero.
        */
        double normal() noexcept
        {
            const double u1 = 1.0 - uniform();
            const double u2 = uniform();
            return std::sqrt (-2.0 * std::log (u1)) * std::cos (6.283185307179586 * u2);
        }

    private:
        std::uint64_t state;
    };

    //==========================================================================
    /** What the player asks for. */
    struct Settings
    {
        /** How far to wander, from a nudge to unrecognisable. Synplant's
            branch length, Omnisphere's subtle to extreme. */
        double amount = 0.3;

        /** How far from musical pitch a mutation may go. Synplant's name for
            it, and its meaning: nothing keeps every interval consonant. */
        double atonality = 0.0;
    };

    /** How each control takes part in breeding. */
    enum class Role
    {
        fixed,      /**< never moved: setup, performance state, wiring */
        travel,     /**< a normal step along its knob */
        table,      /**< a wavetable, swapped within its category first */
        interval,   /**< the second oscillator's detune */
        filter,     /**< swapped only among low, band and high pass */
        choice,     /**< any entry of its list */
        depth,      /**< a matrix depth, if the row is wired */
        pitchDepth  /**< a matrix depth pointed at an oscillator's pitch */
    };

    /** Which matrix row a control belongs to and which of the row's six it
        is, or minus one for both when it is not in the matrix. */
    inline void routeOf (std::size_t index, int& row, int& slot)
    {
        const std::size_t first = PanOneParams::firstRouteControl();

        if (index < first)
        {
            row = slot = -1;
            return;
        }

        row = (int) ((index - first) / (std::size_t) PanOneParams::controlsPerRoute);
        slot = (int) ((index - first) % (std::size_t) PanOneParams::controlsPerRoute);
    }

    inline bool startsWith (const char* text, const char* prefix)
    {
        return std::strncmp (text, prefix, std::strlen (prefix)) == 0;
    }

    inline bool endsWith (const char* text, const char* suffix)
    {
        const std::size_t a = std::strlen (text), b = std::strlen (suffix);
        return a >= b && std::strcmp (text + a - b, suffix) == 0;
    }

    /** The role of one control, given the patch it is in.

        The patch is needed for the matrix: a depth on an empty or disabled
        row is a number nothing reads, and breeding it would make the
        instrument report a changed patch that sounds the same.
    */
    inline Role roleOf (std::size_t index, const Patch& patch)
    {
        const auto& specs = PanOneParams::all();

        if (index >= specs.size())
            return Role::fixed;

        int row = -1, slot = -1;
        routeOf (index, row, slot);

        if (row >= 0)
        {
            // Slot two is the depth; every other slot is the wiring.
            if (slot != 2 || row >= Synth::ModMatrix::maxRoutes)
                return Role::fixed;

            const auto& route = patch.matrix.routes[row];

            if (! route.isWired() || ! route.enabled)
                return Role::fixed;

            static const int osc1Pitch = Synth::ModMatrix::destinationFromId ("osc1pitch");
            static const int osc2Pitch = Synth::ModMatrix::destinationFromId ("osc2pitch");

            if (route.destination == osc1Pitch || route.destination == osc2Pitch)
                return Role::pitchDepth;

            return Role::depth;
        }

        const char* id = specs[index].id;

        // Setup and performance rather than sound. See the header comment.
        if (std::strcmp (id, "pan") == 0 || std::strcmp (id, "glide") == 0
            || std::strcmp (id, "gatemode") == 0 || startsWith (id, "macro"))
            return Role::fixed;

        // Every on and off switch, and the LFO's division, which only means
        // anything when its sync switch is on and so would be a change that
        // is reported and not heard.
        if (specs[index].choices == &PanOneParams::offOrOn
            || (startsWith (id, "lfo") && endsWith (id, "div")))
            return Role::fixed;

        if (std::strcmp (id, "osc1table") == 0 || std::strcmp (id, "osc2table") == 0)
            return Role::table;

        if (std::strcmp (id, "detune") == 0)
            return Role::interval;

        if (std::strcmp (id, "filtertype") == 0)
            return Role::filter;

        if (specs[index].kind == PanOneParams::Kind::choice)
            return Role::choice;

        return Role::travel;
    }

    //==========================================================================
    /** The intervals a tonal mutation may put the second oscillator on:
        unison, fourths, fifths, octaves and the twelfth, either way. Thirds
        are left out on purpose: a third between two oscillators of one voice
        is a chord on every note, which turns a bass into a major triad and is
        a choice somebody makes rather than a variation they stumble on. */
    inline const std::vector<double>& consonantIntervals()
    {
        static const std::vector<double> list { -24.0, -12.0, -7.0, -5.0, 0.0,
                                                5.0, 7.0, 12.0, 19.0, 24.0 };
        return list;
    }

    /** Which filter responses are relatives, by id. */
    inline bool isPassFilter (const char* id)
    {
        return std::strcmp (id, "lowpass") == 0 || std::strcmp (id, "bandpass") == 0
            || std::strcmp (id, "bandpassq") == 0 || std::strcmp (id, "highpass") == 0;
    }

    /** Reflects a value back into nothing to one rather than clamping it.

        Clamping piles every step that overshoots onto the end stop, so a
        control near either end would arrive exactly at it far more often
        than anywhere else, and a fold mix of exactly one or a sustain of
        exactly nothing is an extreme the player did not ask for.
    */
    inline double reflect (double x)
    {
        for (int i = 0; i < 8 && (x < 0.0 || x > 1.0); ++i)
            x = x < 0.0 ? -x : 2.0 - x;

        return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
    }

    //==========================================================================
    /** One mutation of a set of values, without listening to it.

        `values` is one float per control in table order, the way
        `PanOneParams::fromPatch` writes them. The result is the same size,
        and every value in it is inside its control's range.
    */
    inline std::vector<float> mutate (const std::vector<float>& values,
                                      const Settings& settings, Dice& dice)
    {
        const auto& specs = PanOneParams::all();

        std::vector<float> out = values;
        out.resize (specs.size());

        Patch seed;
        PanOneParams::toPatch (out, seed);

        const double amount = settings.amount < 0.0 ? 0.0 : (settings.amount > 1.0 ? 1.0 : settings.amount);
        const double atonal = settings.atonality < 0.0 ? 0.0
                                                       : (settings.atonality > 1.0 ? 1.0 : settings.atonality);

        // A quarter of the genes move on a nudge and three quarters at full
        // stretch, and the step grows with the amount too. Moving every gene
        // a little on the smallest setting makes every branch sound equally
        // unlike the seed; moving a few is what makes a small branch a small
        // change. The figures were set by ear and are not from anywhere.
        const double moveChance = 0.25 + 0.5 * amount;
        const double step = 0.3 * amount;
        const double swapChance = 0.3 * amount;

        for (std::size_t i = 0; i < specs.size(); ++i)
        {
            const auto& spec = specs[i];
            const auto role = roleOf (i, seed);

            switch (role)
            {
                case Role::fixed:
                    break;

                case Role::travel:
                {
                    if (! dice.chance (moveChance))
                        break;

                    const double t = reflect (toTravel (spec, out[i]) + step * dice.normal());
                    out[i] = (float) fromTravel (spec, t);
                    break;
                }

                case Role::depth:
                case Role::pitchDepth:
                {
                    const double scale = role == Role::pitchDepth ? atonal : 1.0;

                    if (scale <= 0.0 || ! dice.chance (moveChance))
                        break;

                    // In magnitude, kept on the side of zero it started on.
                    const double sign = out[i] < 0.0f ? -1.0 : 1.0;
                    const double magnitude = reflect (std::fabs ((double) out[i])
                                                      + 0.5 * step * scale * dice.normal());
                    out[i] = (float) (sign * magnitude);
                    break;
                }

                case Role::interval:
                {
                    const double now = out[i];

                    // The fine detune the seed carries, kept through an
                    // interval change: a pair of oscillators seven cents
                    // apart is a chorus, and landing exactly on the fifth
                    // would quietly take that away.
                    const double fine = now - std::round (now);
                    double next = now;

                    if (dice.chance (swapChance * (1.0 - 0.5 * atonal)))
                    {
                        const auto& list = consonantIntervals();
                        const double interval = list[(std::size_t) dice.below ((int) list.size())];
                        next = interval + fine;

                        // Two octaves and a fine detune is past the end of
                        // the control; the interval matters more than the
                        // chorus.
                        if (next < spec.minimum || next > spec.maximum)
                            next = interval;
                    }

                    if (atonal > 0.0 && dice.chance (moveChance))
                        next += 12.0 * step * atonal * dice.normal();

                    next = next < spec.minimum ? spec.minimum : (next > spec.maximum ? spec.maximum : next);
                    out[i] = (float) next;
                    break;
                }

                case Role::table:
                {
                    if (! dice.chance (swapChance))
                        break;

                    const auto& catalogue = Synth::WaveBank::catalogue();
                    const int current = PanOneParams::toIndex (out[i], (int) catalogue.size());

                    // Relatives first, the whole bank only past halfway.
                    std::vector<int> pool;

                    for (int t = 0; t < (int) catalogue.size(); ++t)
                        if (t != current
                            && (amount > 0.5
                                || std::strcmp (catalogue[(std::size_t) t].category,
                                                catalogue[(std::size_t) current].category) == 0))
                            pool.push_back (t);

                    if (! pool.empty())
                        out[i] = (float) pool[(std::size_t) dice.below ((int) pool.size())];

                    break;
                }

                case Role::filter:
                {
                    const auto& list = spec.choices();
                    const int current = PanOneParams::toIndex (out[i], (int) list.size());

                    if (! isPassFilter (list[(std::size_t) current].id) || ! dice.chance (swapChance))
                        break;

                    std::vector<int> pool;

                    for (int t = 0; t < (int) list.size(); ++t)
                        if (t != current && isPassFilter (list[(std::size_t) t].id))
                            pool.push_back (t);

                    if (! pool.empty())
                        out[i] = (float) pool[(std::size_t) dice.below ((int) pool.size())];

                    break;
                }

                case Role::choice:
                {
                    if (! dice.chance (swapChance))
                        break;

                    const int count = (int) spec.choices().size();
                    const int current = PanOneParams::toIndex (out[i], count);
                    const int pick = dice.below (count - 1);

                    out[i] = (float) (pick >= current ? pick + 1 : pick);
                    break;
                }
            }
        }

        return out;
    }

    //==========================================================================
    /** What a patch sounds like, in the two numbers the preset bank is held
        to: the loudest tenth of a second, and the peak. */
    struct Hearing
    {
        double loudest = 0.0;
        double peak = 0.0;
        bool   finite = true;
    };

    /** Plays one held note of a patch and measures it.

        A voice of its own, never one of the instrument's, so this can run on
        the message thread while the audio thread plays. A C below middle C
        at a firm velocity, held for `seconds`. One and a fifth by default,
        short of the preset test's three, because this runs while somebody
        waits for a button: a second of one voice costs about fifty
        milliseconds, and a branch takes one listen for the seed and usually
        one or two for candidates.
    */
    inline Hearing listen (const Patch& patch, const Synth::WaveBank& bank,
                           double seconds = 1.2, double rate = 48000.0)
    {
        Synth::Voice voice;
        voice.setSampleRate (rate);
        voice.setBank (&bank);
        voice.setSeed (1);
        voice.setPatch (patch);
        voice.start (48, 0.9f);

        const int samples = (int) (rate * seconds);
        const int window = (int) (rate * 0.1);

        std::vector<double> squares ((std::size_t) window, 0.0);

        Hearing heard;
        double running = 0.0;

        for (int i = 0; i < samples; ++i)
        {
            const double s = (double) voice.nextSample();

            if (! std::isfinite (s))
            {
                heard.finite = false;
                return heard;
            }

            heard.peak = std::max (heard.peak, std::fabs (s));

            auto& slot = squares[(std::size_t) (i % window)];
            running += s * s - slot;
            slot = s * s;

            if (i >= window)
                heard.loudest = std::max (heard.loudest,
                                          std::sqrt (std::max (0.0, running) / (double) window));
        }

        return heard;
    }

    /** Whether a candidate is fit to hand to the player, given the seed it
        grew from.

        The floor and the ceiling are the preset bank's: quieter than a
        fiftieth of full scale at its loudest is a patch with something wired
        wrong, and a voice peaking past one and a half clips once a chord is
        held. Between those, a candidate has to stay within
        twelve decibels of its seed, because a branch that is suddenly four
        times louder is a jump in the monitors rather than a variation, and
        Synplant keeps volume as a bulb control outside the genes for the
        same reason.
    */
    inline bool fit (const Hearing& candidate, const Hearing& seed)
    {
        if (! candidate.finite || ! (candidate.peak < 1.5) || ! (candidate.loudest > 0.02))
            return false;

        if (! (seed.loudest > 0.0))
            return true;

        const double ratio = candidate.loudest / seed.loudest;
        return ratio > 0.25 && ratio < 4.0;
    }

    //==========================================================================
    struct Result
    {
        std::vector<float> values;  /**< the branch, or the seed if nothing was fit */
        bool    grown = false;      /**< whether a fit candidate was found */
        int     tries = 0;
        Hearing heard;
    };

    /** Grows one branch: mutates, listens, and keeps the first fit result.

        Each try draws from the same dice, so a seed number names one branch
        exactly, whichever try it landed on. When nothing fits in `maxTries`
        the seed comes back unchanged and `grown` says so, which is better
        than handing over a patch known to be silent.
    */
    inline Result grow (const std::vector<float>& seedValues, const Settings& settings,
                        std::uint64_t seedNumber, const Synth::WaveBank& bank,
                        int maxTries = 12)
    {
        Patch seedPatch;
        PanOneParams::toPatch (seedValues, seedPatch);

        // A drone whose attack is longer than the short listen is quieter
        // than the floor at its own loudest, and every faithful branch of it
        // would be too, so nothing would ever be fit. Such a seed is heard
        // for as long as the preset test hears every preset, and so is
        // everything grown from it: a candidate is only ever compared with
        // its seed over the same length.
        double seconds = 1.2;
        Hearing seedHeard = listen (seedPatch, bank, seconds);

        if (seedHeard.finite && seedHeard.loudest < 0.02)
        {
            seconds = 3.0;
            seedHeard = listen (seedPatch, bank, seconds);
        }

        Dice dice (seedNumber);
        Result result;
        result.values = seedValues;

        for (int t = 0; t < maxTries; ++t)
        {
            result.tries = t + 1;

            auto candidate = mutate (seedValues, settings, dice);

            Patch patch;
            PanOneParams::toPatch (candidate, patch);

            const Hearing heard = listen (patch, bank, seconds);

            if (fit (heard, seedHeard))
            {
                result.values = std::move (candidate);
                result.grown = true;
                result.heard = heard;
                return result;
            }
        }

        result.heard = seedHeard;
        return result;
    }

    //==========================================================================
    /** A point between two patches: nothing is `a`, one is `b`.

        Synplant's Tween Branches mod morphs neighbouring branches across the
        keyboard or velocity, and Omnisphere's Quadzone crossfades whole
        layers on one fader. Both are a sound that can be stood anywhere
        between two others, which is what this is.

        Continuous controls move along their knobs, for the reason mutations
        do: halfway between a cutoff of 200 Hz and 3.2 kHz should be 800, not
        1.7 kHz. Choices cannot be halfway, so they change at the middle.

        Two things are not interpolated even though they are numbers. A
        matrix depth only moves smoothly when the row is wired the same way
        on both sides; otherwise the wiring changes at the middle and the
        depth has to change with it, or the halfway patch would carry one
        row's depth on another row's routing. And the detune changes at the
        middle when the two sides are a semitone or more apart, because a
        morph from unison to a fifth that passes through every microtone on
        the way is out of tune for most of its length. Within a semitone it is
        a fine detune and moves smoothly.
    */
    inline std::vector<float> tween (const std::vector<float>& a, const std::vector<float>& b,
                                     double position)
    {
        const auto& specs = PanOneParams::all();
        const double t = position < 0.0 ? 0.0 : (position > 1.0 ? 1.0 : position);
        const bool second = t >= 0.5;

        std::vector<float> out (specs.size(), 0.0f);

        for (std::size_t i = 0; i < specs.size(); ++i)
        {
            const float from = i < a.size() ? a[i] : 0.0f;
            const float to = i < b.size() ? b[i] : from;
            const auto& spec = specs[i];

            int row = -1, slot = -1;
            routeOf (i, row, slot);

            if (spec.kind == PanOneParams::Kind::choice)
            {
                out[i] = second ? to : from;
                continue;
            }

            if (row >= 0)
            {
                // The depth of a row: smooth only when the other five
                // controls of the row agree on both sides.
                const std::size_t first = i - (std::size_t) slot;
                bool same = true;

                for (int k = 0; k < PanOneParams::controlsPerRoute; ++k)
                    if (k != slot)
                    {
                        const std::size_t j = first + (std::size_t) k;
                        const float x = j < a.size() ? a[j] : 0.0f;
                        const float y = j < b.size() ? b[j] : x;
                        same = same && std::lround (x) == std::lround (y);
                    }

                if (! same)
                {
                    out[i] = second ? to : from;
                    continue;
                }

                out[i] = (float) ((double) from + ((double) to - (double) from) * t);
                continue;
            }

            if (std::strcmp (spec.id, "detune") == 0 && std::fabs (to - from) >= 1.0f)
            {
                out[i] = second ? to : from;
                continue;
            }

            const double ta = toTravel (spec, from);
            const double tb = toTravel (spec, to);
            out[i] = (float) fromTravel (spec, ta + (tb - ta) * t);
        }

        return out;
    }
}
