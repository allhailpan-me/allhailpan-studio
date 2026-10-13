// Checks Synth/LowPassGate.h.
//
// A low pass gate is not a filter and an amplifier that happen to share an
// envelope. It is one control doing both, through a part that is slow and
// lopsided, and either half on its own is something else entirely. So the
// checks here are about the two things that identify it rather than about the
// arithmetic:
//
// That quiet and dark arrive together. Feed it something rich, strike it, and
// the brightness of what comes out has to fall as the level falls. That is
// what makes a low pass gate sound like a struck object instead of a sound
// being faded, and it is measurable: the spectral centroid of the output has
// to track the level down. An amplifier with a filter bolted beside it, set
// up carelessly, would not.
//
// And that it is slow to let go. The vactrol lights almost at once and then
// takes a long time to relax, and that asymmetry is in the part rather than
// in any envelope. Measured as the time to open against the time to close,
// which has to be a large ratio and not a small one.
//
// The modes are checked by what they do differently rather than by reading
// back a setting: amplitude only has to leave the balance of the spectrum
// alone while it closes, and the other two have to darken it.

#include "Synth/LowPassGate.h"

#include <algorithm>
#include <cmath>
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
    std::snprintf (buffer, sizeof (buffer), "%.3f", v);
    return buffer;
}

constexpr double pi = 3.14159265358979323846;
constexpr double rate = 48000.0;

/** A signal with plenty in it at every height of the spectrum, so that
    darkening is visible. A sawtooth summed from its harmonics, band limited
    by construction. */
double richSignal (double phase)
{
    double sum = 0.0;

    for (int h = 1; h <= 60; ++h)
        sum += std::sin (2.0 * pi * (double) h * phase) / (double) h;

    return sum * 0.5;
}

/** The centre of gravity of a block's spectrum, in hertz. Higher means
    brighter. */
double centroidOf (const std::vector<double>& block)
{
    const int n = (int) block.size();

    double weighted = 0.0, total = 0.0;

    for (int k = 1; k < n / 2; ++k)
    {
        double real = 0.0, imaginary = 0.0;

        for (int i = 0; i < n; ++i)
        {
            const double angle = 2.0 * pi * (double) k * (double) i / (double) n;
            const double window = 0.5 - 0.5 * std::cos (2.0 * pi * (double) i / (double) (n - 1));

            real += block[(std::size_t) i] * window * std::cos (angle);
            imaginary -= block[(std::size_t) i] * window * std::sin (angle);
        }

        const double magnitude = std::sqrt (real * real + imaginary * imaginary);
        const double hz = (double) k * rate / (double) n;

        weighted += magnitude * hz;
        total += magnitude;
    }

    return total > 0.0 ? weighted / total : 0.0;
}

double rmsOf (const std::vector<double>& block)
{
    double sum = 0.0;

    for (double v : block)
        sum += v * v;

    return std::sqrt (sum / (double) block.size());
}

//==============================================================================
/** The one that matters: as the gate closes, the sound gets quieter and
    darker together. */
void quietAndDarkArriveTogether()
{
    std::printf ("brightness follows level\n");

    Synth::LowPassGate gate;
    gate.setSampleRate (rate);
    gate.setMode (Synth::LowPassGate::Mode::both);
    gate.setRange (7.0);
    gate.setRestingCutoff (60.0);
    gate.setLag (1.0);
    gate.reset();

    double phase = 0.0;
    const double hz = 110.0;

    // Open it and let it settle.
    for (int i = 0; i < (int) (rate * 0.5); ++i)
    {
        gate.process (richSignal (phase), 1.0);
        phase += hz / rate;

        if (phase >= 1.0)
            phase -= 1.0;
    }

    // Then let go, and watch what comes out.
    const int window = 1024;

    std::vector<double> levels, centroids, openness;

    for (int block = 0; block < 14; ++block)
    {
        std::vector<double> out ((std::size_t) window);

        for (int i = 0; i < window; ++i)
        {
            out[(std::size_t) i] = gate.process (richSignal (phase), 0.0);
            phase += hz / rate;

            if (phase >= 1.0)
                phase -= 1.0;
        }

        levels.push_back (rmsOf (out));
        centroids.push_back (centroidOf (out));
        openness.push_back (gate.getOpenness());
    }

    std::printf ("    %-8s %-10s %-10s %-10s\n", "block", "openness", "level", "centroid Hz");

    for (std::size_t i = 0; i < levels.size(); ++i)
        std::printf ("    %-8zu %-10s %-10s %.0f\n", i,
                     show (openness[i]).c_str(), show (levels[i]).c_str(), centroids[i]);

    for (std::size_t i = 1; i < levels.size(); ++i)
    {
        // The level has to keep falling the whole way. Nothing stops it.
        if (! (levels[i] < levels[i - 1]))
            fail ("the level stopped falling at block " + std::to_string (i));

        // The brightness is only asked about while there is still something
        // to be bright.
        //
        // The centre of gravity of a spectrum cannot go below its lowest
        // partial, so once the gate has taken everything above the
        // fundamental the measurement has reached its floor and what is left
        // is noise wandering by a few percent. Judged against the note rather
        // than against a level, because that is what sets the floor.
        if (centroids[i - 1] < hz * 1.6)
            continue;

        if (! (centroids[i] <= centroids[i - 1] * 1.01))
            fail ("the brightness rose as the gate closed, at block "
                  + std::to_string (i));
    }

    // And by a lot, or the gate is barely doing anything.
    const double levelDrop = 20.0 * std::log10 (levels.back() / levels.front());
    const double brightnessDrop = centroids.front() / centroids.back();

    std::printf ("    level fell %s dB, brightness fell by a factor of %s\n",
                 show (levelDrop).c_str(), show (brightnessDrop).c_str());

    if (! (levelDrop < -30.0))
        fail ("the level only fell " + show (levelDrop) + " dB");

    if (! (brightnessDrop > 3.0))
        fail ("the brightness only fell by a factor of " + show (brightnessDrop));
}

/** The vactrol is much slower to let go than to light up. */
void itIsSlowToLetGo()
{
    std::printf ("the vactrol's asymmetry\n");

    Synth::LowPassGate gate;
    gate.setSampleRate (rate);
    gate.setLag (1.0);
    gate.reset();

    // Time to get most of the way open from shut.
    int rising = 0;

    while (gate.getOpenness() < 0.632 && rising < (int) rate)
    {
        gate.process (0.0, 1.0);
        ++rising;
    }

    // Then all the way, and time to get most of the way back.
    for (int i = 0; i < (int) rate; ++i)
        gate.process (0.0, 1.0);

    int falling = 0;

    while (gate.getOpenness() > 0.368 && falling < (int) (rate * 5.0))
    {
        gate.process (0.0, 0.0);
        ++falling;
    }

    const double openMs = 1000.0 * (double) rising / rate;
    const double closeMs = 1000.0 * (double) falling / rate;

    std::printf ("    opens in %s ms, closes in %s ms, a ratio of %s\n",
                 show (openMs).c_str(), show (closeMs).c_str(),
                 show (closeMs / openMs).c_str());

    // The datasheet figures are 12 ms and 250 ms, about twenty times apart.
    // The model runs faster than both because it speeds up with the level,
    // which is what the part does, so what is asserted is the asymmetry
    // rather than the two numbers.
    if (! (closeMs > openMs * 8.0))
        fail ("the gate closes only " + show (closeMs / openMs) + " times slower than it opens");

    if (! (openMs > 1.0 && openMs < 20.0))
        fail ("the gate opens in " + show (openMs) + " ms, which is not what the part does");

    if (! (closeMs > 40.0 && closeMs < 400.0))
        fail ("the gate closes in " + show (closeMs) + " ms, which is not what the part does");
}

/** With no lag it follows the control at once, which is the setting for a
    patch that wants the pairing without the slowness. */
void noLagMeansNoLag()
{
    std::printf ("lag at zero\n");

    Synth::LowPassGate gate;
    gate.setSampleRate (rate);
    gate.setLag (0.0);
    gate.reset();

    gate.process (0.0, 1.0);

    if (! (gate.getOpenness() > 0.999))
        fail ("with no lag the gate took more than a sample to open: "
              + show (gate.getOpenness()));

    gate.process (0.0, 0.0);

    if (! (gate.getOpenness() < 0.001))
        fail ("with no lag the gate took more than a sample to close");

    // And a lag in between is in between, which is the only claim that
    // setting makes.
    Synth::LowPassGate middling;
    middling.setSampleRate (rate);
    middling.setLag (0.5);
    middling.reset();

    int steps = 0;

    while (middling.getOpenness() < 0.632 && steps < (int) rate)
    {
        middling.process (0.0, 1.0);
        ++steps;
    }

    const double ms = 1000.0 * (double) steps / rate;

    if (! (ms > 0.5 && ms < 12.0))
        fail ("half lag opened in " + show (ms) + " ms, which is not between the two");
}

/** The modes do different things, checked by what they do rather than by
    reading the setting back. */
void theModesDiffer()
{
    std::printf ("modes\n");

    const int window = 1024;

    const auto measure = [&] (Synth::LowPassGate::Mode mode, double control)
    {
        Synth::LowPassGate gate;
        gate.setSampleRate (rate);
        gate.setMode (mode);
        gate.setRange (7.0);
        gate.setRestingCutoff (60.0);
        gate.setLag (0.0);          // no lag, so this measures the mode alone
        gate.reset();

        double phase = 0.0;
        std::vector<double> out ((std::size_t) window);

        for (int i = 0; i < window * 4; ++i)
        {
            const double value = gate.process (richSignal (phase), control);

            if (i >= window * 3)
                out[(std::size_t) (i - window * 3)] = value;

            phase += 110.0 / rate;

            if (phase >= 1.0)
                phase -= 1.0;
        }

        return std::pair<double, double> { rmsOf (out), centroidOf (out) };
    };

    for (auto mode : { Synth::LowPassGate::Mode::both,
                       Synth::LowPassGate::Mode::lowpass,
                       Synth::LowPassGate::Mode::amplitude })
    {
        const auto open = measure (mode, 1.0);
        const auto half = measure (mode, 0.45);

        const char* name = mode == Synth::LowPassGate::Mode::both ? "both"
                         : mode == Synth::LowPassGate::Mode::lowpass ? "lowpass" : "amplitude";

        const double levelDb = 20.0 * std::log10 (half.first / open.first + 1e-300);
        const double brightness = half.second / (open.second + 1e-300);

        std::printf ("    %-10s half open: level %s dB, brightness x%s\n",
                     name, show (levelDb).c_str(), show (brightness).c_str());

        if (mode == Synth::LowPassGate::Mode::amplitude)
        {
            // An ordinary gate: quieter, and exactly as bright as it was.
            if (! (levelDb < -5.0))
                fail ("amplitude mode did not get quieter");

            if (! (std::fabs (brightness - 1.0) < 0.02))
                fail ("amplitude mode changed the brightness by x" + show (brightness)
                      + ", so it is filtering when it should not be");
        }
        else
        {
            // The other two darken it.
            if (! (brightness < 0.7))
                fail (std::string (name) + " did not darken: x" + show (brightness));
        }

        if (mode == Synth::LowPassGate::Mode::lowpass)
        {
            // Darker, but not scaled on top of that: a lowpass loses level
            // because it is removing harmonics, not because it is fading.
            if (! (levelDb > -20.0))
                fail ("lowpass mode is attenuating like an amplifier: " + show (levelDb) + " dB");
        }
    }
}

/** Nothing unreasonable breaks it. */
void nothingUnreasonableBreaksIt()
{
    std::printf ("out of range use\n");

    std::mt19937 rng (5150);
    std::uniform_real_distribution<double> audio (-8.0, 8.0);
    std::uniform_real_distribution<double> knob (-5.0, 5.0);
    std::uniform_real_distribution<double> hz (-100.0, 40000.0);

    for (double sampleRate : { 0.0, 22050.0, 48000.0, 192000.0 })
    {
        Synth::LowPassGate gate;
        gate.setSampleRate (sampleRate);
        gate.reset();

        for (int i = 0; i < 40000; ++i)
        {
            if (i % 500 == 0)
            {
                gate.setRange (knob (rng) * 4.0);
                gate.setRestingCutoff (hz (rng));
                gate.setLag (knob (rng));
            }

            const double out = gate.process (audio (rng), knob (rng));

            if (! std::isfinite (out))
            {
                fail ("the output stopped being finite");
                return;
            }

            const double openness = gate.getOpenness();

            if (! (openness >= 0.0 && openness <= 1.0))
            {
                fail ("the vactrol went outside shut and open: " + show (openness));
                return;
            }
        }
    }

    // A control held steady has to arrive at it rather than near it, or a
    // held note drifts in level for as long as it is held.
    Synth::LowPassGate steady;
    steady.setSampleRate (rate);
    steady.setLag (1.0);
    steady.reset();

    for (double target : { 0.0, 0.3, 1.0, 0.7 })
    {
        for (int i = 0; i < (int) (rate * 4.0); ++i)
            steady.process (0.0, target);

        if (! (std::fabs (steady.getOpenness() - target) < 1e-6))
            fail ("held at " + show (target) + " the gate settled at "
                  + show (steady.getOpenness()));
    }
}

} // namespace

int main()
{
    std::printf ("Synth::LowPassGate\n\n");

    quietAndDarkArriveTogether();
    itIsSlowToLetGo();
    noLagMeansNoLag();
    theModesDiffer();
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
