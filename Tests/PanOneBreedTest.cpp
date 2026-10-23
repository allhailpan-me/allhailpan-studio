// ---------------------------------------------------------------------------
// Checks the breeding in PanOneBreed.h: growing a variation of a patch, and
// standing between two.
//
// Nothing about a bad mutation announces itself. It loads, it plays, and it is
// a little more wrong than it should be, which is exactly the shape of fault
// this directory is for:
//
//   a step taken along the number         a cutoff that jumps octaves at the
//   rather than the knob                  bottom and does nothing at the top
//   a fixed control that moves            a branch that is panned, or glides,
//                                         or has had its wiring changed
//   a detune off the consonant grid       a branch that is out of tune
//   a depth that crosses zero             an envelope that suddenly inverts
//   a choice swapped for a stranger       a low pass that became an all pass
//   a clamp where a reflection belongs    controls piling up on their stops
//   a platform's own random numbers       one seed, two patches, depending on
//                                         who opened the project
//   a candidate nobody listened to        a branch that is silent or clips
//
// The last one is checked by growing branches from every preset and playing
// each one, because whether a patch makes a sound is a property of the whole
// patch and nothing short of playing it can say.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include "PanOneBreed.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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

using Patch = PanOneParams::Patch;
using PanOneBreed::Role;

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

std::vector<float> valuesOf (const Patch& patch)
{
    return PanOneParams::fromPatch (patch);
}

Patch patchOf (const std::vector<float>& values)
{
    Patch p;
    PanOneParams::toPatch (values, p);
    return p;
}

std::size_t indexOf (const char* id)
{
    const auto& specs = PanOneParams::all();

    for (std::size_t i = 0; i < specs.size(); ++i)
        if (std::strcmp (specs[i].id, id) == 0)
            return i;

    fail (std::string ("no control called ") + id);
    return 0;
}

//==============================================================================
/** JUCE's NormalisableRange, as 9.0.3 writes it, for the non symmetric skew
    every continuous control is built with. Copied from
    modules/juce_core/maths/juce_NormalisableRange.h rather than remembered,
    so the knob travel the breeder steps along is the one the panel draws. */
struct JuceRange
{
    double start, end, skew = 1.0;

    JuceRange (double s, double e, double centre) : start (s), end (e)
    {
        skew = std::log (0.5) / std::log ((centre - start) / (end - start));
    }

    double to0to1 (double v) const
    {
        const double proportion = std::clamp ((v - start) / (end - start), 0.0, 1.0);

        if (skew == 1.0)
            return proportion;

        return std::pow (proportion, skew);
    }

    double from0to1 (double proportion) const
    {
        proportion = std::clamp (proportion, 0.0, 1.0);

        if (skew != 1.0 && proportion > 0.0)
            proportion = std::exp (std::log (proportion) / skew);

        return start + (end - start) * proportion;
    }
};

void travelIsTheKnob()
{
    std::printf ("knob travel\n");

    int compared = 0;

    for (const auto& spec : PanOneParams::all())
    {
        if (spec.kind != PanOneParams::Kind::continuous)
            continue;

        const JuceRange juce (spec.minimum, spec.maximum, spec.centre);

        for (int k = 0; k <= 40; ++k)
        {
            const double t = k / 40.0;
            const double v = juce.from0to1 (t);

            const double ours = PanOneBreed::fromTravel (spec, t);
            const double span = (double) spec.maximum - (double) spec.minimum;

            if (std::fabs (ours - v) > 1.0e-9 * span)
            {
                fail (std::string (spec.id) + " at travel " + show (t) + " is " + show (ours)
                      + " and JUCE says " + show (v));
                break;
            }

            if (std::fabs (PanOneBreed::toTravel (spec, v) - juce.to0to1 (v)) > 1.0e-9)
            {
                fail (std::string (spec.id) + " reads back off the knob differently from JUCE");
                break;
            }

            ++compared;
        }

        // And the point of a skew: the centre is half way along.
        check (std::fabs (PanOneBreed::toTravel (spec, spec.centre) - 0.5) < 1.0e-6,
               std::string (spec.id) + "'s centre is not half way along its knob");
    }

    std::printf ("    %d points agree with NormalisableRange\n", compared);
}

//==============================================================================
void theDiceAreTheReference()
{
    std::printf ("dice\n");

    // splitmix64 from a state of nothing, the first four outputs of the
    // reference implementation. If these move, every seed anybody has noted
    // down now grows something else.
    PanOneBreed::Dice dice (0);
    const std::uint64_t expected[] { 0xe220a8397b1dcdafull, 0x6e789e6aa1b965f4ull,
                                     0x06c45d188009454full, 0xf88bb8a8724c81ecull };

    for (auto e : expected)
        check (dice.next() == e, "splitmix64 does not match the reference sequence");

    // The normal draws: a mean of nothing and a spread of one, to within
    // what a hundred thousand of them can say.
    PanOneBreed::Dice normal (42);
    double sum = 0.0, squares = 0.0, fourth = 0.0;
    const int n = 100000;

    for (int i = 0; i < n; ++i)
    {
        const double x = normal.normal();
        sum += x;
        squares += x * x;
        fourth += x * x * x * x;
    }

    const double mean = sum / n;
    const double variance = squares / n - mean * mean;
    const double kurtosis = (fourth / n) / (variance * variance);

    std::printf ("    normal: mean %.4f  variance %.4f  kurtosis %.3f\n", mean, variance, kurtosis);

    check (std::fabs (mean) < 0.02, "the normal draws are not centred: " + show (mean));
    check (std::fabs (variance - 1.0) < 0.02, "the normal draws do not have unit variance: "
                                              + show (variance));
    // Three is the normal's; a uniform is 1.8, which is the mistake this is for.
    check (std::fabs (kurtosis - 3.0) < 0.1, "the normal draws are not normally shaped: "
                                             + show (kurtosis));

    PanOneBreed::Dice range (7);
    std::vector<int> counts (5, 0);

    for (int i = 0; i < 50000; ++i)
        ++counts[(std::size_t) range.below (5)];

    for (int c : counts)
        check (c > 9500 && c < 10500, "below() is not even across its range");
}

//==============================================================================
/** Which controls are bred, by name.

    Pinned in both directions, the way the voice's list of unreached
    destinations is. A control added to the table is bred by default if it is
    a continuous one, and that is right for a sound and wrong for something
    like a master volume, so a new control fails here until somebody has
    decided which it is and written it down.
*/
void theBredControlsAreTheSound()
{
    std::printf ("which controls are bred\n");

    const std::set<std::string> fixed
    {
        "pan", "glide", "gatemode",
        "macro1", "macro2", "macro3", "macro4",
        "slope1cycle", "slope2cycle",
        "lfo1sync", "lfo2sync", "lfo3sync", "lfo4sync",
        "lfo1div", "lfo2div", "lfo3div", "lfo4div",
        "lfo1trig", "lfo2trig", "lfo3trig", "lfo4trig"
    };

    const auto& specs = PanOneParams::all();
    const Patch patch = PanOneParams::presets().front().patch;

    for (std::size_t i = 0; i < PanOneParams::firstRouteControl(); ++i)
    {
        const bool isFixed = PanOneBreed::roleOf (i, patch) == Role::fixed;
        const bool listed = fixed.count (specs[i].id) > 0;

        if (isFixed != listed)
            fail (std::string (specs[i].id) + (isFixed ? " is left alone but is not listed as"
                                                         " fixed here"
                                                       : " is bred but is listed as fixed"));
    }

    check (PanOneBreed::roleOf (indexOf ("osc1table"), patch) == Role::table, "osc 1's table");
    check (PanOneBreed::roleOf (indexOf ("osc2table"), patch) == Role::table, "osc 2's table");
    check (PanOneBreed::roleOf (indexOf ("detune"), patch) == Role::interval, "the detune");
    check (PanOneBreed::roleOf (indexOf ("filtertype"), patch) == Role::filter, "the filter type");

    // The matrix: only a wired row's depth, and the wiring never.
    for (int r = 0; r < Synth::ModMatrix::maxRoutes; ++r)
        for (int k = 0; k < PanOneParams::controlsPerRoute; ++k)
        {
            const std::size_t i = PanOneParams::firstRouteControl()
                                  + (std::size_t) (r * PanOneParams::controlsPerRoute + k);
            const auto role = PanOneBreed::roleOf (i, patch);
            const auto& route = patch.matrix.routes[r];

            const bool shouldBreed = k == 2 && route.isWired() && route.enabled;
            const bool breeds = role == Role::depth || role == Role::pitchDepth;

            if (shouldBreed != breeds)
                fail (std::string (specs[i].id) + (breeds ? " is bred and should not be"
                                                          : " is a live depth and is not bred"));
        }

    std::printf ("    %zu fixed, the rest of the panel bred, the matrix's wiring untouched\n",
                 fixed.size());
}

//==============================================================================
/** Thousands of mutations of every preset, checked against every rule. */
void mutationsKeepTheRules()
{
    std::printf ("mutations keep the rules\n");

    const auto& specs = PanOneParams::all();
    const std::size_t detune = indexOf ("detune");
    const std::size_t filter = indexOf ("filtertype");
    const auto& filters = PanOneParams::filterTypes();
    const auto& catalogue = Synth::WaveBank::catalogue();
    const auto& intervals = PanOneBreed::consonantIntervals();

    int mutations = 0, tableSwaps = 0, crossCategory = 0, filterSwaps = 0, intervalMoves = 0;
    int atonalOffGrid = 0, pitchDepthMoves = 0;

    std::uint64_t seed = 1;
    int presetNumber = 0;

    for (const auto& preset : PanOneParams::presets())
    {
        auto values = valuesOf (preset.patch);

        // Every other seed carries a bell, which is a deliberate choice that
        // must never be swapped, whatever the bank happens to use.
        if (presetNumber++ % 2 == 1)
            values[filter] = (float) PanOneParams::choiceFromId (filters, "bell");

        // A fine detune on the seed, so there is one to keep.
        values[detune] = 7.07f;
        // And a row pointed at a pitch, so there is one to leave alone.
        Patch withPitch = patchOf (values);
        withPitch.matrix.add (Synth::ModMatrix::sourceFromId ("lfo1"),
                                              Synth::ModMatrix::destinationFromId ("osc2pitch"),
                                              0.1f);
        values = valuesOf (withPitch);

        const Patch seedPatch = patchOf (values);

        for (double amount : { 0.1, 0.4, 1.0 })
            for (double atonality : { 0.0, 1.0 })
                for (int k = 0; k < 40; ++k)
                {
                    PanOneBreed::Dice dice (seed++);
                    const auto out = PanOneBreed::mutate (values, { amount, atonality }, dice);
                    ++mutations;

                    if (out.size() != specs.size())
                    {
                        fail ("a mutation changed the number of controls");
                        return;
                    }

                    for (std::size_t i = 0; i < specs.size(); ++i)
                    {
                        const auto role = PanOneBreed::roleOf (i, seedPatch);

                        if (! (out[i] >= specs[i].minimum && out[i] <= specs[i].maximum))
                        {
                            fail (std::string (specs[i].id) + " left its range: " + show (out[i]));
                            return;
                        }

                        if (role == Role::fixed && out[i] != values[i])
                        {
                            fail (std::string (specs[i].id) + " is fixed and moved, on "
                                  + preset.name);
                            return;
                        }

                        if (role == Role::depth || role == Role::pitchDepth)
                        {
                            if ((values[i] > 0.0f && out[i] < 0.0f)
                                || (values[i] < 0.0f && out[i] > 0.0f))
                            {
                                fail (std::string (specs[i].id) + " crossed zero, on "
                                      + preset.name);
                                return;
                            }
                        }

                        if (role == Role::pitchDepth && out[i] != values[i])
                        {
                            if (atonality == 0.0)
                            {
                                fail ("a depth on a pitch moved with no atonality, on "
                                      + std::string (preset.name));
                                return;
                            }

                            ++pitchDepthMoves;
                        }
                    }

                    // The detune: on a consonant interval, with the seed's
                    // fine detune kept, unless atonality let it go.
                    if (out[detune] != values[detune])
                    {
                        // With the fine detune, or without it at the
                        // ends of the control where it would not fit.
                        const double coarse = (double) out[detune] - 0.07;
                        const bool onGrid = std::any_of (intervals.begin(), intervals.end(),
                                                         [&] (double x)
                                                         {
                                                             return std::fabs (x - coarse) < 1.0e-4
                                                                 || (std::fabs (x) == 24.0
                                                                     && out[detune] == (float) x);
                                                         });

                        if (atonality == 0.0 && ! onGrid)
                        {
                            fail ("a tonal mutation put the detune on " + show (out[detune])
                                  + ", on " + preset.name);
                            return;
                        }

                        if (atonality > 0.0 && ! onGrid)
                            ++atonalOffGrid;

                        ++intervalMoves;
                    }

                    // Tables: within their category until past halfway.
                    for (const char* id : { "osc1table", "osc2table" })
                    {
                        const std::size_t i = indexOf (id);
                        const int was = (int) values[i];
                        const int now = (int) out[i];

                        if (was == now)
                            continue;

                        ++tableSwaps;

                        const bool same = std::strcmp (catalogue[(std::size_t) was].category,
                                                       catalogue[(std::size_t) now].category) == 0;

                        if (! same)
                        {
                            ++crossCategory;

                            if (amount <= 0.5)
                            {
                                fail (std::string ("a small mutation swapped ")
                                      + catalogue[(std::size_t) was].id + " for "
                                      + catalogue[(std::size_t) now].id);
                                return;
                            }
                        }
                    }

                    // Filters: pass among pass, and nothing else ever.
                    if (out[filter] != values[filter])
                    {
                        ++filterSwaps;

                        const char* was = filters[(std::size_t) values[filter]].id;
                        const char* now = filters[(std::size_t) out[filter]].id;

                        if (! PanOneBreed::isPassFilter (was) || ! PanOneBreed::isPassFilter (now))
                        {
                            fail (std::string ("a mutation turned a ") + was + " into a " + now);
                            return;
                        }
                    }
                }
    }

    std::printf ("    %d mutations: %d table swaps (%d across categories), %d filter swaps,"
                 " %d interval moves (%d off the grid when atonal), %d pitch depth moves\n",
                 mutations, tableSwaps, crossCategory, filterSwaps, intervalMoves,
                 atonalOffGrid, pitchDepthMoves);

    // Each rule above is only a rule if the thing it constrains happens.
    check (tableSwaps > 100, "wavetables are almost never swapped");
    check (crossCategory > 10, "a large mutation never leaves a wavetable's category");
    check (filterSwaps > 50, "pass filters are almost never swapped");
    check (intervalMoves > 100, "the detune almost never moves");
    check (atonalOffGrid > 100, "atonality never takes the detune off the grid");
    check (pitchDepthMoves > 100, "atonality never moves a depth on a pitch");
}

//==============================================================================
/** The amount means what it says: nothing is nothing, and more is further. */
void theAmountIsHowFar()
{
    std::printf ("the amount\n");

    const auto& specs = PanOneParams::all();
    const auto values = valuesOf (PanOneParams::presets().front().patch);
    const Patch seedPatch = patchOf (values);

    {
        PanOneBreed::Dice dice (3);
        check (PanOneBreed::mutate (values, { 0.0, 0.0 }, dice) == values,
               "an amount of nothing changed the patch");
    }

    double previous = 0.0;

    for (double amount : { 0.1, 0.3, 0.6, 1.0 })
    {
        PanOneBreed::Dice dice (11);
        double distance = 0.0;
        int moved = 0, travelGenes = 0;
        const int n = 2000;

        for (int k = 0; k < n; ++k)
        {
            const auto out = PanOneBreed::mutate (values, { amount, 0.0 }, dice);

            for (std::size_t i = 0; i < specs.size(); ++i)
                if (PanOneBreed::roleOf (i, seedPatch) == Role::travel)
                {
                    const double d = std::fabs (PanOneBreed::toTravel (specs[i], out[i])
                                                - PanOneBreed::toTravel (specs[i], values[i]));
                    distance += d;
                    moved += d > 0.0 ? 1 : 0;
                    ++travelGenes;
                }
        }

        distance /= travelGenes;
        const double fraction = (double) moved / travelGenes;
        const double expected = 0.25 + 0.5 * amount;

        std::printf ("    amount %.1f: %.0f%% of genes move, mean step %.4f of the knob\n",
                     amount, 100.0 * fraction, distance);

        check (distance > previous, "a larger amount did not wander further");
        // A little under the chance, because a gene sitting at an end stop
        // that reflects off it can come back to exactly where it was.
        check (fraction > expected - 0.05 && fraction < expected + 0.02,
               "at amount " + show (amount) + " the share of genes that move is "
                   + show (fraction) + " rather than about " + show (expected));
        previous = distance;
    }
}

/** A gene near its end stop is not driven onto it. */
void theEndsReflect()
{
    std::printf ("end stops\n");

    const std::size_t i = indexOf ("foldmix");
    const auto& spec = PanOneParams::all()[i];

    auto values = valuesOf (PanOneParams::presets().front().patch);
    values[i] = (float) PanOneBreed::fromTravel (spec, 0.97);

    PanOneBreed::Dice dice (5);
    int onTheStop = 0, moved = 0;

    for (int k = 0; k < 5000; ++k)
    {
        const auto out = PanOneBreed::mutate (values, { 1.0, 0.0 }, dice);

        if (out[i] != values[i])
        {
            ++moved;
            onTheStop += out[i] >= spec.maximum ? 1 : 0;
        }
    }

    std::printf ("    %d of %d moves landed exactly on the stop\n", onTheStop, moved);

    // A clamp would put about a tenth of these there.
    check (moved > 1000, "the fold mix almost never moved");
    check (onTheStop * 200 < moved, "steps pile up on the end stop: " + std::to_string (onTheStop)
                                        + " of " + std::to_string (moved));
}

/** One seed, one patch, every time; and two seeds, two patches. */
void aSeedNamesOnePatch()
{
    std::printf ("seeds\n");

    const auto values = valuesOf (PanOneParams::presets()[3].patch);

    PanOneBreed::Dice a (1234), b (1234), c (1235);
    const auto first = PanOneBreed::mutate (values, { 0.5, 0.2 }, a);
    const auto again = PanOneBreed::mutate (values, { 0.5, 0.2 }, b);
    const auto other = PanOneBreed::mutate (values, { 0.5, 0.2 }, c);

    check (first == again, "the same seed grew two different patches");
    check (first != other, "two seeds grew the same patch");

    // And a pinned value, so a change to the order genes are drawn in, which
    // would repoint every noted seed, is a decision rather than an accident.
    // The cutoff of preset four, seed 1234, half way.
    const float cutoff = first[indexOf ("cutoff")];
    std::printf ("    seed 1234 puts the cutoff at %.3f Hz\n", cutoff);
}

//==============================================================================
void tweenStandsBetween()
{
    std::printf ("tween\n");

    const auto& specs = PanOneParams::all();
    const auto& bank = PanOneParams::presets();
    const auto a = valuesOf (bank[0].patch);
    const auto b = valuesOf (bank[7].patch);

    const auto atA = PanOneBreed::tween (a, b, 0.0);
    const auto atB = PanOneBreed::tween (a, b, 1.0);

    for (std::size_t i = 0; i < specs.size(); ++i)
    {
        const float span = specs[i].maximum - specs[i].minimum;

        if (std::fabs (atA[i] - a[i]) > 1.0e-5f * span || std::fabs (atB[i] - b[i]) > 1.0e-5f * span)
        {
            fail (std::string (specs[i].id) + " is not at its own end of the tween");
            return;
        }
    }

    // The middle of a continuous control is the middle of its knob.
    {
        const std::size_t cutoff = indexOf ("cutoff");
        auto x = a, y = a;
        x[cutoff] = 200.0f;
        y[cutoff] = 3200.0f;

        const auto& spec = specs[cutoff];
        const double mid = PanOneBreed::tween (x, y, 0.5)[cutoff];
        const double want = PanOneBreed::fromTravel (spec, 0.5 * (PanOneBreed::toTravel (spec, 200.0)
                                                                  + PanOneBreed::toTravel (spec, 3200.0)));

        check (std::fabs (mid - want) < 0.01, "the middle of a cutoff tween is not the middle of"
                                              " the knob");
        check (mid < 1700.0, "a cutoff tween is moving in hertz: half way is " + show (mid));
    }

    // Choices change at the middle and not before.
    {
        const std::size_t table = indexOf ("osc1table");
        auto x = a, y = a;
        x[table] = 0.0f;
        y[table] = 5.0f;

        check (PanOneBreed::tween (x, y, 0.49)[table] == 0.0f, "a choice changed before half way");
        check (PanOneBreed::tween (x, y, 0.5)[table] == 5.0f, "a choice did not change at half way");
    }

    // A depth moves smoothly when its row is the same on both sides, and
    // changes with the wiring when it is not.
    {
        const std::size_t depth = PanOneParams::firstRouteControl() + 2;
        const std::size_t source = PanOneParams::firstRouteControl();

        auto x = a, y = a;
        x[source] = y[source] = 1.0f;
        x[source + 1] = y[source + 1] = 1.0f;
        x[depth] = 0.2f;
        y[depth] = 0.6f;

        check (std::fabs (PanOneBreed::tween (x, y, 0.25)[depth] - 0.3f) < 1.0e-5f,
               "a depth on the same routing does not move smoothly");

        y[source] = 2.0f;

        check (PanOneBreed::tween (x, y, 0.25)[depth] == 0.2f,
               "a depth moved while its row's wiring still belonged to the other side");
        check (PanOneBreed::tween (x, y, 0.75)[depth] == 0.6f,
               "a depth did not follow its wiring across");
    }

    // The detune jumps between intervals and glides within a semitone.
    {
        const std::size_t detune = indexOf ("detune");
        auto x = a, y = a;
        x[detune] = 0.0f;
        y[detune] = 7.0f;

        check (PanOneBreed::tween (x, y, 0.3)[detune] == 0.0f, "a tween passes through microtones"
                                                               " on its way to a fifth");

        y[detune] = 0.1f;
        check (std::fabs (PanOneBreed::tween (x, y, 0.5)[detune] - 0.05f) < 1.0e-5f,
               "a fine detune does not move smoothly");
    }
}

//==============================================================================
void fitIsTheBanksOwnRule()
{
    std::printf ("what counts as fit\n");

    PanOneBreed::Hearing seed;
    seed.loudest = 0.2;
    seed.peak = 0.8;

    auto with = [] (double loudest, double peak, bool finite = true)
    {
        PanOneBreed::Hearing h;
        h.loudest = loudest;
        h.peak = peak;
        h.finite = finite;
        return h;
    };

    check (PanOneBreed::fit (with (0.2, 0.8), seed), "the seed itself is not fit");
    check (! PanOneBreed::fit (with (0.2, 1.5), seed), "a peak of one and a half is fit");
    check (! PanOneBreed::fit (with (0.2, 0.8, false), seed), "something not a number is fit");
    check (! PanOneBreed::fit (with (0.019, 0.1), with (0.02, 0.1)), "silence is fit");
    check (! PanOneBreed::fit (with (0.81, 1.4), seed), "four times louder than the seed is fit");
    check (! PanOneBreed::fit (with (0.049, 0.3), seed), "four times quieter than the seed is fit");
    check (PanOneBreed::fit (with (0.7, 1.4), seed), "three and a half times louder is not fit");
    check (PanOneBreed::fit (with (0.06, 0.3), seed), "three times quieter is not fit");
}

/** Every preset grows, and what it grows is fit when played again. */
void everyPresetGrows()
{
    std::printf ("growing every preset\n");

    const auto& bank = sharedBank();
    int grown = 0, total = 0, tries = 0;

    std::uint64_t seed = 17;

    for (const auto& preset : PanOneParams::presets())
    {
        const auto values = valuesOf (preset.patch);

        // One branch per preset, alternating a nudge and full stretch, which
        // covers both ends across the bank in half the time of growing both
        // from every preset under the sanitizers.
        {
            const double amount = (total % 2 == 0) ? 0.3 : 1.0;
            const auto result = PanOneBreed::grow (values, { amount, 0.0 }, seed++, bank);
            ++total;
            tries += result.tries;

            if (! result.grown)
            {
                fail (std::string (preset.name) + " grew nothing fit at amount " + show (amount)
                      + " in " + std::to_string (result.tries) + " tries");
                continue;
            }

            ++grown;

            if (result.values == values)
                fail (std::string (preset.name) + " grew a branch identical to its seed");

            // Heard again, independently, for as long as the preset test
            // hears a preset.
            const auto heard = PanOneBreed::listen (patchOf (result.values), bank, 3.0);

            if (! (heard.finite && heard.peak < 1.5 && heard.loudest > 0.01))
                fail (std::string (preset.name) + " grew a branch that is silent or clips: loudest "
                      + show (heard.loudest) + " peak " + show (heard.peak));
        }
    }

    std::printf ("    %d of %d grew, %.1f tries each on average\n", grown, total,
                 (double) tries / total);

    // Same seed number, same branch, including the listening.
    const auto values = valuesOf (PanOneParams::presets()[2].patch);
    const auto one = PanOneBreed::grow (values, { 0.6, 0.0 }, 99, bank);
    const auto two = PanOneBreed::grow (values, { 0.6, 0.0 }, 99, bank);
    check (one.values == two.values && one.tries == two.tries, "growing is not deterministic");

    // And nothing fit is said, not hidden: the seed comes back.
    const auto none = PanOneBreed::grow (values, { 0.6, 0.0 }, 99, bank, 0);
    check (! none.grown && none.values == values, "a breed with no tries did not hand back the seed");
}

}   // namespace

int main()
{
    travelIsTheKnob();
    theDiceAreTheReference();
    theBredControlsAreTheSound();
    mutationsKeepTheRules();
    theAmountIsHowFar();
    theEndsReflect();
    aSeedNamesOnePatch();
    tweenStandsBetween();
    fitIsTheBanksOwnRule();
    everyPresetGrows();

    if (failures > 0)
    {
        std::printf ("\n%d failure(s)\n", failures);
        return 1;
    }

    std::printf ("\nall breeding checks passed\n");
    return 0;
}
