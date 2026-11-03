// ---------------------------------------------------------------------------
// Checks the envelope: that it arrives, that it arrives when the dials say,
// and that the shape between the arrivals is the one the header claims.
//
// The failure the first version of this file was written for is still the
// worst one: a release that approaches zero without arriving, which you would
// not hear. The note fades out, sounds correct, and the voice never frees.
// The voice pool then fills with notes that ended a minute ago, new notes
// steal voices that are still sounding, and the arithmetic runs down into
// denormals, which on some processors are slow enough to matter inside a
// callback. A synth that gets gradually more glitchy the longer you play it,
// and is fine again when you reload, is this bug.
//
// Everything past that was added after a mutation run on 2026-11-03 broke the
// header thirty six ways and the file as it stood caught eleven. What it could
// not see was everything about *time and shape*: an attack at twice the dialled
// speed, a decay stage that never ran at all so the level stepped from full to
// sustain in one sample, a decay driven by the release dial, a release falling
// in a straight line instead of exponentially, and a release cut off twenty
// decibels early, which is a click at the end of every note. All of those
// still arrived at the right places, and arriving was all that was checked.
//
// So the checks here are in three groups:
//
//   * arriving: exactly at one, exactly at the sustain level, exactly at zero,
//     at every sample rate, and with the times at their limits.
//   * when: the attack, decay and release each take the time their dial says,
//     measured against the closed form the header's own rule implies.
//   * what shape: the attack straight, the decay and release straight on a
//     decibel scale, and the last audible sample of a release at least sixty
//     decibels down, which is what makes the snap inaudible.
//
// Two of the measurements are pointed at a deliberately wrong curve first and
// have to notice, because a measurement nobody has seen fail is not evidence.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include "Envelope.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

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

static std::string two (double v)
{
    char buffer[64];
    std::snprintf (buffer, sizeof (buffer), "%.4f", v);
    return buffer;
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

/** The fraction of the dialled decay time the decay stage actually lasts.

    The header's rule has two halves that only agree when the sustain is zero.
    The per sample multiplier covers a thousandth of the *remaining distance*
    in the dialled time, and the stage ends when that distance is a thousandth
    of *full scale*. So a decay into a high sustain, which has little distance
    to cover, hits the absolute floor before it has used its time, and ends
    early by exactly this much. The shortfall grows as the remaining movement
    becomes inaudible: at a sustain of 0.9 the whole decay is one decibel.

    Worth having as a formula rather than a table of measurements, because it
    is the claim being checked, and a table would have to be regenerated from
    the code it is meant to be checking. */
static double decayFractionOfDial (double sustain) noexcept
{
    const double distance = 1.0 - sustain;

    if (distance <= 0.001)
        return 0.0;                       // already inside the floor: snaps at once

    return 1.0 - std::log (distance) / std::log (0.001);
}

//==============================================================================
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
           "attack reached " + two (peak) + " rather than 1");

    // Decay then settles on the sustain level and stays there exactly.
    for (int i = 0; i < 48000; ++i)
        env.nextSample();

    const float held = env.nextSample();
    check (held == 0.5f, "sustain sat at " + two (held) + " rather than 0.5");

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
                                    + two (release) + " s";

            if (took == -1)      fail (where + ": never finished");
            else if (took == -2) fail (where + ": produced something that was not finite");
            else
            {
                check (env.getLevel() == 0.0f,
                       where + ": finished at " + two (env.getLevel()) + " rather than zero");

                // Should take about the release time, not wildly more. A
                // release that drags is how a voice pool silently fills.
                const double seconds = (double) took / rate;
                if (seconds > release + 0.05 + release * 0.5)
                    fail (where + ": took " + two (seconds) + " s to finish");
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
                              + two (t) + ", sustain " + two (sustain)
                              + ": " + two (s));
                        return;
                    }
                }

                env.noteOff();

                for (int i = 0; i < 5000; ++i)
                {
                    const float s = env.nextSample();

                    if (! std::isfinite (s) || s < 0.0f || s > 1.0f)
                    {
                        fail ("out of range during release: " + two (s));
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
    check (first == 1.0f,
           "a zero length attack reached " + two (first) + " on its first sample rather than 1");

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
           "retrigger dropped from " + two (before) + " to " + two (after));
    check (after - before < 0.1f, "retrigger jumped upward by too much");
}

//==============================================================================
// When: each dial is the time the stage takes.
//==============================================================================

static void attackTakesTheTimeItsDialSays()
{
    std::printf ("the attack arrives when its dial says, not sooner\n");

    double worst = 0.0;
    std::string worstWhere;

    for (double rate : { 22050.0, 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        for (double attack : { 0.0005, 0.001, 0.005, 0.05, 0.5, 5.0 })
        {
            Envelope env;
            env.setSampleRate (rate);
            env.setSettings ({ attack, 1.0, 0.5, 0.2 });
            env.noteOn();

            // The envelope leaves the attack stage on the sample it reaches
            // one, so the first sample that reads one is the arrival.
            const long expected = (long) std::ceil (attack * rate);
            const long limit    = expected * 4 + 1000;
            long arrived = -1;

            for (long i = 1; i <= limit; ++i)
                if (env.nextSample() >= 1.0f)
                {
                    arrived = i;
                    break;
                }

            const std::string where = std::to_string ((int) rate) + " Hz, attack "
                                    + two (attack) + " s";

            if (arrived < 0)
            {
                fail (where + ": the attack never reached one");
                continue;
            }

            // Within a sample either way. This is the check that a dial
            // reading in seconds means seconds: an attack at twice the
            // dialled speed is the difference between a swell and a click,
            // and the old file could not see it because it only ever asked
            // whether the attack had got there by the end of its time.
            const double error = (double) (arrived - expected) / (double) expected;

            if (std::fabs (error) > std::fmax (1.5 / (double) expected, 0.01))
                fail (where + ": arrived at sample " + std::to_string (arrived)
                      + " where the dial says " + std::to_string (expected));

            if (std::fabs (error) >= worst || worstWhere.empty())
            {
                worst = std::fabs (error);
                worstWhere = where;
            }
        }
    }

    std::printf ("    worst attack arrival error %.4f%% (%s)\n", worst * 100.0, worstWhere.c_str());
}

static void decayTakesTheTimeItsDialSays()
{
    std::printf ("the decay stage lasts the fraction of its dial the header's rule implies\n");

    double worst = 0.0;
    std::string worstWhere;

    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        for (double decay : { 0.01, 0.1, 1.0, 8.0 })
        {
            for (double sustain : { 0.0, 0.25, 0.5, 0.7, 0.9, 0.99 })
            {
                Envelope env;
                env.setSampleRate (rate);
                // A zero attack lands on the next sample, so sample one is
                // the top and the decay starts at sample two. That keeps the
                // attack out of the figure being measured.
                env.setSettings ({ 0.0, decay, sustain, 0.2 });
                env.noteOn();
                env.nextSample();

                // The snap onto the sustain level is a jump of more than a
                // thousandth of full scale, so arriving within a millionth of
                // it is unambiguously the end of the stage and cannot be
                // confused with the approach to it.
                const long limit = (long) (decay * rate) * 4 + 1000;
                long ran = -1;

                for (long i = 1; i <= limit; ++i)
                    if (std::fabs ((double) env.nextSample() - sustain) < 1.0e-6)
                    {
                        ran = i;
                        break;
                    }

                const std::string where = std::to_string ((int) rate) + " Hz, decay "
                                        + two (decay) + " s, sustain " + two (sustain);

                if (ran < 0)
                {
                    fail (where + ": the decay stage never ended");
                    continue;
                }

                const double expected = decayFractionOfDial (sustain) * decay * rate;
                const double error    = ((double) ran - expected) / expected;

                if (std::fabs (error) > std::fmax (2.0 / expected, 0.02))
                    fail (where + ": the decay ran " + two ((double) ran / rate)
                          + " s where the rule says " + two (expected / rate) + " s");

                if (std::fabs (error) > worst)
                {
                    worst = std::fabs (error);
                    worstWhere = where;
                }
            }
        }
    }

    std::printf ("    worst decay length error %.4f%% (%s)\n", worst * 100.0, worstWhere.c_str());
}

static void releaseTakesTheTimeItsDialSays()
{
    std::printf ("the release from full scale lasts exactly its dial\n");

    double worst = 0.0;
    std::string worstWhere;

    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        for (double release : { 0.001, 0.01, 0.1, 1.0, 10.0 })
        {
            Envelope env;
            env.setSampleRate (rate);
            // Sustain at one, so the note is released from full scale, which
            // is the level the header's claim is about: the release time is
            // how long it takes to fall to a thousandth of full scale.
            env.setSettings ({ 0.0, 0.01, 1.0, release });
            env.noteOn();
            env.nextSample();
            env.noteOff();

            const long expected = (long) (release * rate);
            const long took     = runToSilence (env, expected * 4 + 1000);

            const std::string where = std::to_string ((int) rate) + " Hz, release "
                                    + two (release) + " s";

            if (took <= 0)
            {
                fail (where + ": the release never finished");
                continue;
            }

            const double error = ((double) took - (double) expected) / (double) expected;

            if (std::fabs (error) > std::fmax (2.0 / (double) expected, 0.02))
                fail (where + ": took " + two ((double) took / rate)
                      + " s where the dial says " + two (release) + " s");

            if (std::fabs (error) > worst)
            {
                worst = std::fabs (error);
                worstWhere = where;
            }
        }
    }

    std::printf ("    worst release length error %.4f%% (%s)\n", worst * 100.0, worstWhere.c_str());
}

//==============================================================================
// What shape: straight for the attack, straight in decibels for the other two.
//==============================================================================

/** Largest departure from a straight rise from nought to one, as a fraction of
    full scale. Fed a list of levels covering the attack. */
static double worstDeparture (const std::vector<float>& levels)
{
    double worst = 0.0;
    const double last = (double) levels.size() - 1.0;

    for (std::size_t i = 0; i < levels.size(); ++i)
        worst = std::fmax (worst, std::fabs ((double) levels[i] - (double) i / last));

    return worst;
}

static void theAttackIsStraight()
{
    std::printf ("the attack rises in a straight line\n");

    Envelope env;
    env.setSampleRate (48000.0);
    env.setSettings ({ 0.100, 0.100, 0.5, 0.100 });
    env.noteOn();

    std::vector<float> levels;
    levels.push_back (0.0f);
    for (int i = 0; i < 4800; ++i)
        levels.push_back (env.nextSample());

    const double departure = worstDeparture (levels);
    std::printf ("    worst departure from the line %.6f of full scale\n", departure);

    check (departure < 1.0e-4,
           "the attack departs from a straight line by " + two (departure));

    // The measurement pointed at something known to be curved, so that a pass
    // above means something. An exponential approach to one, which is what an
    // attack written the same way as the decay would give, has to be caught.
    std::vector<float> curved;
    double level = 0.0;
    for (std::size_t i = 0; i < levels.size(); ++i)
    {
        curved.push_back ((float) level);
        level += (1.0 - level) * 2.0 / (double) levels.size();
    }

    check (worstDeparture (curved) > 1.0e-4,
           "the straightness measurement cannot see an exponential attack, so it proves nothing");
}

/** How loud the last sample before the snap is allowed to be.

    The snap fires on the first sample at or under a thousandth of full scale,
    so the sample before it is one step of the release's own travel above that.
    A release covers sixty decibels in N samples, so one step is 1000^(1/N),
    which for the shortest release the dial offers, a millisecond, is 1.15 and
    puts the step to zero at -58.8 dB rather than -60. For anything from ten
    milliseconds upward it is within a twentieth of a decibel of the floor.

    Derived rather than measured into a table, because the point of the check
    is that the step to zero is inaudible, and a table of what the code
    currently does could not fail. */
static double loudestAllowedLastSample (double release, double rate) noexcept
{
    const double samples = release * rate;

    if (samples < 1.0)
        return 0.0;                       // snapped on its first sample

    // A hair of slack, because a release from exactly full scale lands exactly
    // on this bound and the level has been through a float on the way out, so
    // the comparison would otherwise turn on the last bit of it. Four
    // thousandths of a decibel.
    return 0.001 * std::pow (1000.0, 1.0 / samples) * 1.0005;
}

/** Largest departure, in decibels, from a fall of sixty decibels over the
    given number of samples. Only levels above the floor are judged, because
    below it the stage has snapped and there is nothing left to measure. */
static double worstDecibelDeparture (const std::vector<double>& distances,
                                     double samplesForSixtyDb,
                                     double floorValue,
                                     double reference)
{
    double worst = 0.0;

    for (std::size_t i = 0; i < distances.size(); ++i)
    {
        if (distances[i] <= floorValue)
            break;

        const double measured = 20.0 * std::log10 (distances[i] / reference);
        const double wanted   = -60.0 * ((double) (i + 1) / samplesForSixtyDb);

        worst = std::fmax (worst, std::fabs (measured - wanted));
    }

    return worst;
}

static void theDecayAndReleaseAreExponential()
{
    std::printf ("the decay and the release fall sixty decibels over their dial\n");

    const double rate = 48000.0;

    // The decay, measured as the distance still to cover. Sixty decibels of
    // that distance over the dialled time is what makes the dial mean seconds.
    {
        Envelope env;
        env.setSampleRate (rate);
        env.setSettings ({ 0.0, 0.5, 0.25, 0.2 });
        env.noteOn();
        env.nextSample();

        std::vector<double> distances;
        for (int i = 0; i < 24000; ++i)
            distances.push_back ((double) env.nextSample() - 0.25);

        const double departure = worstDecibelDeparture (distances, 0.5 * rate, 0.002, 0.75);
        std::printf ("    worst decay departure %.4f dB\n", departure);
        check (departure < 0.2, "the decay is not exponential: off by " + two (departure) + " dB");
    }

    // The release, measured as the level itself.
    {
        Envelope env;
        env.setSampleRate (rate);
        env.setSettings ({ 0.0, 0.01, 1.0, 0.5 });
        env.noteOn();
        env.nextSample();
        env.noteOff();

        std::vector<double> levels;
        for (int i = 0; i < 24000; ++i)
            levels.push_back ((double) env.nextSample());

        const double departure = worstDecibelDeparture (levels, 0.5 * rate, 0.002, 1.0);
        std::printf ("    worst release departure %.4f dB\n", departure);
        check (departure < 0.2, "the release is not exponential: off by " + two (departure) + " dB");

        // And the measurement has to be able to see a release that falls in a
        // straight line instead, which is what multiplying written as a
        // subtraction gives and which the old file could not tell apart.
        std::vector<double> straight;
        for (std::size_t i = 0; i < levels.size(); ++i)
            straight.push_back (std::fmax (0.0, 1.0 - (double) i / (0.5 * rate)));

        check (worstDecibelDeparture (straight, 0.5 * rate, 0.002, 1.0) > 0.2,
               "the decibel measurement cannot see a straight line release, so it proves nothing");
    }
}

static void theLastAudibleSampleIsSixtyDecibelsDown()
{
    std::printf ("a release is inaudible before it is snapped to zero\n");

    double loudest = 0.0;
    std::string loudestWhere;

    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        for (double release : { 0.001, 0.01, 0.2, 2.0 })
        {
            for (double sustain : { 1.0, 0.7, 0.2 })
            {
                Envelope env;
                env.setSampleRate (rate);
                env.setSettings ({ 0.0, 0.001, sustain, release });
                env.noteOn();
                for (int i = 0; i < (int) (rate * 0.05); ++i)
                    env.nextSample();
                env.noteOff();

                float last = 0.0f;
                for (long i = 0; i < (long) (release * rate) * 4 + 1000; ++i)
                {
                    const float s = env.nextSample();

                    if (! env.isActive())
                        break;

                    last = s;
                }

                const std::string where = std::to_string ((int) rate) + " Hz, release "
                                        + two (release) + " s, sustain " + two (sustain);

                // This is the whole justification for snapping: the step to
                // zero happens from somewhere nobody can hear. At a tenth of
                // full scale the same step is a click at the end of every
                // note, and the old file could not see the difference because
                // it only asked whether the envelope had stopped.
                const double allowed = loudestAllowedLastSample (release, rate);

                if ((double) last > allowed)
                    fail (where + ": the last sample before silence was " + two (last)
                          + " (" + two (20.0 * std::log10 (std::fmax ((double) last, 1.0e-12)))
                          + " dB), where one sample above the floor is " + two (allowed));

                // And a release long enough for the grid to be fine is at the
                // floor itself, which is the claim the header makes.
                if (release >= 0.01 && last > 0.0011f)
                    fail (where + ": a release with room to resolve stepped to zero from "
                          + two (last));

                if (last > loudest)
                {
                    loudest = last;
                    loudestWhere = where;
                }
            }
        }
    }

    std::printf ("    loudest last sample %.6f (%.2f dB) (%s)\n",
                 loudest, 20.0 * std::log10 (std::fmax (loudest, 1.0e-12)), loudestWhere.c_str());
}

//==============================================================================
// The decay stage has to end, and the sustain has to be a level rather than
// the place an exponential is heading.
//==============================================================================

static void theDecayStageEndsRatherThanApproaching()
{
    std::printf ("the decay stage ends, so the sustain knob reaches a note already sounding\n");

    Envelope env;
    env.setSampleRate (48000.0);
    env.setSettings ({ 0.0, 0.02, 0.5, 0.2 });
    env.noteOn();

    // Well past the end of a 20 ms decay.
    for (int i = 0; i < 48000; ++i)
        env.nextSample();

    // The instrument reads its knobs once a block and pushes a patch into
    // every voice, so this is an ordinary thing to do to a held note. In the
    // sustain stage the level is assigned, so it arrives on the next sample.
    // An envelope still grinding through a decay it never finished would ease
    // toward the new level instead, over its decay time, and the note would
    // lag the knob by as much as eight seconds.
    Envelope::Settings moved = env.getSettings();
    moved.sustainLevel = 0.2;
    env.setSettings (moved);

    const float after = env.nextSample();
    check (std::fabs (after - 0.2f) < 1.0e-6f,
           "a sustain moved to 0.2 read " + two (after) + " on the next sample");

    // And it stays where it was put rather than drifting back.
    for (int i = 0; i < 1000; ++i)
        check (env.nextSample() == 0.2f, "the sustain drifted after the knob moved");
}

static void theDecayNeverUndershootsTheSustain()
{
    std::printf ("the decay falls toward the sustain level, never past it\n");

    for (double sustain : { 0.1, 0.5, 0.9 })
    {
        Envelope env;
        env.setSampleRate (48000.0);
        env.setSettings ({ 0.005, 0.2, sustain, 0.2 });
        env.noteOn();

        float lowest = 1.0f;
        float previous = 0.0f;
        bool  peaked = false;

        for (int i = 0; i < 48000; ++i)
        {
            const float s = env.nextSample();

            if (s >= 1.0f)
                peaked = true;

            if (peaked)
            {
                lowest = std::min (lowest, s);

                // A decay heading for zero rather than for the sustain level
                // dips below it and is then snapped back up, which is a step
                // upward in the middle of every note. Catching the dip is the
                // same check as catching the step.
                if (s < (float) sustain - 1.0e-6f)
                    fail ("the decay fell to " + two (s) + " below a sustain of " + two (sustain));
            }

            previous = s;
        }

        check (std::fabs (previous - (float) sustain) < 1.0e-6f,
               "the note settled at " + two (previous) + " rather than its sustain of "
               + two (sustain));
        check (lowest >= (float) sustain - 1.0e-6f,
               "the lowest level reached was " + two (lowest) + " against a sustain of "
               + two (sustain));
    }
}

static void theDecayStageActuallyRuns()
{
    std::printf ("there is a decay between the top and the sustain, not a step\n");

    Envelope env;
    env.setSampleRate (48000.0);
    env.setSettings ({ 0.0, 0.1, 0.5, 0.2 });
    env.noteOn();
    env.nextSample();                      // a zero attack: the top

    // The biggest single step anywhere in the decay. Over 4800 samples from
    // 1.0 to 0.5 nothing should move by more than a small fraction of the
    // distance. A decay stage that never runs, or one snapped on its first
    // sample, puts the whole half of full scale into one sample, which is the
    // click this check exists for.
    float previous = 1.0f;
    double biggest = 0.0;

    for (int i = 0; i < 4800; ++i)
    {
        const float s = env.nextSample();
        biggest = std::fmax (biggest, std::fabs ((double) previous - (double) s));
        previous = s;
    }

    std::printf ("    biggest single step in the decay %.6f of full scale\n", biggest);
    check (biggest < 0.01,
           "the decay moved " + two (biggest) + " of full scale in one sample");
}

//==============================================================================
// The guards, and the parts nothing in the studio happens to reach today.
//==============================================================================

static void settingsAreClampedToWhatTheyMean()
{
    std::printf ("settings outside their range are brought back inside\n");

    Envelope env;
    env.setSampleRate (48000.0);

    // The dials in PanOneParams cannot produce any of these. A preset that
    // assigns the Patch struct directly can, and twenty two of them do, so
    // this is the guard that stands between a typo in a preset and an
    // envelope that runs backwards.
    env.setSettings ({ -1.0, -2.0, -0.5, -3.0 });
    check (env.getSettings().attackSeconds == 0.0,  "a negative attack was not clamped");
    check (env.getSettings().decaySeconds == 0.0,   "a negative decay was not clamped");
    check (env.getSettings().releaseSeconds == 0.0, "a negative release was not clamped");
    check (env.getSettings().sustainLevel == 0.0,   "a negative sustain was not clamped");

    env.setSettings ({ 0.01, 0.01, 2.0, 0.01 });
    check (env.getSettings().sustainLevel == 1.0,   "a sustain above one was not clamped");

    // And a negative sustain cannot make the envelope go negative.
    env.setSettings ({ 0.001, 0.01, -0.5, 0.01 });
    env.noteOn();
    for (int i = 0; i < 1000; ++i)
    {
        const float s = env.nextSample();
        if (s < 0.0f)
        {
            fail ("a negative sustain produced " + two (s));
            break;
        }
    }
}

static void anUnusableSampleRateFallsBack()
{
    std::printf ("a sample rate of nought does not make every stage instant\n");

    for (double rate : { 0.0, -48000.0 })
    {
        Envelope env;
        env.setSampleRate (rate);
        env.setSettings ({ 0.010, 0.050, 0.5, 0.100 });
        env.noteOn();

        // The fallback is 44100, so a 10 ms attack is 441 samples. What this
        // is really checking is that the attack is a slope at all: a rate of
        // nought divides by zero, and every stage then lands on its first
        // sample, which is an envelope generator that does nothing.
        const float first = env.nextSample();

        check (first > 0.0f && first < 0.01f,
               "at a rate of " + two (rate) + " the first sample was " + two (first)
               + ", so the attack is not a slope");
    }
}

static void anEndlessAttackStillArrives()
{
    std::printf ("an attack too long to measure still arrives rather than hanging\n");

    // Unreachable from the dial, which stops at five seconds, and reachable
    // from a preset assigning the struct. The guard is for an attack step that
    // has underflowed to nothing, which needs the attack time multiplied by
    // the sample rate to overflow, so 1e300 seconds is not it: that is a
    // finite ramp of 1e300 seconds and it correctly takes 1e300 seconds. A
    // linear ramp of literally no slope is the case where the stage never ends
    // at all, and a voice that never leaves its attack is a voice that never
    // frees.
    for (double attack : { 1.0e306, std::numeric_limits<double>::infinity() })
    {
        Envelope env;
        env.setSampleRate (48000.0);
        env.setSettings ({ attack, 0.01, 0.5, 0.01 });
        env.noteOn();

        const float first = env.nextSample();
        check (first == 1.0f,
               "an attack of " + two (attack) + " s gave " + two (first)
               + " on its first sample rather than arriving at once");
    }
}

static void resetLeavesNothingBehind()
{
    std::printf ("a killed voice keeps nothing that the next note can hear\n");

    Envelope env;
    env.setSampleRate (48000.0);
    env.setSettings ({ 0.100, 0.100, 0.8, 0.500 });
    env.noteOn();

    for (int i = 0; i < 2000; ++i)
        env.nextSample();

    check (env.getLevel() > 0.1f, "the test needs the envelope to be well up here");

    // Voice::kill calls this when there is nothing left to steal, and then the
    // stealing note's noteOn follows. noteOn deliberately does not reset the
    // level, so if reset did not either, the new note would start part way up
    // the old one: a step at exactly the moment a steal happens, which is the
    // moment kill exists to make orderly.
    env.reset();
    check (env.getLevel() == 0.0f, "reset left the level at " + two (env.getLevel()));
    check (! env.isActive(), "reset left the envelope active");

    env.noteOn();
    const float first = env.nextSample();
    const double step = 1.0 / (0.100 * 48000.0);

    check (std::fabs ((double) first - step) < 1.0e-6,
           "after a reset the next note started at " + two (first)
           + " rather than one attack step, " + two (step));
}

static void noteOffOnAnIdleEnvelopeChangesNothing()
{
    std::printf ("a note off with no note under it leaves the voice free\n");

    Envelope env;
    env.setSampleRate (48000.0);
    env.setSettings ({ 0.01, 0.01, 0.5, 0.2 });

    check (! env.isActive(), "a fresh envelope reported itself active");

    env.noteOff();
    check (! env.isActive(),
           "a note off on an idle envelope made it active, so the allocator cannot see the voice is free");
    check (env.getLevel() == 0.0f, "a note off on an idle envelope moved the level");
}

static void releasingIsReportedForTheReleaseAlone()
{
    std::printf ("isReleasing is true for the release and nothing else\n");

    Envelope env;
    env.setSampleRate (48000.0);
    env.setSettings ({ 0.01, 0.01, 0.5, 0.2 });

    check (! env.isReleasing(), "an idle envelope said it was releasing");

    env.noteOn();
    env.nextSample();
    check (! env.isReleasing(), "an attacking envelope said it was releasing");

    for (int i = 0; i < 48000; ++i)
        env.nextSample();
    check (! env.isReleasing(), "a sustaining envelope said it was releasing");

    env.noteOff();
    check (env.isReleasing(), "a released envelope did not say it was releasing");

    runToSilence (env, 48000);
    check (! env.isReleasing(), "a finished envelope still said it was releasing");
}

static void itsOwnDefaultsAreTheOnesItUses()
{
    std::printf ("a fresh envelope behaves like the settings it declares\n");

    // Settings declares a 5 ms attack, and nothing in the class is allowed to
    // need a setter called first before that is true. This is the shape of
    // fault that a sample rate change left in the state variable filter: an
    // object whose declared defaults and whose actual state disagree works
    // perfectly everywhere that configures it and wrongly in the one place
    // that forgets.
    Envelope env;
    env.noteOn();

    const Envelope::Settings declared;
    const double step = 1.0 / (declared.attackSeconds * 44100.0);
    const float first = env.nextSample();

    check (std::fabs ((double) first - step) < 1.0e-6,
           "a fresh envelope's first sample was " + two (first) + " rather than one attack step, "
           + two (step) + ", so it is not using the attack it declares");

    const long arrived = 1 + [&env]
    {
        for (long i = 1; i < 100000; ++i)
            if (env.nextSample() >= 1.0f)
                return i;
        return (long) -1;
    }();

    const long expected = (long) std::ceil (declared.attackSeconds * 44100.0);
    check (std::labs (arrived - expected) <= 1,
           "a fresh envelope arrived at sample " + std::to_string (arrived)
           + " where its declared 5 ms attack is sample " + std::to_string (expected));
}

//==============================================================================
// Randomised, because the awkward settings here are ones nobody would write
// down: a decay of a sample and a half, a sustain a hair under one, a release
// of ten seconds at 192 kHz.
//==============================================================================

static std::uint64_t seed = 0x9e3779b97f4a7c15ull;

static double nextRandom() noexcept
{
    seed += 0x9e3779b97f4a7c15ull;
    std::uint64_t z = seed;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    return (double) (z >> 11) * (1.0 / 9007199254740992.0);
}

static double between (double low, double high) noexcept
{
    return low + nextRandom() * (high - low);
}

/** Uniform in the logarithm, which is both how a time dial is laid out and
    where the awkward cases are: a decay of a sample and a half is a different
    kind of setting from a decay of eight seconds, and drawing uniformly in
    seconds would spend almost the whole sweep at the slow end rendering
    minutes of audio nobody is in any doubt about. */
static double betweenInOctaves (double low, double high) noexcept
{
    return std::exp (between (std::log (low), std::log (high)));
}

static void randomisedSettingsAllBehave()
{
    std::printf ("ten thousand randomised settings, all of which have to behave\n");

    const double rates[] = { 22050.0, 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
    int checked = 0;
    double worstRelease = 0.0;

    for (int trial = 0; trial < 10000; ++trial)
    {
        const double rate    = rates[(std::size_t) (nextRandom() * 6.0) % 6];
        const double attack  = betweenInOctaves (0.0005, 5.0);
        const double decay   = betweenInOctaves (0.001, 8.0);
        const double sustain = between (0.0, 1.0);
        const double release = betweenInOctaves (0.001, 10.0);

        Envelope env;
        env.setSampleRate (rate);
        env.setSettings ({ attack, decay, sustain, release });
        env.noteOn();

        const std::string where = std::to_string ((int) rate) + " Hz, A " + two (attack)
                                + " D " + two (decay) + " S " + two (sustain)
                                + " R " + two (release);

        // The attack arrives where the dial says.
        const long expectedAttack = (long) std::ceil (attack * rate);
        long arrived = -1;

        for (long i = 1; i <= expectedAttack * 2 + 100; ++i)
            if (env.nextSample() >= 1.0f)
            {
                arrived = i;
                break;
            }

        if (arrived < 0)
        {
            fail (where + ": the attack never arrived");
            return;
        }

        if (std::labs (arrived - expectedAttack) > 1 + expectedAttack / 50)
        {
            fail (where + ": the attack arrived at " + std::to_string (arrived)
                  + " where the dial says " + std::to_string (expectedAttack));
            return;
        }

        // The decay settles on the sustain level without going under it.
        const long decayLimit = (long) (decay * rate) + 100;
        for (long i = 0; i < decayLimit; ++i)
        {
            const float s = env.nextSample();

            if (! std::isfinite (s) || s < (float) sustain - 1.0e-6f || s > 1.0f)
            {
                fail (where + ": the decay reached " + two (s));
                return;
            }
        }

        if (std::fabs ((double) env.getLevel() - sustain) > 1.0e-6)
        {
            fail (where + ": settled at " + two (env.getLevel()) + " rather than its sustain");
            return;
        }

        // The release finishes, from somewhere inaudible, within its dial.
        env.noteOff();
        const long releaseLimit = (long) (release * rate) + 100;
        float last = 0.0f;
        long took = -1;

        for (long i = 1; i <= releaseLimit; ++i)
        {
            const float s = env.nextSample();

            if (! env.isActive())
            {
                took = i;
                break;
            }

            last = s;
        }

        if (took < 0)
        {
            fail (where + ": the release did not finish inside its own dial");
            return;
        }

        if ((double) last > loudestAllowedLastSample (release, rate))
        {
            fail (where + ": the last audible sample was " + two (last) + ", above the "
                  + two (loudestAllowedLastSample (release, rate)) + " one step above the floor");
            return;
        }

        worstRelease = std::fmax (worstRelease, (double) took / (release * rate));
        ++checked;
    }

    std::printf ("    %d settings checked, longest release %.4f of its dial\n",
                 checked, worstRelease);
}

//==============================================================================
int main()
{
    std::printf ("envelope\n");

    reachesItsTargetsExactly();
    releaseFinishesAtExactlyZero();
    staysInRangeAndFinite();
    zeroTimesDoNotHang();
    retriggerDoesNotClick();

    attackTakesTheTimeItsDialSays();
    decayTakesTheTimeItsDialSays();
    releaseTakesTheTimeItsDialSays();

    theAttackIsStraight();
    theDecayAndReleaseAreExponential();
    theLastAudibleSampleIsSixtyDecibelsDown();

    theDecayStageEndsRatherThanApproaching();
    theDecayNeverUndershootsTheSustain();
    theDecayStageActuallyRuns();

    settingsAreClampedToWhatTheyMean();
    anUnusableSampleRateFallsBack();
    anEndlessAttackStillArrives();
    resetLeavesNothingBehind();
    noteOffOnAnIdleEnvelopeChangesNothing();
    releasingIsReportedForTheReleaseAlone();
    itsOwnDefaultsAreTheOnesItUses();

    randomisedSettingsAllBehave();

    if (failures > 0)
    {
        std::printf ("\n%d envelope check(s) failed\n", failures);
        return 1;
    }

    std::printf ("all envelope checks passed\n");
    return 0;
}
