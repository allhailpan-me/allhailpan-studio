// What a row switched off used to cost, measured through the real voice.
//
// Not run by run.sh. This is the measurement that justified the fix to
// `Patch::anyRouteTo`, kept so the figure can be rechecked rather than taken
// from a write-up.
//
// The fault: `anyRouteTo` answered yes for a row whose own on/off control was
// off. Its only caller is `Synth/Voice.h`, which asks whether anything drives
// the gate and opens the gate from the amplitude envelope when nothing does,
// so that an instrument nobody has patched still makes a sound. A switched
// off row pointed at the gate therefore took the envelope away and put the
// matrix sum in its place, and `evaluate` skips a switched off row, so that
// sum is nothing. The gate is in series with everything the voice produces.
//
// The three renders are the same patch. The routing is env 1 at full depth on
// the gate, which is what the default already does, so the first two rows
// have to come out equal: that is what says the row is the only difference
// and the measurement is not reading something else. With the fault in place
// the third row was 0.000000. Not quieter. Silent.
//
// Build and run it by hand:
//
//   g++ -std=c++20 -O2 -I Tests/juce-stub -I Source -o /tmp/GateProbe
//       Tests/ModMatrixGateProbe.cpp
//   /tmp/GateProbe
//
// To see the fault, make `anyRouteTo` ignore `route.enabled` first.

#include "Synth/Voice.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{

constexpr double rate = 48000.0;

const Synth::WaveBank& bank()
{
    static Synth::WaveBank b = []
    {
        Synth::WaveBank x;
        x.buildAll();
        return x;
    }();

    return b;
}

Synth::Voice::Patch plainPatch()
{
    Synth::Voice::Patch p;

    p.osc1Table = Synth::WaveBank::indexFromId ("analogue");
    p.osc2Table = Synth::WaveBank::indexFromId ("analogue");
    p.osc1Level = 0.7;
    p.osc2Level = 0.0;
    p.foldDrive = 1.0;
    p.envelopes[0] = { 0.005, 0.100, 0.80, 0.150 };

    return p;
}

/** The loudest tenth of a second anywhere in the render.

    The measure CLAUDE.md says to use, and for the reason it gives: no fixed
    stretch of a render suits both a slow swell and something that has
    finished in seventy milliseconds, and measured over a fixed stretch
    several of the instrument's own presets read as silent when they are not.
*/
double loudestTenth (const Synth::Voice::Patch& patch, int note, int samples)
{
    Synth::Voice voice;
    voice.setSampleRate (rate);
    voice.setBank (&bank());
    voice.setPatch (patch);
    voice.start (note, 1.0f);

    std::vector<double> out;
    out.reserve ((std::size_t) samples);

    for (int i = 0; i < samples; ++i)
        out.push_back ((double) voice.nextSample());

    const std::size_t window = (std::size_t) (rate * 0.1);
    const std::size_t step = (std::size_t) (rate * 0.01);

    double best = 0.0;

    for (std::size_t start = 0; start + window <= out.size(); start += step)
    {
        double sum = 0.0;

        for (std::size_t i = 0; i < window; ++i)
            sum += out[start + i] * out[start + i];

        best = std::max (best, std::sqrt (sum / (double) window));
    }

    return best;
}

} // namespace

int main()
{
    const int env1 = Synth::ModMatrix::sourceFromId ("env1");
    const int gateOpen = Synth::ModMatrix::destinationFromId ("gateopen");

    const int samples = (int) (rate * 1.5);

    auto nothing = plainPatch();

    auto wired = plainPatch();
    wired.matrix.add (env1, gateOpen, 1.0f);

    auto off = plainPatch();
    Synth::ModMatrix::Route row;
    row.source = env1;
    row.destination = gateOpen;
    row.depth = 1.0f;
    row.enabled = false;
    off.matrix.add (row);

    std::printf ("loudest tenth of a second, same patch three ways\n\n");
    std::printf ("  no gate routing at all          %.6f\n",
                 loudestTenth (nothing, 60, samples));
    std::printf ("  the routing switched on         %.6f\n",
                 loudestTenth (wired, 60, samples));
    std::printf ("  the same routing switched off   %.6f\n",
                 loudestTenth (off, 60, samples));

    std::printf ("\nanyRouteTo(gateopen) with the row switched off: %s\n",
                 off.matrix.anyRouteTo (gateOpen) ? "true" : "false");

    return 0;
}
