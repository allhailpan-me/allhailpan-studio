// Checks Synth/Slope.h.
//
// The thing to get right is that one object is honestly three. Fired once it
// is an envelope, left to cycle it is an LFO, and cycled fast it is an
// oscillator, and all three have to be true of the same arithmetic rather
// than being three code paths that happen to share a name.
//
// So the checks are mostly about the claims that span those uses. That the
// rate it goes round at is really the two times added together, which is the
// question anybody asks of it once it is cycling and is not either of the
// numbers they set. That the shape control bends the path without moving
// either end, so changing it while something is running changes the sound and
// not the level. And that retriggering it part way through picks up at the
// same height rather than stepping to zero first, which would be a click
// exactly when a patch is at its busiest.

#include "Synth/Slope.h"

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
    std::snprintf (buffer, sizeof (buffer), "%.4f", v);
    return buffer;
}

constexpr double rate = 48000.0;

Synth::Slope make (double rise, double fall, bool cycling, double shape = 0.0)
{
    Synth::Slope s;
    s.setSampleRate (rate);
    s.setTimes (rise, fall);
    s.setShape (shape);
    s.setCycling (cycling);
    s.reset();

    if (! cycling)
        s.trigger();

    return s;
}

//==============================================================================
/** Fired once it rises to the top, comes back down, and stops. */
void firedOnceItIsAnEnvelope()
{
    std::printf ("fired once\n");

    auto slope = make (0.05, 0.20, false);

    double peak = 0.0;
    int peakAt = 0;
    int ran = 0;

    for (int i = 0; i < (int) (rate * 2.0); ++i)
    {
        const double v = slope.nextSample();

        if (v > peak)
        {
            peak = v;
            peakAt = i;
        }

        if (slope.isRunning())
            ran = i;

        if (! (v >= 0.0 && v <= 1.0))
        {
            fail ("the output left zero to one: " + show (v));
            break;
        }
    }

    const double peakSeconds = (double) peakAt / rate;
    const double totalSeconds = (double) ran / rate;

    std::printf ("    peaks at %s s, finishes at %s s\n",
                 show (peakSeconds).c_str(), show (totalSeconds).c_str());

    if (! (peak > 0.999))
        fail ("it only reached " + show (peak) + " rather than the top");

    if (! (std::fabs (peakSeconds - 0.05) < 0.002))
        fail ("it peaked at " + show (peakSeconds) + " s rather than after its rise");

    if (! (std::fabs (totalSeconds - 0.25) < 0.002))
        fail ("it finished at " + show (totalSeconds) + " s rather than after both");

    // And having finished, it stays finished and stays at nothing.
    for (int i = 0; i < 1000; ++i)
        if (slope.nextSample() != 0.0 || slope.isRunning())
        {
            fail ("it started again after finishing");
            break;
        }
}

/** Left to cycle, it goes round at the rate it says it does. */
void cyclingItIsAnLfo()
{
    std::printf ("cycling\n");

    for (auto times : { std::pair<double, double> { 0.5, 2.3 },
                        { 0.01, 0.01 },
                        { 1.4, 1.4 },
                        { 0.002, 0.004 } })
    {
        auto slope = make (times.first, times.second, true);

        const double claimed = slope.cyclesPerSecond();

        // Counted by the turns from falling back to rising, which is one
        // whole trip round.
        int crossings = 0;
        double previous = slope.nextSample();
        bool wasFalling = false;

        const int samples = (int) (rate * 6.0);

        for (int i = 1; i < samples; ++i)
        {
            const double now = slope.nextSample();

            const bool fallingNow = now < previous;

            if (wasFalling && ! fallingNow)
                ++crossings;

            wasFalling = fallingNow;
            previous = now;
        }

        const double measured = (double) crossings / 6.0;

        std::printf ("    rise %s fall %s: claims %s Hz, measures %s Hz\n",
                     show (times.first).c_str(), show (times.second).c_str(),
                     show (claimed).c_str(), show (measured).c_str());

        if (! (std::fabs (measured - claimed) < claimed * 0.02 + 0.2))
            fail ("it claims " + show (claimed) + " Hz and runs at " + show (measured));

        // And the claim is the two times added together, which is the whole
        // point of saying it out loud.
        const double expected = 1.0 / (times.first + times.second);

        if (! (std::fabs (claimed - expected) < 1e-9))
            fail ("the claimed rate is not one over the two times together");
    }
}

/** The shape bends the path and leaves both ends where they were. */
void theShapeMovesThePathAndNotTheEnds()
{
    std::printf ("shape\n");

    for (double shape : { -1.0, -0.5, 0.0, 0.5, 1.0 })
    {
        auto slope = make (0.10, 0.10, false, shape);

        std::vector<double> rising;

        double peak = 0.0;

        for (int i = 0; i < (int) (rate * 0.2); ++i)
        {
            const double v = slope.nextSample();

            peak = std::max (peak, v);

            if (i < (int) (rate * 0.1) - 2)
                rising.push_back (v);
        }

        // Both ends fixed, whatever the shape.
        if (! (peak > 0.999))
            fail ("at shape " + show (shape) + " it only reached " + show (peak));

        // It starts from nothing, checked before a sample is taken rather
        // than after one. A logarithmic shape climbs steeply at the start, so
        // one sample in it is already at 0.06 with nothing wrong: the claim
        // is that the curve passes through zero, not that it is slow to leave
        // it.
        {
            auto fresh = make (0.10, 0.10, false, shape);

            if (fresh.getOutput() != 0.0)
                fail ("at shape " + show (shape) + " it did not start from nothing");
        }

        // Rising the whole way, whatever the bend.
        for (std::size_t i = 1; i < rising.size(); ++i)
            if (! (rising[i] >= rising[i - 1]))
            {
                fail ("at shape " + show (shape) + " the rise went backwards");
                break;
            }

        // And the middle is where the shape actually shows: a log shape is
        // already past halfway at the halfway point, an exponential is not,
        // and the centre of the control is exactly straight.
        const double halfway = rising[rising.size() / 2];

        std::printf ("    shape %-6s halfway through the rise it is at %s\n",
                     show (shape).c_str(), show (halfway).c_str());

        if (shape == 0.0 && ! (std::fabs (halfway - 0.5) < 0.01))
            fail ("the middle of the shape control is not straight: halfway it is at "
                  + show (halfway));

        if (shape < 0.0 && ! (halfway > 0.55))
            fail ("a logarithmic shape is not ahead at halfway: " + show (halfway));

        if (shape > 0.0 && ! (halfway < 0.45))
            fail ("an exponential shape is not behind at halfway: " + show (halfway));
    }
}

/** Retriggering part way through carries on from the same height. */
void retriggeringDoesNotStep()
{
    std::printf ("retrigger\n");

    for (double shape : { -0.7, 0.0, 0.7 })
    {
        auto slope = make (0.05, 0.50, false, shape);

        // Let it get up and come half way down. Half way rather than a
        // fifth, because an exponential fall has barely moved a fifth of the
        // way through and the test would be catching it at the top.
        for (int i = 0; i < (int) (rate * 0.30); ++i)
            slope.nextSample();

        const double before = slope.getOutput();

        if (! (before > 0.2 && before < 0.95))
        {
            fail ("the test did not catch it part way down: " + show (before));
            continue;
        }

        slope.trigger();

        const double after = slope.nextSample();

        std::printf ("    shape %-6s retriggered at %s, next sample %s\n",
                     show (shape).c_str(), show (before).c_str(), show (after).c_str());

        // The step across the retrigger has to be no worse than a normal
        // sample to sample move, which at these times is tiny.
        if (! (std::fabs (after - before) < 0.01))
            fail ("retriggering stepped from " + show (before) + " to " + show (after));

        // And it is going up again afterwards.
        if (! (slope.nextSample() > after))
            fail ("after retriggering it was not rising");
    }
}

/** Cycled fast enough it is an oscillator, which is the third thing it
    claims to be. With both times equal it is a triangle, and a triangle's
    harmonics are the odd ones falling off as one over their square. */
void cycledFastItIsAnOscillator()
{
    std::printf ("cycled at audio rate\n");

    const double wanted = 220.0;

    // Both halves equal, so a triangle at that rate.
    auto slope = make (0.5 / wanted, 0.5 / wanted, true);

    std::printf ("    asked for %s Hz, claims %s Hz\n",
                 show (wanted).c_str(), show (slope.cyclesPerSecond()).c_str());

    if (! (std::fabs (slope.cyclesPerSecond() - wanted) < 0.5))
        fail ("at audio rate it claims " + show (slope.cyclesPerSecond()) + " Hz");

    const int points = 4096;
    std::vector<double> rendered ((std::size_t) points);

    for (int i = 0; i < points; ++i)
        rendered[(std::size_t) i] = slope.nextSample() - 0.5;

    // The harmonics, by hand. A triangle has the odd ones at one over h
    // squared and nothing even.
    const auto harmonic = [&] (int h)
    {
        double real = 0.0, imaginary = 0.0;

        for (int i = 0; i < points; ++i)
        {
            const double angle = 2.0 * 3.14159265358979323846 * (double) h * wanted
                               * (double) i / rate;

            real += rendered[(std::size_t) i] * std::cos (angle);
            imaginary -= rendered[(std::size_t) i] * std::sin (angle);
        }

        return std::sqrt (real * real + imaginary * imaginary) / (double) points;
    };

    const double first = harmonic (1);
    const double second = harmonic (2);
    const double third = harmonic (3);

    std::printf ("    harmonics: second is %s of the first, third is %s\n",
                 show (second / first).c_str(), show (third / first).c_str());

    if (! (second / first < 0.05))
        fail ("it has an even harmonic at " + show (second / first) + " of the first, "
              "so it is not a triangle");

    // A ninth of the first, give or take the window.
    if (! (std::fabs (third / first - 1.0 / 9.0) < 0.03))
        fail ("the third harmonic is at " + show (third / first) + " rather than a ninth");
}

void nothingUnreasonableBreaksIt()
{
    std::printf ("out of range use\n");

    std::mt19937 rng (606);
    std::uniform_real_distribution<double> wild (-1e4, 1e4);

    for (double sampleRate : { 0.0, -1.0, 8000.0, 192000.0 })
    {
        Synth::Slope slope;
        slope.setSampleRate (sampleRate);

        for (int i = 0; i < 40000; ++i)
        {
            if (i % 300 == 0)
            {
                slope.setTimes (wild (rng), wild (rng));
                slope.setShape (wild (rng));
                slope.setCycling (i % 600 == 0);

                if (i % 900 == 0)
                    slope.trigger();
            }

            const double v = slope.nextSample();

            if (! std::isfinite (v))
            {
                fail ("the output stopped being finite");
                return;
            }

            if (! (v >= 0.0 && v <= 1.0))
            {
                fail ("the output left zero to one: " + show (v));
                return;
            }
        }
    }

    // Times of zero cannot make a cycle of no length, which would be a rate
    // of infinity and a division by nothing.
    Synth::Slope instant;
    instant.setSampleRate (rate);
    instant.setTimes (0.0, 0.0);

    if (! std::isfinite (instant.cyclesPerSecond()))
        fail ("with both times at zero the rate is not a number");
}

} // namespace

int main()
{
    std::printf ("Synth::Slope\n\n");

    firedOnceItIsAnEnvelope();
    cyclingItIsAnLfo();
    theShapeMovesThePathAndNotTheEnds();
    retriggeringDoesNotStep();
    cycledFastItIsAnOscillator();
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
