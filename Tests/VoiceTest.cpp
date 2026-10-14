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
#include <cstring>
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

double frequencyOfNote (int note)
{
    return 440.0 * std::pow (2.0, ((double) note - 69.0) / 12.0);
}

/** How much of the output is not a harmonic of the note, in decibels.

    The same measurement the wavefolder's and the saturator's tests use, so
    the three sets of numbers can be read against one another. Everything
    within eight bins of a harmonic is treated as signal, and only what lands
    below 20 kHz is counted: the oversampler's filters are specified to push
    what folds above that on purpose, so counting it would be measuring the
    thing that is working.
*/
double aliasFloorDb (const std::vector<double>& rendered, double frequency)
{
    const int n = (int) rendered.size();

    std::vector<double> windowed ((std::size_t) n);

    const double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;

    for (int i = 0; i < n; ++i)
    {
        const double t = 2.0 * 3.14159265358979323846 * (double) i / (double) (n - 1);

        windowed[(std::size_t) i] = rendered[(std::size_t) i]
            * (a0 - a1 * std::cos (t) + a2 * std::cos (2.0 * t) - a3 * std::cos (3.0 * t));
    }

    const double binHz = rate / (double) n;

    double loudest = 0.0, stray = 0.0;
    int strayBins = 0;

    for (int k = 10; k <= n / 2; ++k)
    {
        std::complex<double> sum (0.0, 0.0);

        for (int i = 0; i < n; ++i)
            sum += windowed[(std::size_t) i]
                 * std::exp (std::complex<double> (0.0,
                       -2.0 * 3.14159265358979323846 * (double) k * (double) i / (double) n));

        const double hz = (double) k * binHz;
        const double magnitude = std::abs (sum);
        const double nearest = std::floor (hz / frequency + 0.5);

        if (nearest >= 1.0 && std::fabs (hz - nearest * frequency) / binHz <= 8.0)
        {
            loudest = std::max (loudest, magnitude);
            continue;
        }

        if (hz > 20000.0)
            continue;

        ++strayBins;
        stray = std::max (stray, magnitude);
    }

    if (strayBins < 30 || ! (loudest > 0.0))
        return -1000.0;

    return 20.0 * std::log10 (stray / loudest + 1e-300);
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

    // The folder is crossfaded in rather than always in circuit, and the mix
    // rests at nothing so that an untouched patch is clean. A patch that
    // wants to hear the fold has to ask, which is the whole point of the
    // control, and it is also why this line is here: without it the routing
    // below reaches the fold's drive perfectly well and the fold is not in
    // the signal for it to matter to.
    quiet.foldMix = 1.0;

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

/** Every destination the matrix declares either changes the sound or is on
    the list of ones that do not exist yet, and the list is measured rather
    than believed.

    This is the check that closes a whole class of fault rather than one
    instance of it. A destination that is declared and unreached is the exact
    shape this project keeps meeting: the menu offers it, the patch accepts
    it, the routing saves and reloads, and nothing happens. Nothing errors,
    so the only way anybody finds out is by wiring one up and listening hard
    enough to be sure.

    It works in both directions, which is what stops the list rotting. Adding
    a destination to the matrix and forgetting to reach it fails here until
    somebody writes down that it is not reached yet. Reaching one and
    forgetting to take it off the list fails here too. So the list cannot
    quietly become a lie, which is the usual fate of a list of known gaps.

    The method is to play the same patch twice, once with one extra routing
    pointed at the destination under test, and see whether the output moved
    at all. Both signs of depth are tried, because some destinations sit at
    one end of their range in the base patch and can only be pushed one way.
*/
void everyDestinationIsReachedOrSaidNotToBe()
{
    std::printf ("every destination\n");

    // The destinations that are declared and that the voice does not reach,
    // with the reason. Each of these is a module that is not built rather
    // than a wire that is missing, which is why they are listed here instead
    // of being quietly dropped from the matrix: the engine is going to have
    // them, and taking them out of the menu now would mean every patch
    // written in the meantime loses its routing when they arrive.
    struct NotYet { const char* id; const char* why; };

    const NotYet notYet[]
    {
        { "unisondetune", "unison belongs to the instrument, not one voice" },
        { "unisonspread", "unison belongs to the instrument, not one voice" },
    };

    const int env3 = Synth::ModMatrix::sourceFromId ("env3");
    const int slope1 = Synth::ModMatrix::sourceFromId ("slope1");

    // A patch with something going on in every part of the voice, so that
    // moving any one of them is visible. Anything resting at zero, or any
    // section crossfaded out, would make the destinations that feed it look
    // unreached when they are merely inaudible.
    auto base = plainPatch();

    base.osc1Table = Synth::WaveBank::indexFromId ("analogue");
    base.osc2Table = Synth::WaveBank::indexFromId ("harmonics");
    base.osc1Position = 0.5;
    base.osc2Position = 0.5;
    base.osc1Level = 0.5;
    base.osc2Level = 0.3;
    base.subLevel = 0.2;
    base.noiseLevel = 0.05;
    base.osc2Semitones = 7.0;
    base.fmAmount = 0.5;

    base.foldDrive = 2.0;
    base.foldMix = 0.4;
    base.driveAmount = 2.0;

    base.cutoff = 1500.0;
    base.resonance = 2.0;

    base.gateRange = 4.0;
    base.gateResting = 300.0;

    // Held open, so the voice is steady for the whole measurement and a
    // difference is a difference in the sound rather than in the envelope.
    base.envelopes[0] = { 0.002, 0.050, 1.0, 0.150 };
    base.envelopes[2] = { 0.001, 0.001, 1.0, 0.100 };

    // Both slopes cycling and both wired to something, because a slope's
    // rate can only be seen through whatever the slope is driving. Without
    // these two lines the two slope rate destinations would measure as
    // unreached while being perfectly well connected.
    base.slopes[0] = { 0.05, 0.05, 0.0, true };
    base.slopes[1] = { 0.03, 0.09, 0.0, true };

    base.matrix.add (slope1, Synth::ModMatrix::destinationFromId ("osc1pos"), 0.3f);
    base.matrix.add (Synth::ModMatrix::sourceFromId ("slope2"),
                     Synth::ModMatrix::destinationFromId ("foldmix"), 0.3f);

    // And all four LFOs wired to something, for exactly the reason the two
    // slopes are: an LFO's rate destination can only be seen through
    // whatever the LFO is driving. Without these four lines the four rate
    // destinations measure as unreached while being perfectly connected,
    // and the test agrees with a list that says they are not built, which
    // is two wrongs reading as a right.
    const char* const lfoSources[4] { "lfo1", "lfo2", "lfo3", "lfo4" };
    const char* const lfoTargets[4] { "osc2pos", "cutoff", "foldsymmetry",
                                      "noiselevel" };

    const double lfoRates[4] { 7.0, 11.0, 3.0, 5.0 };

    for (int i = 0; i < 4; ++i)
    {
        base.lfos[i].rateHz = lfoRates[i];
        base.lfos[i].shape = Synth::Lfo::Shape::triangle;

        base.matrix.add (Synth::ModMatrix::sourceFromId (lfoSources[i]),
                         Synth::ModMatrix::destinationFromId (lfoTargets[i]),
                         0.3f);
    }

    // A glide long enough to still be arriving across the measurement.
    base.glideSeconds = 0.25;

    const int samples = 6000;

    // Two notes rather than one, because a glide has nothing to do on a
    // first note: there is no pitch to come from. Without the note before
    // it, the glide destination measures as unreached while being
    // perfectly connected, which is the same trap the slope and LFO rates
    // set.
    const auto playFromAnotherNote = [] (const Synth::Voice::Patch& patch, int count)
    {
        Synth::Voice voice;
        voice.setSampleRate (rate);
        voice.setBank (&sharedBank());
        voice.setPatch (patch);

        voice.start (45, 0.9f);

        for (int i = 0; i < 2000; ++i)
            voice.nextSample();

        voice.start (57, 0.9f);

        std::vector<double> out;
        out.reserve ((std::size_t) count);

        for (int i = 0; i < count; ++i)
            out.push_back ((double) voice.nextSample());

        return out;
    };

    const auto reference = playFromAnotherNote (base, samples);
    const double referenceLevel = rmsOf (reference);

    const auto movedBy = [&] (int destination, float depth)
    {
        auto patch = base;

        if (! patch.matrix.add (env3, destination, depth))
            return 0.0;

        const auto rendered = playFromAnotherNote (patch, samples);

        double worst = 0.0;

        for (std::size_t i = 0; i < rendered.size(); ++i)
            worst = std::max (worst, std::fabs (rendered[i] - reference[i]));

        return worst / (referenceLevel + 1e-12);
    };

    int reached = 0;
    std::vector<std::string> wrongWayRound;

    for (int d = 0; d < Synth::ModMatrix::numDestinations(); ++d)
    {
        const char* id = Synth::ModMatrix::destinationId (d);

        // Three depths, not two. Full depth is the one value that does
        // nothing to a destination whose range is a whole cycle: adding
        // one complete rotation to an oscillator's phase leaves it exactly
        // where it was, so the two phase destinations measured as
        // unreached at plus and minus one while being perfectly connected.
        const double moved = std::max (movedBy (d, 1.0f),
                                       std::max (movedBy (d, -1.0f),
                                                 movedBy (d, 0.5f)));

        bool listed = false;
        const char* why = "";

        for (const auto& entry : notYet)
            if (std::strcmp (entry.id, id) == 0)
            {
                listed = true;
                why = entry.why;
            }

        // A thousandth of the signal's own level. Far below anything
        // audible, so a destination that is connected at all clears it by
        // orders of magnitude, and nothing clears it by accident: the voice
        // is deterministic, including its noise, so an unrouted destination
        // moves the output by exactly nothing.
        const bool works = moved > 1e-3;

        if (works)
            ++reached;

        if (works && listed)
            wrongWayRound.push_back (std::string (id) + " is listed as not built ("
                                     + why + ") and moves the output by "
                                     + show (moved));

        if (! works && ! listed)
            wrongWayRound.push_back (std::string (id)
                                     + " is declared by the matrix and reaches nothing:"
                                       " moved the output by " + show (moved));
    }

    std::printf ("    %d of %d destinations reach the voice, %d declared not built\n",
                 reached, Synth::ModMatrix::numDestinations(),
                 (int) (sizeof (notYet) / sizeof (notYet[0])));

    for (const auto& problem : wrongWayRound)
        fail (problem);
}

/** Every source the matrix declares actually drives something.

    The mirror of the destination check above, and worth having for the same
    reason in the other direction: a source that is declared and never read
    offers a patch a modulator that does nothing. The menu lists it, the
    routing saves, and the sound never moves.

    It needs a little more setting up than the destination check, because
    several of these are inputs rather than generators: a mod wheel nobody
    has moved is zero, and a routing from zero is indistinguishable from no
    routing at all. So every input is pushed somewhere definite first, and
    what is then measured is whether pointing it at something changes the
    sound.
*/
void everySourceDrivesSomething()
{
    std::printf ("every source\n");

    // The destination they are all pointed at. The fold's drive is a good
    // choice: it is audible in the spectrum, it is not already being driven
    // by the base patch, and it responds across its whole range rather than
    // saturating at one end.
    const int toFold = Synth::ModMatrix::destinationFromId ("fold");

    auto base = plainPatch();
    base.osc1Table = Synth::WaveBank::indexFromId ("harmonics");
    base.osc1Position = 0.0;
    base.osc1Level = 0.8;
    base.foldDrive = 2.0;
    base.foldMix = 0.8;
    base.cutoff = 18000.0;
    base.gateRange = 0.0;
    base.gateMode = Synth::LowPassGate::Mode::amplitude;
    base.gateLag = 0.0;
    base.envelopes[0] = { 0.002, 0.050, 1.0, 0.150 };

    // The generators need to be doing something, and the four envelopes
    // need to be open rather than resting at nothing.
    for (int i = 1; i < 4; ++i)
        base.envelopes[i] = { 0.002, 0.050, 1.0, 0.150 };

    base.slopes[0] = { 0.03, 0.03, 0.0, true };
    base.slopes[1] = { 0.05, 0.02, 0.0, true };

    for (int i = 0; i < 4; ++i)
    {
        base.lfos[i].rateHz = 6.0 + 3.0 * (double) i;
        base.lfos[i].shape = Synth::Lfo::Shape::triangle;
    }

    for (int i = 0; i < 4; ++i)
        base.macros[i] = 0.3 + 0.15 * (double) i;

    const int samples = 6000;

    // Every input pushed off its resting value, so that a routing from one
    // has something to carry.
    const auto play = [&] (const Synth::Voice::Patch& patch)
    {
        Synth::Voice voice;
        voice.setSampleRate (rate);
        voice.setBank (&sharedBank());
        voice.setSeed (7);
        voice.setPatch (patch);

        voice.setModWheel (0.8);
        voice.setPressure (0.6);
        voice.setBend (0.5);
        voice.setUnisonPosition (-0.7);

        // Not middle C, so key tracking is not sitting at its own zero.
        voice.start (52, 0.75f);

        std::vector<double> out;
        out.reserve ((std::size_t) samples);

        for (int i = 0; i < samples; ++i)
            out.push_back ((double) voice.nextSample());

        return out;
    };

    const auto reference = play (base);
    const double referenceLevel = rmsOf (reference);

    std::vector<std::string> dead;

    for (int s = 0; s < Synth::ModMatrix::numSources(); ++s)
    {
        double moved = 0.0;

        for (float depth : { 1.0f, -1.0f })
        {
            auto patch = base;

            if (! patch.matrix.add (s, toFold, depth))
                continue;

            const auto rendered = play (patch);

            for (std::size_t i = 0; i < rendered.size(); ++i)
                moved = std::max (moved, std::fabs (rendered[i] - reference[i]));
        }

        moved /= referenceLevel + 1e-12;

        if (! (moved > 1e-3))
            dead.push_back (std::string (Synth::ModMatrix::sourceId (s))
                            + " moved the output by " + show (moved));
    }

    std::printf ("    %d sources, %d of them driving nothing\n",
                 Synth::ModMatrix::numSources(), (int) dead.size());

    for (const auto& one : dead)
        fail ("a source the matrix declares drives nothing: " + one);
}

/** A glide takes the time it is given, and takes it whichever way the
    interval goes.

    Two claims. The time is how long the whole interval takes rather than
    how long each semitone takes, which is what a knob marked in seconds
    implies and is what hardware marked "glide" has meant since the
    Minimoog. And because the pitch is moved in semitones rather than in
    hertz, an octave up and an octave down take the same time: moved in
    hertz an octave up is twice as far and would take twice as long, which
    is not what anybody means by a glide time.
*/
void aGlideTakesTheTimeItIsGiven()
{
    std::printf ("glide\n");

    auto base = plainPatch();
    base.osc1Table = Synth::WaveBank::indexFromId ("harmonics");
    base.osc1Position = 0.0;            // a sine, so the pitch is easy to read
    base.osc1Level = 0.9;
    base.cutoff = 20000.0;
    base.gateRange = 0.0;
    base.gateMode = Synth::LowPassGate::Mode::amplitude;
    base.gateLag = 0.0;
    base.envelopes[0] = { 0.001, 0.020, 1.0, 0.100 };

    /** The pitch through time, one reading per cycle.

        Counting crossings inside a window was tried first and cannot do
        this: at 110 Hz a fiftieth of a second holds five cycles, so the
        best it can resolve is about eighteen per cent, and a glide of an
        octave downward never appeared to arrive at all. Interpolating each
        upward crossing and taking the period to the next one gives a
        reading every cycle, accurate to a fraction of a per cent, which is
        what following a moving pitch needs.
    */
    struct Reading { double seconds, hz; };

    const auto track = [] (const std::vector<double>& block)
    {
        std::vector<Reading> out;
        double previous = -1.0;

        for (std::size_t i = 1; i < block.size(); ++i)
        {
            if (! (block[i - 1] <= 0.0 && block[i] > 0.0))
                continue;

            // Where between the two samples it really crossed.
            const double span = block[i] - block[i - 1];
            const double at = (double) (i - 1)
                                + (span != 0.0 ? -block[i - 1] / span : 0.0);

            if (previous >= 0.0)
            {
                const double period = (at - previous) / rate;

                if (period > 0.0)
                    out.push_back ({ at / rate, 1.0 / period });
            }

            previous = at;
        }

        return out;
    };

    const auto glideBetween = [&] (double seconds, int from, int to, int count)
    {
        auto patch = base;
        patch.glideSeconds = seconds;

        Synth::Voice voice;
        voice.setSampleRate (rate);
        voice.setBank (&sharedBank());
        voice.setPatch (patch);

        voice.start (from, 0.9f);

        for (int i = 0; i < (int) (rate * 0.3); ++i)
            voice.nextSample();

        voice.start (to, 0.9f);

        std::vector<double> out;
        out.reserve ((std::size_t) count);

        for (int i = 0; i < count; ++i)
            out.push_back ((double) voice.nextSample());

        return out;
    };

    /** How far along the interval the pitch has travelled at a moment, as
        a fraction, measured in semitones because that is the domain the
        glide moves in. */
    const auto progressAt = [&] (const std::vector<Reading>& readings,
                                  int from, int to, double seconds)
    {
        const double startNote = (double) from;
        const double endNote = (double) to;

        double closest = 1e9, note = startNote;

        for (const auto& reading : readings)
        {
            const double away = std::fabs (reading.seconds - seconds);

            if (away < closest && reading.hz > 20.0)
            {
                closest = away;
                note = 69.0 + 12.0 * std::log2 (reading.hz / 440.0);
            }
        }

        return (note - startNote) / (endNote - startNote);
    };

    // Notes high enough that a cycle is short against the glide, and an
    // octave apart so the interval is large and easy to read.
    const int low = 69, high = 81;

    for (double seconds : { 0.1, 0.4, 1.0 })
    {
        const int count = (int) (rate * (seconds * 2.5 + 0.2));
        const auto readings = track (glideBetween (seconds, low, high, count));

        const double tenth = progressAt (readings, low, high, seconds * 0.1);
        const double whole = progressAt (readings, low, high, seconds);
        const double twice = progressAt (readings, low, high, seconds * 2.0);

        std::printf ("    set to %s s: a tenth of the way through it is %s of the"
                     " way there, after the whole time %s, after twice %s\n",
                     show (seconds).c_str(), show (tenth).c_str(),
                     show (whole).c_str(), show (twice).c_str());

        // The exponential leaves a thousandth of the interval after the
        // time it was given, so by then it has arrived; a tenth of the way
        // through it is about half way, which is the shape of an
        // exponential and is nowhere near arrived.
        if (! (tenth < 0.75))
            fail ("a glide set to " + show (seconds) + " s was already "
                  + show (tenth) + " of the way there after a tenth of it");

        if (! (whole > 0.9 && twice > 0.98))
            fail ("a glide set to " + show (seconds) + " s was only " + show (whole)
                  + " of the way there after the whole time");
    }

    // An octave up and an octave down travel the same fraction in the same
    // time, which is what says the pitch is moved in semitones and not in
    // hertz: in hertz an octave up is twice as far and would take twice as
    // long.
    {
        const double seconds = 0.4;
        const int count = (int) (rate * 1.2);

        const auto upReadings = track (glideBetween (seconds, low, high, count));
        const auto downReadings = track (glideBetween (seconds, high, low, count));

        const double up = progressAt (upReadings, low, high, seconds * 0.3);
        const double down = progressAt (downReadings, high, low, seconds * 0.3);

        std::printf ("    three tenths in, an octave up is %s of the way and an"
                     " octave down is %s\n", show (up).c_str(), show (down).c_str());

        if (! (std::fabs (up - down) < 0.12))
            fail ("an octave up is " + show (up) + " of the way and an octave down "
                  + show (down) + ", so the pitch is not being moved in semitones");
    }

    // And no glide means the note is simply there.
    {
        const auto readings = track (glideBetween (0.0, low, high, (int) (rate * 0.3)));

        const double straightAway = progressAt (readings, low, high, 0.01);

        std::printf ("    with no glide the note is %s of the way there at once\n",
                     show (straightAway).c_str());

        if (! (straightAway > 0.98))
            fail ("with no glide the note was only " + show (straightAway)
                  + " of the way there");
    }
}

/** The filter filters, the resonance resonates, and the key tracking tracks.

    Separate from the check above, which only proves that pointing something
    at a destination changes the output. A filter wired to the wrong tap of
    the state variable, or fed hertz where it wanted octaves, would pass that
    check easily and would still not be a filter.
*/
void theFilterIsAFilter()
{
    std::printf ("the filter\n");

    auto base = plainPatch();
    base.osc1Table = Synth::WaveBank::indexFromId ("analogue");
    base.osc1Position = 0.45;               // a sawtooth: plenty to take away
    base.osc1Level = 0.8;
    base.envelopes[0] = { 0.002, 0.050, 1.0, 0.150 };
    base.gateRange = 0.0;                   // the gate out of the way
    base.gateMode = Synth::LowPassGate::Mode::amplitude;

    // Closing the filter has to darken the sound, every step of the way.
    double previous = 1e9;

    std::printf ("    centroid as the cutoff closes:");

    for (double cutoff : { 16000.0, 8000.0, 4000.0, 2000.0, 1000.0, 500.0 })
    {
        auto patch = base;
        patch.cutoff = cutoff;

        const auto rendered = play (patch, 45, 8192);
        const double centroid = centroidOf (rendered);

        std::printf (" %.0f:%.0f", cutoff, centroid);

        if (! (centroid < previous))
            fail ("closing the filter to " + show (cutoff) + " Hz did not darken the"
                  " sound: centroid " + show (centroid) + " against " + show (previous));

        previous = centroid;
    }

    std::printf ("\n");

    // Resonance has to lift what is at the cutoff. Measured as the level of
    // the whole output with the cutoff sitting on a harmonic, which rises as
    // the peak sharpens.
    double quietest = 1e9, loudest = 0.0;

    std::printf ("    level as the resonance rises:");

    for (double q : { 0.7, 2.0, 6.0, 14.0 })
    {
        auto patch = base;
        patch.cutoff = 880.0;               // the fourth harmonic of the note
        patch.resonance = q;

        const auto rendered = play (patch, 45, 8192);
        const double level = rmsOf (rendered, 2000);

        std::printf (" %.1f:%.4f", q, level);

        quietest = std::min (quietest, level);
        loudest = std::max (loudest, level);
    }

    std::printf ("\n");

    if (! (loudest > quietest * 1.3))
        fail ("resonance barely changed the output: " + show (quietest) + " to "
              + show (loudest));

    // Key tracking. With it at one the filter moves with the note, so the
    // sound is the same distance above its own fundamental everywhere on the
    // keyboard; with it at nothing the filter stays put and high notes come
    // out darker relative to themselves.
    const auto brightnessAgainstPitch = [&] (double keyTrack)
    {
        auto patch = base;
        patch.cutoff = 600.0;
        patch.filterKeyTrack = keyTrack;

        const double low = centroidOf (play (patch, 36, 8192))
                             / frequencyOfNote (36);
        const double high = centroidOf (play (patch, 60, 8192))
                             / frequencyOfNote (60);

        return high / low;
    };

    const double tracked = brightnessAgainstPitch (1.0);
    const double fixed = brightnessAgainstPitch (0.0);

    std::printf ("    two octaves up, brightness against the note: tracking %s,"
                 " fixed %s\n", show (tracked).c_str(), show (fixed).c_str());

    // Tracking keeps the ratio near one. Not exactly one, because the
    // oscillator's own harmonics run out against Nyquist differently at the
    // two pitches, but far nearer than leaving the filter where it was.
    if (! (tracked > 0.6))
        fail ("key tracking did not keep the timbre with the note: ratio "
              + show (tracked));

    if (! (fixed < tracked * 0.8))
        fail ("turning key tracking off changed nothing: fixed " + show (fixed)
              + " against tracked " + show (tracked));
}

/** The nonlinear section does not alias, which is the whole reason it costs
    what it costs.

    Measured on the voice rather than on the folder and the saturator
    separately, because the thing that would go wrong here is in the joining:
    an oversampler prepared at the wrong rate, a module told the base rate
    while running at four times it, or a nonlinearity accidentally left
    outside the section. Each of those leaves both modules passing their own
    tests and the instrument dirty.
*/
void theVoiceDoesNotAlias()
{
    std::printf ("aliasing\n");

    auto patch = plainPatch();
    patch.osc1Table = Synth::WaveBank::indexFromId ("harmonics");
    patch.osc1Position = 0.0;               // a sine in, so everything out is made here
    patch.osc1Level = 0.9;
    patch.noiseLevel = 0.0;

    // The note and the drive are chosen from a measurement rather than for
    // looking hard. A low note folded gently aliases at the floor of this
    // measurement whatever the oversampling is, so a test written there
    // proves nothing: at 110 Hz and a fold of four, one times, two times and
    // four times all read the same number. Across notes and drives:
    //
    //     note             1x        2x        4x
    //     110 Hz, fold 4  -100      -101      -101
    //     523 Hz, fold 8   -42       -71      -104
    //    1046 Hz, fold 8   -38       -61       -86
    //    2093 Hz, fold 8   -33       -52       -68
    //
    // 523 Hz at a fold of eight is where the three separate by the most, so
    // it is the case that holds the oversampling in place. The last row is
    // the honest limit: folding is a bass and midrange instrument, and the
    // folder says where its own edge is in `foldsCleanlyBelow`.
    patch.foldDrive = 8.0;
    patch.foldMix = 1.0;
    patch.driveAmount = 6.0;

    patch.cutoff = 18000.0;
    patch.gateRange = 0.0;
    patch.gateMode = Synth::LowPassGate::Mode::amplitude;
    patch.envelopes[0] = { 0.001, 0.010, 1.0, 0.100 };

    const double hz = frequencyOfNote (72);

    const auto rendered = play (patch, 72, 16384);

    std::vector<double> steady (rendered.begin() + 4096, rendered.end());

    const double db = aliasFloorDb (steady, hz);

    std::printf ("    a folded and driven sine at %.1f Hz: strays at %s dB\n",
                 hz, show (db).c_str());

    // Measured at -104 dB. Held to -90, which leaves room for the ordinary
    // drift of a measurement with an oscillator, a gate and an envelope in
    // front of it, and still fails by a wide margin on anything that breaks
    // the section: half the oversampling reads -71 here and none of it
    // reads -42.
    if (! (db < -90.0))
        fail ("the voice aliases: strays at " + show (db) + " dB");
}

/** Driving a slope's rate upward makes it faster, and by the amount the
    destination says.

    The direction is the point. A rate destination declared in octaves whose
    positive end slows the slope down is the complete opposite of what every
    patch written against it will expect, and it is invisible to any check
    that only asks whether the routing changed the sound: it changes it
    either way. Inverting the sign survived every other check in this file.
*/
void drivingASlopeFasterMakesItFaster()
{
    std::printf ("a slope driven faster\n");

    const int slope1 = Synth::ModMatrix::sourceFromId ("slope1");
    const int env3 = Synth::ModMatrix::sourceFromId ("env3");
    const int gateOpen = Synth::ModMatrix::destinationFromId ("gateopen");
    const int slope1Rate = Synth::ModMatrix::destinationFromId ("slope1rate");

    auto base = plainPatch();
    base.osc1Table = Synth::WaveBank::indexFromId ("analogue");
    base.osc1Level = 0.8;
    base.envelopes[0] = { 0.002, 0.020, 1.0, 0.100 };

    // Five times a second, and pointed at the gate, so the output pulses at
    // whatever rate the slope is actually running at.
    base.slopes[0] = { 0.1, 0.1, 0.0, true };
    base.matrix.add (slope1, gateOpen, 1.0f);

    // How fast the output is swelling, read off the envelope's own spectrum
    // rather than by counting crossings.
    //
    // Counting was tried first and is not good enough here. The level has to
    // be taken over a window, the window has to be short enough to see a
    // swell five times a second, and at that length it is also short enough
    // to see the note itself going up and down inside it: a window of 128
    // samples on a 110 Hz note read fifty swells a second where there were
    // five. Smoothing the rectified signal and asking which low frequency
    // has the most energy in it has no such argument to lose.
    const auto swellsPerSecond = [] (const std::vector<double>& rendered)
    {
        // A one pole follower at about 30 ms, which passes a few swells a
        // second and holds still across a cycle of any note.
        const double coefficient = 1.0 - std::exp (-1.0 / (0.03 * rate));

        std::vector<double> envelope;
        double level = 0.0;

        for (std::size_t i = 0; i < rendered.size(); ++i)
        {
            level += coefficient * (std::fabs (rendered[i]) - level);

            if (i % 64 == 0)
                envelope.push_back (level);
        }

        double mean = 0.0;

        for (double v : envelope)
            mean += v;

        mean /= (double) std::max<std::size_t> (1, envelope.size());

        const double envelopeRate = rate / 64.0;

        double best = 0.0, strongest = 0.0;

        for (double hz = 0.5; hz < 40.0; hz += 0.05)
        {
            double real = 0.0, imaginary = 0.0;

            for (std::size_t i = 0; i < envelope.size(); ++i)
            {
                const double angle = 2.0 * 3.14159265358979323846 * hz
                                       * (double) i / envelopeRate;

                real += (envelope[i] - mean) * std::cos (angle);
                imaginary -= (envelope[i] - mean) * std::sin (angle);
            }

            const double magnitude = std::sqrt (real * real + imaginary * imaginary);

            if (magnitude > strongest)
            {
                strongest = magnitude;
                best = hz;
            }
        }

        return best;
    };

    const int samples = (int) (rate * 2.0);

    const double resting = swellsPerSecond (play (base, 45, samples));

    // Envelope three held wide open, pointed at the rate. The destination
    // declares six octaves at full depth, so a sixth of full depth is one
    // octave, which is exactly twice the speed.
    auto faster = base;
    faster.envelopes[2] = { 0.0005, 0.001, 1.0, 0.050 };
    faster.matrix.add (env3, slope1Rate, 1.0f / 6.0f);

    const double driven = swellsPerSecond (play (faster, 45, samples));

    std::printf ("    resting %s swells a second, driven up an octave %s\n",
                 show (resting).c_str(), show (driven).c_str());

    if (! (resting > 3.0 && resting < 7.0))
    {
        fail ("the slope is not running at the five a second it was set to: "
              + show (resting));
        return;
    }

    // Twice, give or take what counting swells in windows can resolve.
    if (! (driven > resting * 1.6 && driven < resting * 2.5))
        fail ("an octave of rate modulation took the slope from " + show (resting)
              + " a second to " + show (driven) + " rather than to about twice");
}

/** A synced LFO runs at the tempo, and a rate driven upward goes faster.

    Two claims with the same shape as the slope's and the same reason for
    being checked separately from whether the routing works at all. A sync
    that divided where it should multiply still moves the sound, just at the
    wrong speed, and nobody finds that by listening to one patch: they find
    it when a patch that was in time at 120 is not at 140.
*/
void aSyncedLfoFollowsTheTempo()
{
    std::printf ("an LFO against the tempo\n");

    const int lfo1 = Synth::ModMatrix::sourceFromId ("lfo1");
    const int env3 = Synth::ModMatrix::sourceFromId ("env3");
    const int gateOpen = Synth::ModMatrix::destinationFromId ("gateopen");
    const int lfo1Rate = Synth::ModMatrix::destinationFromId ("lfo1rate");

    auto base = plainPatch();
    base.osc1Table = Synth::WaveBank::indexFromId ("analogue");
    base.osc1Level = 0.8;
    base.envelopes[0] = { 0.002, 0.020, 1.0, 0.100 };

    // One cycle a beat, pointed at the gate so the output swells at
    // whatever rate the LFO is really running at.
    base.lfos[0].shape = Synth::Lfo::Shape::sine;
    base.lfos[0].synced = true;
    base.lfos[0].beatsPerCycle = 1.0;
    base.matrix.add (lfo1, gateOpen, 1.0f);

    const auto swellsAt = [&] (const Synth::Voice::Patch& patch, double bpm)
    {
        Synth::Voice voice;
        voice.setSampleRate (rate);
        voice.setBank (&sharedBank());
        voice.setTempo (bpm);
        voice.setPatch (patch);
        voice.start (45, 1.0f);

        std::vector<double> rendered;

        const int samples = (int) (rate * 4.0);

        for (int i = 0; i < samples; ++i)
            rendered.push_back ((double) voice.nextSample());

        // The envelope's own spectrum, as the slope's rate is measured.
        const double coefficient = 1.0 - std::exp (-1.0 / (0.02 * rate));

        std::vector<double> envelope;
        double level = 0.0;

        for (std::size_t i = 0; i < rendered.size(); ++i)
        {
            level += coefficient * (std::fabs (rendered[i]) - level);

            if (i % 64 == 0)
                envelope.push_back (level);
        }

        double mean = 0.0;

        for (double v : envelope)
            mean += v;

        mean /= (double) std::max<std::size_t> (1, envelope.size());

        const double envelopeRate = rate / 64.0;

        double best = 0.0, strongest = 0.0;

        for (double hz = 0.3; hz < 30.0; hz += 0.02)
        {
            double real = 0.0, imaginary = 0.0;

            for (std::size_t i = 0; i < envelope.size(); ++i)
            {
                const double angle = 2.0 * 3.14159265358979323846 * hz
                                       * (double) i / envelopeRate;

                real += (envelope[i] - mean) * std::cos (angle);
                imaginary -= (envelope[i] - mean) * std::sin (angle);
            }

            const double magnitude = std::sqrt (real * real + imaginary * imaginary);

            if (magnitude > strongest)
            {
                strongest = magnitude;
                best = hz;
            }
        }

        return best;
    };

    for (double bpm : { 90.0, 120.0, 150.0 })
    {
        const double measured = swellsAt (base, bpm);
        const double wanted = bpm / 60.0;

        std::printf ("    at %.0f bpm, one cycle a beat measures %s Hz against %s\n",
                     bpm, show (measured).c_str(), show (wanted).c_str());

        if (std::fabs (measured - wanted) > wanted * 0.12)
            fail ("a synced LFO at " + show (bpm) + " bpm runs at " + show (measured)
                  + " Hz where a beat is " + show (wanted));
    }

    // Half a beat a cycle is twice the rate, not half of it.
    {
        auto faster = base;
        faster.lfos[0].beatsPerCycle = 0.5;

        const double measured = swellsAt (faster, 120.0);

        std::printf ("    half a beat a cycle at 120 bpm measures %s Hz\n",
                     show (measured).c_str());

        if (std::fabs (measured - 4.0) > 0.5)
            fail ("half a beat a cycle at 120 bpm runs at " + show (measured)
                  + " Hz rather than four");
    }

    // And the rate destination drives it faster, not slower. Six octaves at
    // full depth, so a sixth of full depth is one octave.
    {
        auto driven = base;
        driven.lfos[0].synced = false;
        driven.lfos[0].rateHz = 3.0;
        driven.envelopes[2] = { 0.0005, 0.001, 1.0, 0.050 };
        driven.matrix.add (env3, lfo1Rate, 1.0f / 6.0f);

        auto resting = driven;
        resting.matrix.routes[resting.matrix.count - 1] = Synth::ModMatrix::Route {};

        const double at = swellsAt (resting, 120.0);
        const double up = swellsAt (driven, 120.0);

        std::printf ("    3 Hz driven up an octave measures %s Hz against %s\n",
                     show (up).c_str(), show (at).c_str());

        if (! (up > at * 1.6 && up < at * 2.5))
            fail ("an octave of rate modulation took an LFO from " + show (at)
                  + " Hz to " + show (up) + " rather than to about twice");
    }
}

/** The voice reports the folder's clean range, and reports it for the rate
    the folder is really running at.

    Worth a check of its own because it is the only number in the voice that
    depends on the folder having been told it is running at four times the
    sample rate. Told the base rate instead, every sample the folder produces
    is still exactly right, and this figure is quietly a quarter of what it
    should be, so an interface built on it would warn about notes that are
    perfectly clean.
*/
void theFoldsCleanlyFigureKnowsTheRate()
{
    std::printf ("the fold's clean range\n");

    Synth::Voice voice;
    voice.setSampleRate (48000.0);
    voice.setBank (&sharedBank());

    for (double drive : { 2.0, 4.0, 8.0 })
    {
        auto patch = plainPatch();
        patch.foldDrive = drive;
        voice.setPatch (patch);

        const double reported = voice.foldsCleanlyBelow();

        // What the folder says at the base rate, which is what it would
        // report if the voice had not told it about the oversampling.
        Fx::WaveFolder atBaseRate;
        atBaseRate.prepareAtRate (48000.0);
        atBaseRate.setDrive (drive);

        const double unoversampled = atBaseRate.foldsCleanlyBelow();

        std::printf ("    drive %.0f: %.0f Hz, against %.0f Hz without the"
                     " oversampling\n", drive, reported, unoversampled);

        if (! (std::fabs (reported - unoversampled * 4.0) < 1.0))
            fail ("the voice reports a clean range of " + show (reported)
                  + " Hz where four times the base rate figure is "
                  + show (unoversampled * 4.0));
    }
}

/** The delay the oversampling costs is reported, and is the delay it really
    costs. A figure nobody checks is a figure that drifts from the thing it
    describes, and this one is handed to the studio to line the instrument up
    with everything else. */
void theReportedLatencyIsReal()
{
    std::printf ("latency\n");

    Synth::Voice voice;
    voice.setSampleRate (rate);
    voice.setBank (&sharedBank());

    auto patch = plainPatch();
    patch.envelopes[0] = { 0.0001, 0.050, 1.0, 0.100 };   // as near a step as it gets

    voice.setPatch (patch);

    const double claimed = voice.latencySamples();

    voice.start (57, 1.0f);

    int firstHeard = -1;

    for (int i = 0; i < 4096 && firstHeard < 0; ++i)
        if (std::fabs ((double) voice.nextSample()) > 1e-4)
            firstHeard = i;

    std::printf ("    claims %s samples, first sample above nothing at %d\n",
                 show (claimed).c_str(), firstHeard);

    if (firstHeard < 0)
    {
        fail ("the voice never made a sound");
        return;
    }

    // The filters ring before their peak as well as after it, so sound
    // arrives a little before the group delay rather than exactly at it.
    // What this catches is a figure that is zero, or wrong by a factor.
    if (! (claimed > 1.0 && (double) firstHeard > claimed * 0.3
                         && (double) firstHeard < claimed * 2.5))
        fail ("the reported latency of " + show (claimed) + " samples does not match"
              " the " + std::to_string (firstHeard) + " it took to make a sound");
}

void theLfoRateFollowsAPatchChange();

/** A patch changed while a note is held takes effect on that note.

    This is not a corner case, it is the ordinary path: the instrument reads
    its knobs once a block and pushes a patch into every voice whenever any
    of them has moved, so turning a filter knob during a held chord arrives
    here. The voice skips retuning and recomputing its filter when nothing it
    depends on has moved, which is most of the time and is worth a fifth of a
    processor core, and the whole of that saving rests on those caches being
    forgotten whenever a patch or a note changes.

    Forgetting to forget is silent and selective: notes started after the
    change are right, notes already sounding keep the old filter and the old
    tuning until something happens to modulate them. Deliberately breaking it
    survived every other check in this file, because every one of them plays
    a note on a fresh voice.
*/
void changingThePatchWhileANoteIsHeldTakesEffect()
{
    std::printf ("a patch changed under a held note\n");

    auto first = plainPatch();
    first.osc1Table = Synth::WaveBank::indexFromId ("analogue");
    first.osc1Position = 0.45;
    first.osc1Level = 0.5;
    first.osc2Table = Synth::WaveBank::indexFromId ("analogue");
    first.osc2Position = 0.45;
    first.osc2Level = 0.5;
    first.osc2Semitones = 0.0;
    first.cutoff = 16000.0;
    first.envelopes[0] = { 0.002, 0.050, 1.0, 0.150 };
    first.gateRange = 0.0;
    first.gateMode = Synth::LowPassGate::Mode::amplitude;

    Synth::Voice voice;
    voice.setSampleRate (rate);
    voice.setBank (&sharedBank());
    voice.setPatch (first);
    voice.start (45, 1.0f);

    std::vector<double> before, after;

    for (int i = 0; i < 8192; ++i)
        before.push_back ((double) voice.nextSample());

    // The filter slammed shut and the second oscillator moved a fifth,
    // with the note never released. Two separate caches, each of which can
    // be forgotten on its own.
    auto second = first;
    second.cutoff = 400.0;
    second.osc2Semitones = 7.0;

    voice.setPatch (second);

    for (int i = 0; i < 8192; ++i)
        after.push_back ((double) voice.nextSample());

    const double was = centroidOf (before);
    const double now = centroidOf (after);

    std::printf ("    centroid before %.0f Hz, after %.0f Hz\n", was, now);

    if (! (now < was * 0.6))
        fail ("closing the filter under a held note did nothing: centroid "
              + show (was) + " Hz to " + show (now));

    // And the pitch half of it, separately, because the two are cached
    // separately and one can be forgotten without the other. The second
    // oscillator moved up a fifth, so there is now energy at a frequency
    // that was not there before.
    const double fifth = frequencyOfNote (45) * 1.4983;

    const auto levelAt = [] (const std::vector<double>& block, double hz)
    {
        double real = 0.0, imaginary = 0.0;

        for (std::size_t i = 0; i < block.size(); ++i)
        {
            const double angle = 2.0 * 3.14159265358979323846 * hz
                                   * (double) i / rate;

            real += block[i] * std::cos (angle);
            imaginary -= block[i] * std::sin (angle);
        }

        return std::sqrt (real * real + imaginary * imaginary) / (double) block.size();
    };

    const double wasThere = levelAt (before, fifth);
    const double isThere = levelAt (after, fifth);

    std::printf ("    at the fifth: %.5f before, %.5f after\n", wasThere, isThere);

    if (! (isThere > wasThere * 3.0))
        fail ("moving an oscillator under a held note did not retune it: "
              + show (wasThere) + " to " + show (isThere) + " at the fifth");

    theLfoRateFollowsAPatchChange();
}

/** And the same for an LFO's rate, which is a third cache of its own.

    In its own check rather than piled onto the one above, because the
    patch change there closes the filter to 400 Hz, and a filter at 400 Hz
    removes everything the fold was doing. The first version of this
    measured the LFO through that filter, read no change, and was right to:
    there was nothing left to hear.
*/
void theLfoRateFollowsAPatchChange()
{
    std::printf ("an LFO rate changed under a held note\n");

    auto slow = plainPatch();
    slow.osc1Table = Synth::WaveBank::indexFromId ("harmonics");
    slow.osc1Position = 0.0;
    slow.osc1Level = 0.8;
    slow.foldMix = 0.9;
    slow.foldDrive = 3.0;
    slow.cutoff = 20000.0;
    slow.gateRange = 0.0;
    slow.gateMode = Synth::LowPassGate::Mode::amplitude;
    slow.gateLag = 0.0;
    slow.envelopes[0] = { 0.002, 0.020, 1.0, 0.100 };

    // Slow enough to barely move across the measurement.
    slow.lfos[0].rateHz = 0.1;
    slow.lfos[0].shape = Synth::Lfo::Shape::triangle;

    // Pointed at the amplitude rather than at the fold, because what is
    // measured below is how much the level moves about. Pointed at the
    // fold the LFO changes the timbre far more than the level, and the
    // first version of this read a 13 per cent change in level from an
    // LFO that was going round four hundred times faster.
    slow.matrix.add (Synth::ModMatrix::sourceFromId ("lfo1"),
                     Synth::ModMatrix::destinationFromId ("amp"), -0.8f);

    auto fast = slow;
    fast.lfos[0].rateHz = 40.0;

    Synth::Voice voice;
    voice.setSampleRate (rate);
    voice.setBank (&sharedBank());
    voice.setPatch (slow);
    voice.start (45, 1.0f);

    std::vector<double> before, after;

    for (int i = 0; i < 8192; ++i)
        before.push_back ((double) voice.nextSample());

    voice.setPatch (fast);

    for (int i = 0; i < 8192; ++i)
        after.push_back ((double) voice.nextSample());

    // How much energy the envelope has at the new rate.
    //
    // Not how much the level varies from one stretch of the sound to the
    // next, which was tried first and is the wrong tool entirely: a faster
    // LFO fits a whole cycle inside every stretch, so every stretch
    // averages to the same thing and the measure reads less variation
    // rather than more. It reported a 13 per cent change from an LFO going
    // round four hundred times faster.
    const auto energyAt = [] (const std::vector<double>& block, double hz)
    {
        // A follower at two milliseconds, which passes forty hertz.
        const double coefficient = 1.0 - std::exp (-1.0 / (0.002 * rate));

        std::vector<double> envelope;
        double level = 0.0;

        for (double v : block)
        {
            level += coefficient * (std::fabs (v) - level);
            envelope.push_back (level);
        }

        double mean = 0.0;

        for (double v : envelope)
            mean += v;

        mean /= (double) std::max<std::size_t> (1, envelope.size());

        double real = 0.0, imaginary = 0.0;

        for (std::size_t i = 0; i < envelope.size(); ++i)
        {
            const double angle = 2.0 * 3.14159265358979323846 * hz
                                   * (double) i / rate;

            real += (envelope[i] - mean) * std::cos (angle);
            imaginary -= (envelope[i] - mean) * std::sin (angle);
        }

        return 2.0 * std::sqrt (real * real + imaginary * imaginary)
                 / (double) envelope.size();
    };

    const double still = energyAt (before, 40.0);
    const double moving = energyAt (after, 40.0);

    std::printf ("    the envelope's energy at 40 Hz: %s before, %s after\n",
                 show (still).c_str(), show (moving).c_str());

    // Measured at 0.019 before and 0.128 after, a factor of seven. The
    // reading before is not zero because a sixth of a second is too short
    // a window to resolve a tenth of a hertz, so the envelope's slow drift
    // leaks into the bin. Held to four, which separates seven cleanly from
    // the one that a forgotten cache would give.
    if (! (moving > still * 4.0))
        fail ("speeding an LFO to 40 Hz under a held note put " + show (moving)
              + " at 40 Hz against " + show (still) + " when it was at a tenth"
                " of a hertz");
}

/** Two LFOs in one voice draw different random numbers.

    Seeded from one number each, so that sample and hold on LFO one and on
    LFO two are two different sequences rather than the same one twice.
    Seeded identically they move together, and a patch using both to
    scatter two things ends up scattering them in lockstep, which looks
    like a modulation that is somehow correlated with itself.

    Measured by cancellation, which is the only way to see it from outside:
    two LFOs pointed at the same destination with opposite depths subtract
    exactly if their sequences are the same, and the output is then
    indistinguishable from no routing at all.
*/
void twoLfosInAVoiceDoNotAgree()
{
    std::printf ("two LFOs in one voice\n");

    const int lfo1 = Synth::ModMatrix::sourceFromId ("lfo1");
    const int lfo2 = Synth::ModMatrix::sourceFromId ("lfo2");
    const int toFold = Synth::ModMatrix::destinationFromId ("fold");

    auto base = plainPatch();
    base.osc1Table = Synth::WaveBank::indexFromId ("harmonics");
    base.osc1Position = 0.0;
    base.osc1Level = 0.8;
    base.foldDrive = 3.0;
    base.foldMix = 0.9;
    base.gateRange = 0.0;
    base.gateMode = Synth::LowPassGate::Mode::amplitude;
    base.gateLag = 0.0;
    base.envelopes[0] = { 0.002, 0.020, 1.0, 0.100 };

    // Both set identically, so the only thing that can tell them apart is
    // the sequence they draw from.
    for (int i = 0; i < 2; ++i)
    {
        base.lfos[i].shape = Synth::Lfo::Shape::sampleAndHold;
        base.lfos[i].rateHz = 60.0;
        base.lfos[i].retrigger = true;
    }

    auto opposed = base;
    opposed.matrix.add (lfo1, toFold, 0.6f);
    opposed.matrix.add (lfo2, toFold, -0.6f);

    const int samples = 8000;

    const auto plain = play (base, 45, samples);
    const auto both = play (opposed, 45, samples);

    double worst = 0.0;

    for (std::size_t i = 0; i < plain.size(); ++i)
        worst = std::max (worst, std::fabs (plain[i] - both[i]));

    worst /= rmsOf (plain) + 1e-12;

    std::printf ("    two opposed sample and holds move the sound by %s\n",
                 show (worst).c_str());

    if (! (worst > 0.05))
        fail ("two LFOs in one voice cancel each other, so they are drawing the"
              " same random numbers: they moved the output by " + show (worst));
}

/** Bringing the fold in from nothing does not click.

    The folder is skipped while it is mixed out, which is most patches most
    of the time and is most of the saving. The cost of skipping is that its
    history is stale when it comes back, and the antialiasing is a difference
    across that history, so the first sample would be computed from a signal
    that stopped being true some time ago.
*/
void bringingTheFoldInDoesNotClick()
{
    std::printf ("bringing the fold in\n");

    const int env3 = Synth::ModMatrix::sourceFromId ("env3");
    const int foldMix = Synth::ModMatrix::destinationFromId ("foldmix");

    auto patch = plainPatch();
    patch.osc1Table = Synth::WaveBank::indexFromId ("harmonics");
    patch.osc1Position = 0.0;
    patch.osc1Level = 0.8;
    patch.foldDrive = 6.0;
    patch.foldMix = 0.0;
    patch.cutoff = 18000.0;
    patch.gateRange = 0.0;
    patch.gateMode = Synth::LowPassGate::Mode::amplitude;
    patch.envelopes[0] = { 0.001, 0.010, 1.0, 0.100 };

    // Held at nothing for a while, then opened quickly, so the moment the
    // folder comes back is sharp and lands where the signal is moving.
    patch.envelopes[2] = { 0.0005, 0.001, 1.0, 0.100 };
    patch.matrix.add (env3, foldMix, 1.0f);

    const auto ramped = play (patch, 45, 8192);

    // The same patch with the folder in circuit the whole way, for what an
    // ordinary step between samples looks like on this sound.
    auto steady = patch;
    steady.matrix.clear();
    steady.foldMix = 1.0;

    const auto always = play (steady, 45, 8192);

    const auto largestStep = [] (const std::vector<double>& v, std::size_t from)
    {
        double worst = 0.0;

        for (std::size_t i = from + 1; i < v.size(); ++i)
            worst = std::max (worst, std::fabs (v[i] - v[i - 1]));

        return worst;
    };

    const double ordinary = largestStep (always, 2000);
    const double crossing = largestStep (ramped, 100);

    std::printf ("    largest step with the fold always in %s, across the entry %s\n",
                 show (ordinary).c_str(), show (crossing).c_str());

    if (! (crossing < ordinary * 1.5))
        fail ("bringing the fold in stepped by " + show (crossing)
              + " against an ordinary " + show (ordinary));
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
    everyDestinationIsReachedOrSaidNotToBe();
    everySourceDrivesSomething();
    aGlideTakesTheTimeItIsGiven();
    theFilterIsAFilter();
    theVoiceDoesNotAlias();
    drivingASlopeFasterMakesItFaster();
    aSyncedLfoFollowsTheTempo();
    theFoldsCleanlyFigureKnowsTheRate();
    theReportedLatencyIsReal();
    changingThePatchWhileANoteIsHeldTakesEffect();
    twoLfosInAVoiceDoNotAgree();
    bringingTheFoldInDoesNotClick();
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
