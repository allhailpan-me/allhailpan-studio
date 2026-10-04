// ---------------------------------------------------------------------------
// Checks the envelope, and mostly checks that it finishes.
//
// The interesting failure here is not a wrong curve, which you would hear. It
// is a release that approaches zero without arriving, which you would not:
// the note fades out, sounds correct, and the voice never frees. The voice
// pool then fills with notes that ended a minute ago, new notes steal voices
// that are still sounding, and the arithmetic runs down into denormals, which
// on some processors are slow enough to matter inside a callback. A synth
// that gets gradually more glitchy the longer you play it, and is fine again
// when you reload, is this bug.
//
// So the checks are mostly about arriving: exactly at one, exactly at the
// sustain level, exactly at zero, and at zero in roughly the time the dial
// says, including when the times are zero or the sample rate is unusual.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include "Envelope.h"

#include <cmath>
#include <cstdio>
#include <string>

static int failures = 0;

static void fail (const std::string& what)
{
    std::printf ("  FAIL  %s\n", what.c_str());
    ++failures;
}

static void check (bool ok, const std::string& what)
{
    if (! ok)
        fail (what);
}

/** Runs until the envelope says it is finished, or gives up. Returns how many
    samples it took, or -1 if it never finished. */
static long runToSilence (Envelope& env, long limit)
{
    for (long i = 0; i < limit; ++i)
    {
        const float s = env.nextSample();

        if (! std::isfinite (s))
            return -2;

        if (! env.isActive())
            return i + 1;
    }

    return -1;
}

static void reachesItsTargetsExactly()
{
    std::printf ("attack, decay and sustain arrive rather than approach\n");

    Envelope env;
    env.setSampleRate (48000.0);
    env.setSettings ({ 0.010, 0.050, 0.5, 0.100 });
    env.noteOn();

    // Attack: 10 ms at 48 kHz is 480 samples, and it must land on exactly one.
    float peak = 0.0f;
    for (int i = 0; i < 480; ++i)
        peak = std::max (peak, env.nextSample());

    check (std::fabs (peak - 1.0f) < 1.0e-5f,
           "attack reached " + std::to_string (peak) + " rather than 1");

    // Decay then settles on the sustain level and stays there exactly.
    for (int i = 0; i < 48000; ++i)
        env.nextSample();

    const float held = env.nextSample();
    check (held == 0.5f, "sustain sat at " + std::to_string (held) + " rather than 0.5");

    for (int i = 0; i < 1000; ++i)
        check (env.nextSample() == 0.5f, "sustain drifted");
}

static void releaseFinishesAtExactlyZero()
{
    std::printf ("the release arrives at zero and the voice frees\n");

    for (double rate : { 22050.0, 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        for (double release : { 0.0, 0.001, 0.05, 0.5, 2.0 })
        {
            Envelope env;
            env.setSampleRate (rate);
            env.setSettings ({ 0.001, 0.010, 0.8, release });
            env.noteOn();

            for (int i = 0; i < (int) (rate * 0.1); ++i)
                env.nextSample();

            env.noteOff();

            // Ten times the release, which is far more than it should need.
            const long limit = (long) (rate * (release + 1.0)) * 10 + 1000;
            const long took = runToSilence (env, limit);

            const std::string where = std::to_string ((int) rate) + " Hz, release "
                                    + std::to_string (release) + " s";

            if (took == -1)      fail (where + ": never finished");
            else if (took == -2) fail (where + ": produced something that was not finite");
            else
            {
                check (env.getLevel() == 0.0f,
                       where + ": finished at " + std::to_string (env.getLevel()) + " rather than zero");

                // Should take about the release time, not wildly more. A
                // release that drags is how a voice pool silently fills.
                const double seconds = (double) took / rate;
                if (seconds > release + 0.05 + release * 0.5)
                    fail (where + ": took " + std::to_string (seconds) + " s to finish");
            }
        }
    }
}

static void staysInRangeAndFinite()
{
    std::printf ("nothing leaves the nought to one range, at any setting\n");

    for (double rate : { 22050.0, 48000.0, 192000.0 })
    {
        for (double t : { 0.0, 0.0001, 0.01, 1.0, 10.0 })
        {
            for (double sustain : { 0.0, 0.001, 0.5, 1.0 })
            {
                Envelope env;
                env.setSampleRate (rate);
                env.setSettings ({ t, t, sustain, t });
                env.noteOn();

                for (int i = 0; i < 5000; ++i)
                {
                    const float s = env.nextSample();

                    if (! std::isfinite (s) || s < 0.0f || s > 1.0f)
                    {
                        fail ("out of range at " + std::to_string ((int) rate) + " Hz, time "
                              + std::to_string (t) + ", sustain " + std::to_string (sustain)
                              + ": " + std::to_string (s));
                        return;
                    }
                }

                env.noteOff();

                for (int i = 0; i < 5000; ++i)
                {
                    const float s = env.nextSample();

                    if (! std::isfinite (s) || s < 0.0f || s > 1.0f)
                    {
                        fail ("out of range during release: " + std::to_string (s));
                        return;
                    }
                }
            }
        }
    }
}

static void zeroTimesDoNotHang()
{
    std::printf ("zero length stages land on the next sample instead of hanging\n");

    Envelope env;
    env.setSampleRate (48000.0);
    env.setSettings ({ 0.0, 0.0, 0.0, 0.0 });
    env.noteOn();

    // Attack, decay and a sustain of zero: it should be done almost at once,
    // which is the degenerate case that an exponential written carelessly
    // turns into an infinite loop or a NaN.
    const float first = env.nextSample();
    check (std::isfinite (first), "a zero length attack produced something that was not finite");

    env.noteOff();
    const long took = runToSilence (env, 10000);
    check (took > 0, "a zero length release never finished");
    check (env.getLevel() == 0.0f, "a zero length release did not end at zero");
}

static void retriggerDoesNotClick()
{
    std::printf ("retriggering mid release carries on from where it was\n");

    Envelope env;
    env.setSampleRate (48000.0);
    env.setSettings ({ 0.050, 0.050, 0.8, 0.500 });
    env.noteOn();

    for (int i = 0; i < 5000; ++i)
        env.nextSample();

    env.noteOff();

    for (int i = 0; i < 2000; ++i)
        env.nextSample();

    const float before = env.getLevel();
    check (before > 0.1f, "the test needs the release to still be audible here");

    env.noteOn();
    const float after = env.nextSample();

    // Starting the attack over from zero is the click. It has to pick up from
    // whatever level the release had reached.
    check (after >= before,
           "retrigger dropped from " + std::to_string (before) + " to " + std::to_string (after));
    check (after - before < 0.1f, "retrigger jumped upward by too much");
}

int main()
{
    std::printf ("envelope\n");

    reachesItsTargetsExactly();
    releaseFinishesAtExactlyZero();
    staysInRangeAndFinite();
    zeroTimesDoNotHang();
    retriggerDoesNotClick();

    if (failures > 0)
    {
        std::printf ("\n%d envelope check(s) failed\n", failures);
        return 1;
    }

    std::printf ("all envelope checks passed\n");
    return 0;
}
