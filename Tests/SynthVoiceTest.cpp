// ---------------------------------------------------------------------------
// Runs the whole signal path of one synth voice and measures it.
//
// The pieces underneath this each have their own test. What this covers is
// what happens when they are wired together and played, which is where a
// synth misbehaves in ways that are hard to pin down from a bug report:
//
//   a voice that never reports itself finished    the synth goes quiet after
//                                                 a few bars of playing
//   a voice that is louder than it should be      a patch that clips the mix
//                                                 only on certain chords
//   the filter envelope driven out of range       a sweep that cracks at the
//                                                 top of its travel
//
// The last one is specific to wiring rather than to any one part. The filter
// envelope here opens the cutoff by octaves, so a generous envelope amount on
// a high note asks for a cutoff well past Nyquist. The filter clamps, and
// this checks that it does, because the version of that bug which does not
// clamp is a tangent running off to infinity and a voice full of NaN.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include "SynthVoice.h"

#include <cmath>
#include <complex>
#include <cstdio>
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

static void makesSoundAndThenStops()
{
    std::printf ("a note sounds, releases, and the voice frees itself\n");

    SynthVoice voice;
    voice.setSampleRate (48000.0);
    voice.setPatch ({});
    voice.start (60, 1.0f);

    check (voice.isActive(), "the voice was not active after being started");

    double loudest = 0.0;
    for (int i = 0; i < 24000; ++i)
        loudest = std::max (loudest, (double) std::fabs (voice.nextSample()));

    check (loudest > 0.05, "the voice barely made a sound, peak was " + std::to_string (loudest));

    voice.stop();

    // Far longer than the release, so a voice that does not finish is a
    // voice that never would.
    int took = -1;
    for (int i = 0; i < 480000; ++i)
    {
        voice.nextSample();

        if (! voice.isActive())
        {
            took = i + 1;
            break;
        }
    }

    check (took > 0, "the voice never reported itself finished");

    if (took > 0)
    {
        std::printf ("    released in %.3f s\n", took / 48000.0);

        // And it is silent afterwards rather than nearly silent.
        for (int i = 0; i < 1000; ++i)
            check (voice.nextSample() == 0.0f, "the voice was still making sound after finishing");
    }
}

static void staysFiniteAndInRangeEverywhere()
{
    std::printf ("every note at every rate stays finite and within range\n");

    double loudest = 0.0;

    for (double rate : { 22050.0, 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        for (int note = 12; note <= 120; note += 9)
        {
            SynthVoice voice;
            voice.setSampleRate (rate);

            // A deliberately extreme patch: everything at full, a lot of
            // resonance, and an envelope that asks the filter to open far
            // past Nyquist on the high notes.
            SynthVoice::Patch patch;
            patch.osc1Level = 1.0;
            patch.osc2Level = 1.0;
            patch.subLevel  = 1.0;
            patch.resonance = 18.0;
            patch.envOctaves = 10.0;
            patch.cutoff = 4000.0;
            voice.setPatch (patch);

            voice.start (note, 1.0f);

            for (int i = 0; i < 4000; ++i)
            {
                const float s = voice.nextSample();

                if (! std::isfinite (s))
                {
                    fail ("not finite at " + std::to_string ((int) rate) + " Hz, note "
                          + std::to_string (note));
                    return;
                }

                loudest = std::max (loudest, (double) std::fabs (s));
            }

            voice.stop();

            for (int i = 0; i < 4000; ++i)
            {
                const float s = voice.nextSample();

                if (! std::isfinite (s))
                {
                    fail ("not finite during release at note " + std::to_string (note));
                    return;
                }

                loudest = std::max (loudest, (double) std::fabs (s));
            }
        }
    }

    // Three oscillators at full into a resonant filter can legitimately be
    // well above one. What matters is that it is a number a mixer can deal
    // with rather than a filter running away.
    std::printf ("    loudest sample across the sweep %.2f\n", loudest);
    check (loudest < 60.0, "a voice reached " + std::to_string (loudest));
}

static void stealingAVoiceDoesNotClickOrBlowUp()
{
    std::printf ("taking a voice mid release crosses over instead of cutting\n");

    SynthVoice voice;
    voice.setSampleRate (48000.0);
    voice.setPatch ({});

    voice.start (48, 1.0f);
    for (int i = 0; i < 12000; ++i)
        voice.nextSample();

    voice.stop();
    for (int i = 0; i < 2000; ++i)
        voice.nextSample();

    const double before = std::fabs (voice.nextSample());

    // Take it for a different note, which is what the allocator does when
    // every voice is busy.
    voice.start (72, 1.0f);
    const double after = std::fabs (voice.nextSample());

    // The envelope picks up from where it was rather than restarting at
    // zero, so the level should not collapse. A drop to nothing here is the
    // click that stealing is supposed to avoid.
    check (after > before * 0.3,
           "the level fell from " + std::to_string (before) + " to " + std::to_string (after)
           + " when the voice was taken");

    check (voice.getNote() == 72, "the voice did not take the new note");

    // And repeated stealing, which is the case that finds stale state.
    double loudest = 0.0;
    unsigned rng = 77771u;

    for (int steal = 0; steal < 400; ++steal)
    {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        voice.start (24 + (int) (rng % 90), 0.4f + 0.6f * (float) ((rng >> 8) % 100) / 100.0f);

        for (int i = 0; i < 200; ++i)
        {
            const float s = voice.nextSample();

            if (! std::isfinite (s))
            {
                fail ("not finite after " + std::to_string (steal) + " steals");
                return;
            }

            loudest = std::max (loudest, (double) std::fabs (s));
        }
    }

    std::printf ("    four hundred steals, loudest %.2f\n", loudest);
    check (loudest < 20.0, "repeated stealing built up to " + std::to_string (loudest));
}

/** The share of the signal's energy that sits above a given frequency.

    The first version of this used how far the waveform moves from one sample
    to the next, which seemed like a reasonable stand in for brightness and
    is not: that figure is dominated by the fundamental, so adding harmonics
    raises the movement and the size together and the ratio barely shifts. It
    moved by seven percent across a filter sweep of five octaves, which is
    not enough to tell anything from. Measuring the energy directly is more
    code and actually answers the question. */
static double highBandFraction (const std::vector<float>& block, double sampleRate, double splitHz)
{
    const size_t n = block.size();
    const double splitBin = splitHz * (double) n / sampleRate;

    double total = 0.0, high = 0.0;

    for (size_t k = 1; k < n / 2; ++k)
    {
        std::complex<double> sum (0.0, 0.0);

        for (size_t i = 0; i < n; ++i)
            sum += (double) block[i] * std::exp (std::complex<double> (0.0, -2.0 * 3.14159265358979323846 * (double) k * (double) i / (double) n));

        const double energy = std::norm (sum);
        total += energy;

        if ((double) k > splitBin)
            high += energy;
    }

    return total > 0.0 ? high / total : 0.0;
}

static void theFilterEnvelopeActuallyOpensTheFilter()
{
    std::printf ("the filter envelope moves the cutoff, and the note gets darker as it decays\n");

    // Without this nothing notices a filter envelope that is wired up but
    // never connected to the cutoff. The synth still plays, still has the
    // right pitch and the right amplitude shape, and sounds static and
    // lifeless in a way no other check here would object to.
    SynthVoice voice;
    voice.setSampleRate (48000.0);

    SynthVoice::Patch patch;
    patch.cutoff = 300.0;
    patch.envOctaves = 5.0;                        // a wide sweep, easy to measure
    patch.filter = { 0.001, 0.300, 0.0, 0.100 };   // snaps open, decays shut
    patch.amp    = { 0.001, 2.000, 1.0, 0.100 };   // holds level while it does
    voice.setPatch (patch);
    voice.start (45, 1.0f);

    std::vector<float> attack (2048), later (2048);

    for (auto& s : attack)
        s = voice.nextSample();

    // Past the filter decay, so the cutoff is back down at its resting point.
    for (int i = 0; i < 12000; ++i)
        voice.nextSample();

    for (auto& s : later)
        s = voice.nextSample();

    const double open = highBandFraction (attack, 48000.0, 1000.0);
    const double shut = highBandFraction (later,  48000.0, 1000.0);

    std::printf ("    energy above 1 kHz: %.1f%% with the envelope open, %.1f%% once it has decayed\n",
                 100.0 * open, 100.0 * shut);

    check (open > shut * 3.0,
           "the note did not get darker as the filter envelope decayed: "
           + std::to_string (open) + " then " + std::to_string (shut));
}

static void killSilencesItAtOnce()
{
    std::printf ("kill stops a voice dead and frees it\n");

    SynthVoice voice;
    voice.setSampleRate (48000.0);
    voice.setPatch ({});
    voice.start (60, 1.0f);

    for (int i = 0; i < 5000; ++i)
        voice.nextSample();

    voice.kill();

    check (! voice.isActive(), "the voice was still active after kill");

    for (int i = 0; i < 1000; ++i)
        check (voice.nextSample() == 0.0f, "the voice still made sound after kill");
}

static void velocityChangesTheLevel()
{
    std::printf ("playing harder is louder\n");

    auto peakAt = [] (float velocity)
    {
        SynthVoice voice;
        voice.setSampleRate (48000.0);
        voice.setPatch ({});
        voice.start (60, velocity);

        double peak = 0.0;
        for (int i = 0; i < 12000; ++i)
            peak = std::max (peak, (double) std::fabs (voice.nextSample()));

        return peak;
    };

    const double soft = peakAt (0.25f);
    const double hard = peakAt (1.0f);

    check (hard > soft * 2.0,
           "soft was " + std::to_string (soft) + " and hard was " + std::to_string (hard));

    const double silent = peakAt (0.0f);
    check (silent == 0.0, "a velocity of nothing still made " + std::to_string (silent));
}

int main()
{
    std::printf ("synth voice\n");

    makesSoundAndThenStops();
    staysFiniteAndInRangeEverywhere();
    stealingAVoiceDoesNotClickOrBlowUp();
    theFilterEnvelopeActuallyOpensTheFilter();
    killSilencesItAtOnce();
    velocityChangesTheLevel();

    if (failures > 0)
    {
        std::printf ("\n%d synth voice check(s) failed\n", failures);
        return 1;
    }

    std::printf ("all synth voice checks passed\n");
    return 0;
}
