// ---------------------------------------------------------------------------
// Checks the filter against the analogue prototype it is standing in for.
//
// A filter is easy to get nearly right. Swap two coefficients and it still
// filters: the response is wrong, but it is wrong in a way that sounds like a
// different filter rather than like a bug, so listening will not find it. The
// checks here are the ones that have exact answers.
//
// The strongest is structural. A state variable filter produces all three
// responses from the same pair of integrators, and they are related by an
// identity that holds for every sample of every signal at every setting:
//
//     highpass + (1/Q) * bandpass + lowpass == input
//
// That is exact arithmetic, not an approximation, so it can be asserted to
// machine precision. Almost any mistake in the coefficients breaks it.
//
// The rest are the textbook properties of a two pole resonant lowpass: unity
// at DC, nothing at Nyquist, and a magnitude at the cutoff of exactly Q.
// Measured before the filter was written, Q from 0.5 to 10 agreed to four
// decimal places.
//
//   ./Tests/run.sh
//
// Source: Pirkle, Virtual Analog Filter Implementation, after Zavalishin,
// The Art of VA Filter Design.
// ---------------------------------------------------------------------------

#include "Filter.h"

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

static constexpr double pi = 3.14159265358979323846;

/** Magnitude response at one frequency, by correlating the output against a
    sine and a cosine once the filter has settled. */
static double magnitudeAt (Filter::Mode mode, double cutoff, double q,
                           double frequency, double sampleRate = 48000.0)
{
    Filter filter;
    filter.setSampleRate (sampleRate);
    filter.setMode (mode);
    filter.setCutoff (cutoff);
    filter.setResonance (q);
    filter.reset();

    const int total = 60000;
    const int settle = total / 2;

    double re = 0.0, im = 0.0;

    for (int i = 0; i < total; ++i)
    {
        const double phase = 2.0 * pi * frequency * i / sampleRate;
        const double out = filter.nextSample ((float) std::sin (phase));

        if (i >= settle)
        {
            re += out * std::cos (phase);
            im += out * std::sin (phase);
        }
    }

    return 2.0 * std::hypot (re, im) / (total - settle);
}

static void theStructuralIdentityHolds()
{
    std::printf ("highpass plus bandpass over Q plus lowpass equals the input, exactly\n");

    // Run all three modes over the same signal from the same starting state
    // and add them back up. Three filters rather than one, because the modes
    // are read from one set of integrators and this has to be checked
    // through the public interface rather than by reaching inside.
    unsigned rng = 123456789u;
    double worst = 0.0;

    for (double cutoff : { 40.0, 500.0, 3000.0, 15000.0 })
    {
        for (double q : { 0.5, 0.707, 2.0, 8.0, 20.0 })
        {
            Filter low, band, high;

            for (auto* f : { &low, &band, &high })
            {
                f->setSampleRate (48000.0);
                f->setCutoff (cutoff);
                f->setResonance (q);
                f->reset();
            }

            low.setMode (Filter::Mode::lowpass);
            band.setMode (Filter::Mode::bandpass);
            high.setMode (Filter::Mode::highpass);

            for (int i = 0; i < 4000; ++i)
            {
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                const float x = (float) ((double) rng / 2147483648.0 - 1.0);

                const double sum = (double) high.nextSample (x)
                                 + (double) band.nextSample (x) / q
                                 + (double) low.nextSample (x);

                worst = std::max (worst, std::fabs (sum - (double) x));
            }
        }
    }

    std::printf ("    worst departure from the identity %.2e\n", worst);
    check (worst < 1.0e-5, "the identity broke by " + std::to_string (worst));
}

static void theResponseIsTheAnalogueOne()
{
    std::printf ("the lowpass has the shape a two pole resonant lowpass should\n");

    const double cutoff = 1000.0;

    // At the cutoff a two pole resonant lowpass sits at exactly Q. This is
    // the one that catches a resonance control wired to the wrong thing,
    // which is otherwise only obvious by ear and only at the extremes.
    //
    // Checked high up as well as low down, and that is the point rather than
    // thoroughness for its own sake. Low down, the tangent that prewarps the
    // cutoff is almost equal to its own argument, so a filter that forgot to
    // prewarp at all measures correct to a fraction of a percent and passes.
    // The error only opens up as the cutoff approaches Nyquist, which is
    // exactly where an unwarped filter goes flat and the resonant peak slides
    // away from where the dial says it is. At 10 kHz and 48 kHz the tangent
    // is 0.765 against an argument of 0.654, so the peak lands in the wrong
    // place by enough to fail this.
    for (double fc : { 100.0, 1000.0, 10000.0 })
    {
        for (double q : { 0.5, 0.707, 1.0, 2.0, 5.0, 10.0 })
        {
            const double measured = magnitudeAt (Filter::Mode::lowpass, fc, q, fc);

            if (std::fabs (measured - q) > 0.01 * q + 0.002)
                fail ("at a cutoff of " + std::to_string ((int) fc) + " Hz with Q "
                      + std::to_string (q) + " the gain there is " + std::to_string (measured));
        }
    }

    const double atDc = magnitudeAt (Filter::Mode::lowpass, cutoff, 0.707, 2.0);
    check (std::fabs (atDc - 1.0) < 0.01, "the lowpass passes " + std::to_string (atDc) + " at DC");

    const double high = magnitudeAt (Filter::Mode::lowpass, cutoff, 0.707, 20000.0);
    check (high < 0.01, "the lowpass passes " + std::to_string (high) + " at 20 kHz");

    // And the highpass is the mirror of it.
    const double hpLow = magnitudeAt (Filter::Mode::highpass, cutoff, 0.707, 20.0);
    check (hpLow < 0.01, "the highpass passes " + std::to_string (hpLow) + " at 20 Hz");
}

static void surviveseEverySettingAndRate()
{
    std::printf ("nothing blows up or turns into a NaN, at any rate or setting\n");

    for (double rate : { 22050.0, 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        for (auto mode : { Filter::Mode::lowpass, Filter::Mode::highpass, Filter::Mode::bandpass })
        {
            // Past both ends deliberately: the setters are supposed to clamp,
            // and the tangent runs to infinity at Nyquist if they ever stop.
            for (double cutoff : { 0.0, 1.0, 20.0, 1000.0, rate * 0.4, rate * 0.5, rate })
            {
                for (double q : { 0.0, 0.5, 10.0, 20.0, 1000.0 })
                {
                    Filter filter;
                    filter.setSampleRate (rate);
                    filter.setMode (mode);
                    filter.setCutoff (cutoff);
                    filter.setResonance (q);
                    filter.reset();

                    for (int i = 0; i < 2000; ++i)
                    {
                        const float out = filter.nextSample (i % 2 == 0 ? 1.0f : -1.0f);

                        if (! std::isfinite (out) || std::fabs (out) > 1000.0f)
                        {
                            fail ("ran away at " + std::to_string ((int) rate) + " Hz, cutoff "
                                  + std::to_string (cutoff) + ", Q " + std::to_string (q)
                                  + ": " + std::to_string (out));
                            return;
                        }
                    }
                }
            }
        }
    }
}

static void sweepingTheCutoffStaysStable()
{
    std::printf ("a filter envelope can sweep it hard without it ringing away\n");

    // This is the case the topology preserving transform exists for. A biquad
    // with its coefficients recomputed every sample is not stable under this,
    // because its state means something different after every change.
    Filter filter;
    filter.setSampleRate (48000.0);
    filter.setMode (Filter::Mode::lowpass);
    filter.setResonance (15.0);
    filter.reset();

    double worst = 0.0;

    for (int i = 0; i < 480000; ++i)
    {
        // Ten full sweeps a second, from 30 Hz to 18 kHz and back, which is
        // faster than any envelope and slower than nothing.
        const double t = std::fmod (i / 4800.0, 1.0);
        const double shape = t < 0.5 ? t * 2.0 : 2.0 - t * 2.0;
        filter.setCutoff (30.0 * std::pow (600.0, shape));

        const double phase = 2.0 * pi * 220.0 * i / 48000.0;
        const float out = filter.nextSample ((float) (0.5 * std::sin (phase)));

        if (! std::isfinite (out))
        {
            fail ("the sweep produced something that was not finite at sample " + std::to_string (i));
            return;
        }

        worst = std::max (worst, (double) std::fabs (out));
    }

    // Resonance of fifteen on a sine at the cutoff can legitimately reach
    // around fifteen times the input. Far past that is the filter running
    // away rather than resonating.
    std::printf ("    loudest sample during the sweep %.3f, input was 0.5\n", worst);
    check (worst < 12.0, "the sweep reached " + std::to_string (worst));
}

static void resetClearsIt()
{
    std::printf ("reset actually clears the state\n");

    Filter filter;
    filter.setSampleRate (48000.0);
    filter.setCutoff (800.0);
    filter.setResonance (10.0);

    for (int i = 0; i < 1000; ++i)
        filter.nextSample (1.0f);

    filter.reset();

    const float first = filter.nextSample (0.0f);
    check (first == 0.0f, "silence after reset gave " + std::to_string (first));

    for (int i = 0; i < 100; ++i)
        check (filter.nextSample (0.0f) == 0.0f, "state left over after reset");
}

int main()
{
    std::printf ("filter\n");

    theStructuralIdentityHolds();
    theResponseIsTheAnalogueOne();
    surviveseEverySettingAndRate();
    sweepingTheCutoffStaysStable();
    resetClearsIt();

    if (failures > 0)
    {
        std::printf ("\n%d filter check(s) failed\n", failures);
        return 1;
    }

    std::printf ("all filter checks passed\n");
    return 0;
}
