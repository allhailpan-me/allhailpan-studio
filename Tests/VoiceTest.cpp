// Checks Synth/Voice.h, where all the separately tested parts meet.
//
// Each part has its own test and this one does not repeat them. What it
// checks is the wiring, and the wiring has its own kinds of failure:
//
// A voice that never reports itself finished. Sixteen of those and the
// instrument is exhausted with nothing audible to show for it. Nothing in any
// of the parts can catch this, because it is a property of how they were
// joined.
//
// A connection that is really soldered. The whole claim of this voice is that
// nothing is fixed: an envelope reaches whatever the patch says. A matrix that
// is read but whose result is then ignored, or a destination the voice looks
// up by the wrong id and so never finds, leaves the instrument working and
// quietly unable to do the thing it was rebuilt for. A test that only played
// a note would not notice.
//
// And a default that does not get out of the way. The gate opens from the
// amplitude envelope when nothing is pointed at it, so an unpatched
// instrument makes a sound, and that default has to stop the moment a patch
// points something at the gate. Getting that backwards gives two things
// fighting over the same control.

#include "Synth/Voice.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
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

std::string show (double v)
{
    char buffer[32];
    std::snprintf (buffer, sizeof (buffer), "%.4f", v);
    return buffer;
}

constexpr double pi = 3.14159265358979323846;
constexpr double rate = 48000.0;

/** The bank, built once. Building it is a few hundred transforms and every
    test here wants the same one. */
const Synth::WaveBank& sharedBank()
{
    static Synth::WaveBank bank = []
    {
        Synth::WaveBank b;
        b.buildAll();
        return b;
    }();

    return bank;
}

Synth::Voice::Patch plainPatch()
{
    Synth::Voice::Patch p;

    p.osc1Table = Synth::WaveBank::indexFromId ("analogue");
    p.osc2Table = Synth::WaveBank::indexFromId ("analogue");
    p.osc1Level = 0.7;
    p.osc2Level = 0.0;
    p.foldDrive = 1.0;

    // A plain amplitude envelope, so a note starts and ends.
    p.envelopes[0] = { 0.005, 0.100, 0.80, 0.150 };

    return p;
}

std::vector<double> play (const Synth::Voice::Patch& patch, int note, int samples,
                          int releaseAt = -1)
{
    Synth::Voice voice;
    voice.setSampleRate (rate);
    voice.setBank (&sharedBank());
    voice.setPatch (patch);
    voice.start (note, 1.0f);

    std::vector<double> out;
    out.reserve ((std::size_t) samples);

    for (int i = 0; i < samples; ++i)
    {
        if (releaseAt >= 0 && i == releaseAt)
            voice.stop();

        out.push_back ((double) voice.nextSample());
    }

    return out;
}

double rmsOf (const std::vector<double>& v, std::size_t from = 0, std::size_t to = 0)
{
    if (to == 0 || to > v.size())
        to = v.size();

    double sum = 0.0;
    std::size_t counted = 0;

    for (std::size_t i = from; i < to; ++i)
    {
        sum += v[i] * v[i];
        ++counted;
    }

    return counted > 0 ? std::sqrt (sum / (double) counted) : 0.0;
}

/** The centre of gravity of the spectrum, for telling brightness apart. */
double centroidOf (const std::vector<double>& block)
{
    const int n = (int) block.size();

    double weighted = 0.0, total = 0.0;

    for (int k = 1; k < n / 2; ++k)
    {
        std::complex<double> sum (0.0, 0.0);

        for (int i = 0; i < n; ++i)
        {
            const double window = 0.5 - 0.5 * std::cos (2.0 * pi * (double) i / (double) (n - 1));

            sum += block[(std::size_t) i] * window
                 * std::exp (std::complex<double> (0.0,
                       -2.0 * pi * (double) k * (double) i / (double) n));
        }

        const double magnitude = std::abs (sum);

        weighted += magnitude * (double) k * rate / (double) n;
        total += magnitude;
    }

    return total > 0.0 ? weighted / total : 0.0;
}

//==============================================================================
/** The tables the voice indexes by have to fit in the room it keeps for them.
    A later version adding sources past that would otherwise write past the
    end of a member, which is the kind of fault that works until it does not. */
void theTablesFit()
{
    std::printf ("table sizes\n");

    std::printf ("    %d sources, %d destinations\n",
                 Synth::ModMatrix::numSources(), Synth::ModMatrix::numDestinations());

    if (! (Synth::ModMatrix::numSources() <= 64))
        fail ("there are more sources than the voice keeps room for");

    if (! (Synth::ModMatrix::numDestinations() <= 64))
        fail ("there are more destinations than the voice keeps room for");

    // And everything the voice looks up by id has to exist, or a connection
    // silently does nothing. These are the ids the voice itself uses.
    for (const char* id : { "env1", "env2", "env3", "env4", "slope1", "slope2",
                            "velocity", "keytrack", "random", "gate" })
        if (Synth::ModMatrix::sourceFromId (id) < 0)
            fail (std::string ("the voice looks for a source called ") + id
                  + " and there is not one");

    for (const char* id : { "osc1pitch", "osc2pitch", "osc1pos", "osc2pos",
                            "osc1level", "osc2level", "sublevel", "noiselevel",
                            "fm", "fold", "foldsymmetry", "gateopen", "gaterange", "amp" })
        if (Synth::ModMatrix::destinationFromId (id) < 0)
            fail (std::string ("the voice looks for a destination called ") + id
                  + " and there is not one");
}

/** A note plays and then finishes. */
void aNoteStartsAndEnds()
{
    std::printf ("a note\n");

    auto patch = plainPatch();

    Synth::Voice voice;
    voice.setSampleRate (rate);
    voice.setBank (&sharedBank());
    voice.setPatch (patch);

    if (voice.isActive())
        fail ("a voice was active before anything was played");

    voice.start (45, 1.0f);

    std::vector<double> out;

    for (int i = 0; i < (int) (rate * 0.2); ++i)
        out.push_back ((double) voice.nextSample());

    if (! (rmsOf (out) > 0.01))
        fail ("playing a note made no sound: " + show (rmsOf (out)));

    if (! voice.isActive())
        fail ("the voice finished while the key was still down");

    voice.stop();

    int stillRunning = 0;

    for (int i = 0; i < (int) (rate * 3.0); ++i)
    {
        voice.nextSample();

        if (voice.isActive())
            stillRunning = i;
    }

    const double tail = (double) stillRunning / rate;

    std::printf ("    finished %s s after the key came up\n", show (tail).c_str());

    // It has to finish, and within something like the release it was given.
    // A voice that never reports itself finished exhausts the instrument.
    if (voice.isActive())
        fail ("the voice never finished");

    if (! (tail < 1.0))
        fail ("the voice took " + show (tail) + " s to finish on a 0.15 s release");

    // And every sample of it was a number.
    for (double v : out)
        if (! std::isfinite (v))
        {
            fail ("the voice produced something not finite");
            break;
        }
}

/** Nothing is soldered: a routing actually reaches what it points at.

    Checked on the fold, because what the fold does is visible in the spectrum
    and is not something any other part of the voice could produce. If the
    matrix were read and its result dropped, or the voice looked the
    destination up by a name that does not exist, this is flat.
*/
void aRoutingReachesWhatItPointsAt()
{
    std::printf ("a routing reaches the fold\n");

    const int env3 = Synth::ModMatrix::sourceFromId ("env3");
    const int fold = Synth::ModMatrix::destinationFromId ("fold");

    auto quiet = plainPatch();
    quiet.osc1Table = Synth::WaveBank::indexFromId ("harmonics");
    quiet.osc1Position = 0.0;           // a sine, so the fold has clean work to do
    quiet.foldDrive = 1.0;

    auto driven = quiet;

    // Envelope three held wide open, pointed at the fold.
    driven.envelopes[2] = { 0.001, 0.001, 1.0, 0.1 };
    driven.matrix.add (env3, fold, 1.0f);

    const int samples = 4096;

    const auto flat = play (quiet, 45, samples);
    const auto folded = play (driven, 45, samples);

    const double flatCentroid = centroidOf (flat);
    const double foldedCentroid = centroidOf (folded);

    std::printf ("    unrouted centroid %.0f Hz, routed %.0f Hz\n",
                 flatCentroid, foldedCentroid);

    // Folding a sine adds harmonics, so the centre of gravity has to climb,
    // and by a lot rather than a little.
    if (! (foldedCentroid > flatCentroid * 2.0))
        fail ("routing an envelope to the fold barely changed the sound: "
              + show (flatCentroid) + " Hz to " + show (foldedCentroid));
}

/** The gate's default gets out of the way when something is pointed at it. */
void theGateDefaultYields()
{
    std::printf ("the gate's default\n");

    const int gateOpen = Synth::ModMatrix::destinationFromId ("gateopen");
    const int env4 = Synth::ModMatrix::sourceFromId ("env4");

    // With nothing routed, the amplitude envelope opens it, so an instrument
    // nobody has patched still makes a sound.
    auto unpatched = plainPatch();

    if (! (rmsOf (play (unpatched, 45, (int) (rate * 0.2))) > 0.01))
        fail ("with nothing routed to the gate the voice was silent");

    // Routed to an envelope that never opens, the voice has to go quiet,
    // which proves the default stopped applying.
    auto shut = plainPatch();
    shut.envelopes[3] = { 0.001, 0.001, 0.0, 0.001 };   // falls straight to nothing
    shut.matrix.add (env4, gateOpen, 1.0f);

    const auto closed = play (shut, 45, (int) (rate * 0.2));

    // Measured after the envelope has had time to shut, and the vactrol after
    // it, rather than from the attack.
    const double late = rmsOf (closed, (std::size_t) (rate * 0.15));

    std::printf ("    gate held shut by a routing: %s\n", show (late).c_str());

    if (! (late < 0.002))
        fail ("a routing holding the gate shut did not: " + show (late));

    // And the same envelope held open gives the sound back, so the difference
    // above is the routing and not something else going quiet.
    auto open = plainPatch();
    open.envelopes[3] = { 0.001, 0.001, 1.0, 0.1 };
    open.matrix.add (env4, gateOpen, 1.0f);

    const double loud = rmsOf (play (open, 45, (int) (rate * 0.2)),
                               (std::size_t) (rate * 0.15));

    std::printf ("    gate held open by a routing: %s\n", show (loud).c_str());

    if (! (loud > 0.01))
        fail ("a routing holding the gate open did not: " + show (loud));
}

/** Modulating one oscillator's phase with the other does not move the note. */
void phaseModulationDoesNotDetune()
{
    std::printf ("phase modulation\n");

    auto patch = plainPatch();
    patch.osc2Level = 0.0;              // heard only through the modulation
    patch.fmAmount = 2.0;
    patch.osc2Semitones = 7.0;          // a fifth up, so the sidebands are obvious

    const int note = 45;                // A2, 110 Hz
    const double expected = 440.0 * std::pow (2.0, (note - 69) / 12.0);

    const int samples = 16384;
    const auto rendered = play (patch, note, samples);

    // The strongest bin anywhere near the note.
    double best = 0.0;
    double bestHz = 0.0;

    for (int k = 1; k < samples / 2; ++k)
    {
        const double hz = (double) k * rate / (double) samples;

        if (hz < expected * 0.6 || hz > expected * 1.6)
            continue;

        std::complex<double> sum (0.0, 0.0);

        for (int i = 0; i < samples; ++i)
        {
            const double window = 0.5 - 0.5 * std::cos (2.0 * pi * (double) i
                                                        / (double) (samples - 1));

            sum += rendered[(std::size_t) i] * window
                 * std::exp (std::complex<double> (0.0,
                       -2.0 * pi * (double) k * (double) i / (double) samples));
        }

        if (std::abs (sum) > best)
        {
            best = std::abs (sum);
            bestHz = hz;
        }
    }

    std::printf ("    note is %.1f Hz, strongest partial near it is %.1f Hz\n",
                 expected, bestHz);

    if (! (std::fabs (bestHz - expected) < 6.0))
        fail ("under modulation the note moved from " + show (expected)
              + " to " + show (bestHz) + " Hz");
}

/** Anything a patch can hold has to be survivable. */
void nothingUnreasonableBreaksIt()
{
    std::printf ("out of range patches\n");

    std::mt19937 rng (24601);
    std::uniform_real_distribution<double> knob (-20.0, 20.0);
    std::uniform_int_distribution<int> anyTable (-5, Synth::WaveBank::count() + 5);
    std::uniform_int_distribution<int> anySource (-3, Synth::ModMatrix::numSources() + 3);
    std::uniform_int_distribution<int> anyDestination (-3, Synth::ModMatrix::numDestinations() + 3);
    std::uniform_int_distribution<int> anyNote (-20, 140);

    Synth::Voice voice;
    voice.setSampleRate (rate);
    voice.setBank (&sharedBank());

    for (int trial = 0; trial < 300; ++trial)
    {
        Synth::Voice::Patch p;

        p.osc1Table = anyTable (rng);
        p.osc2Table = anyTable (rng);
        p.osc1Position = knob (rng);
        p.osc2Position = knob (rng);
        p.osc1Level = knob (rng);
        p.osc2Level = knob (rng);
        p.subLevel = knob (rng);
        p.noiseLevel = knob (rng);
        p.osc2Semitones = knob (rng) * 5.0;
        p.fmAmount = knob (rng);
        p.foldDrive = knob (rng);
        p.foldSymmetry = knob (rng);
        p.gateRange = knob (rng);
        p.gateResting = knob (rng) * 500.0;
        p.gateLag = knob (rng) * 0.1;

        for (auto& e : p.envelopes)
            e = { std::fabs (knob (rng)) * 0.01, std::fabs (knob (rng)) * 0.01,
                  knob (rng) * 0.1, std::fabs (knob (rng)) * 0.01 };

        for (auto& s : p.slopes)
            s = { std::fabs (knob (rng)) * 0.05, std::fabs (knob (rng)) * 0.05,
                  knob (rng) * 0.1, trial % 2 == 0 };

        for (int r = 0; r < 8; ++r)
            p.matrix.add (anySource (rng), anyDestination (rng), (float) knob (rng) * 0.1f);

        voice.setPatch (p);
        voice.start (anyNote (rng), (float) std::fabs (knob (rng)) * 0.1f);

        for (int i = 0; i < 600; ++i)
        {
            if (i == 400)
                voice.stop();

            const double v = (double) voice.nextSample();

            if (! std::isfinite (v))
            {
                fail ("a patch produced something not finite on trial "
                      + std::to_string (trial));
                return;
            }

            // Loud is allowed, because a patch can ask for several
            // oscillators at full level. Absurd is not: it would mean
            // something is running away rather than being loud.
            if (std::fabs (v) > 40.0)
            {
                fail ("a patch produced " + show (v) + " on trial "
                      + std::to_string (trial));
                return;
            }
        }

        voice.kill();
    }

    // A voice with no bank at all is silent rather than a crash, which is
    // what a patch referring to a bank that failed to build would give.
    Synth::Voice orphan;
    orphan.setSampleRate (rate);
    orphan.setPatch (plainPatch());
    orphan.start (45, 1.0f);

    for (int i = 0; i < 256; ++i)
        if (! std::isfinite (orphan.nextSample()))
        {
            fail ("a voice with no bank produced something not finite");
            break;
        }
}

} // namespace

int main()
{
    std::printf ("Synth::Voice\n\n");

    theTablesFit();
    aNoteStartsAndEnds();
    aRoutingReachesWhatItPointsAt();
    theGateDefaultYields();
    phaseModulationDoesNotDetune();
    nothingUnreasonableBreaksIt();

    std::printf ("\n");

    if (failures > 0)
    {
        std::printf ("%d check(s) failed.\n", failures);
        return 1;
    }

    std::printf ("All checks passed.\n");
    return 0;
}
