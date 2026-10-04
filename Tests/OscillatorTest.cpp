// ---------------------------------------------------------------------------
// Checks that the oscillators are actually band limited.
//
// This is the test that justifies PolyBLEP existing at all. A naive saw and a
// corrected saw look nearly identical on a scope and sound obviously
// different in the top two octaves, so eyeballing a waveform proves nothing.
// What separates them is where the energy lands: a correct oscillator puts it
// on harmonics of the note, and a naive one sprays a share of it onto
// frequencies that are not harmonically related and that move the wrong way
// when you play up the keyboard.
//
// So the check measures that split directly. The test tone is placed exactly
// on a DFT bin, which makes every harmonic and every reflected alias land
// exactly on a bin too, so the two can be separated with no windowing and no
// leakage to argue about. Then it compares the naive waveform against the
// corrected one and requires the corrected one to be a long way better.
//
// Measured at 2002.6 Hz, 44100 Hz sample rate, 2048 point DFT:
//
//   saw     naive -12.5 dB alias to harmonic, corrected -29.5 dB, 17 dB better
//   square  naive -14.6 dB alias to harmonic, corrected -33.0 dB, 18 dB better
//
// The thresholds below sit well under those so that this fails on a real
// regression rather than on arithmetic noise.
//
//   ./Tests/run.sh
//
// Sources for the polynomial, which agree with each other verbatim:
//   https://pbat.ch/sndkit/blep/
//   https://www.metafunction.co.uk/post/all-about-digital-oscillators-part-2-blits-bleps
// ---------------------------------------------------------------------------

#include "Oscillator.h"

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

static constexpr double pi = 3.14159265358979323846;

// --- measuring -------------------------------------------------------------

/** Plain DFT. Slow, and small enough here not to matter: being obviously
    correct is worth more in a test than being fast. */
static std::vector<double> magnitudes (const std::vector<double>& x)
{
    const size_t n = x.size();
    std::vector<double> out (n / 2, 0.0);

    for (size_t k = 0; k < n / 2; ++k)
    {
        std::complex<double> sum (0.0, 0.0);

        for (size_t i = 0; i < n; ++i)
            sum += x[i] * std::exp (std::complex<double> (0.0, -2.0 * pi * (double) k * (double) i / (double) n));

        out[k] = std::abs (sum);
    }

    return out;
}

/** Energy that landed anywhere other than on a harmonic of the note, as a
    ratio to the energy that landed on one, in decibels. Lower is better.

    The tone sits exactly on bin `binsPerCycle`, so its harmonics are exact
    multiples of that bin and anything folded back by the sample rate lands on
    an exact bin too. Everything that is not one of the harmonics genuinely
    below Nyquist is therefore alias, with nothing smeared in between. */
static double aliasToHarmonicDb (const std::vector<double>& wave, int binsPerCycle)
{
    const auto spectrum = magnitudes (wave);
    const int half = (int) spectrum.size();
    const int realHarmonics = (half - 1) / binsPerCycle;

    double harmonic = 0.0, alias = 0.0;

    for (int bin = 1; bin < half; ++bin)
    {
        const double energy = spectrum[(size_t) bin] * spectrum[(size_t) bin];
        const bool isHarmonic = (bin % binsPerCycle) == 0 && (bin / binsPerCycle) <= realHarmonics;

        if (isHarmonic) harmonic += energy;
        else            alias    += energy;
    }

    if (harmonic <= 0.0)
        return 0.0;

    return 10.0 * std::log10 (std::max (alias, 1.0e-30) / harmonic);
}

static std::vector<double> fromOscillator (Oscillator::Shape shape, double frequency,
                                           double sampleRate, size_t n)
{
    Oscillator osc;
    osc.setSampleRate (sampleRate);
    osc.setShape (shape);
    osc.setFrequency (frequency);
    osc.reset();

    std::vector<double> out (n);
    for (auto& s : out)
        s = osc.nextSample();

    return out;
}

/** The same shape with no correction at all, for the comparison. */
static std::vector<double> naive (bool square, double frequency, double sampleRate, size_t n)
{
    const double dt = frequency / sampleRate;
    double phase = 0.0;
    std::vector<double> out (n);

    for (auto& s : out)
    {
        s = square ? (phase < 0.5 ? 1.0 : -1.0) : (2.0 * phase - 1.0);
        phase += dt;
        phase -= std::floor (phase);
    }

    return out;
}

// --- the checks ------------------------------------------------------------

static void bandLimitingBeatsTheNaiveWaveform()
{
    std::printf ("the correction puts energy on harmonics instead of between them\n");

    const size_t n = 2048;
    const double sampleRate = 44100.0;
    const int binsPerCycle = 93;                                  // a high note, where this matters
    const double frequency = sampleRate * binsPerCycle / (double) n;

    struct Case { const char* name; Oscillator::Shape shape; bool square; };
    const Case cases[] =
    {
        { "saw",    Oscillator::Shape::saw,    false },
        { "square", Oscillator::Shape::square, true  }
    };

    for (const auto& c : cases)
    {
        const double rough     = aliasToHarmonicDb (naive (c.square, frequency, sampleRate, n), binsPerCycle);
        const double corrected = aliasToHarmonicDb (fromOscillator (c.shape, frequency, sampleRate, n), binsPerCycle);
        const double gain      = rough - corrected;

        std::printf ("    %-7s at %.0f Hz:  naive %+6.1f dB   corrected %+6.1f dB   %.1f dB better\n",
                     c.name, frequency, rough, corrected, gain);

        if (gain < 12.0)
            fail (std::string (c.name) + ": the correction only bought " + std::to_string (gain)
                  + " dB over the naive waveform");

        if (corrected > -25.0)
            fail (std::string (c.name) + ": alias energy is " + std::to_string (corrected)
                  + " dB below the harmonics, which is not band limited");
    }
}

static void staysInRangeAndFinite()
{
    std::printf ("nothing goes out of range or turns into a NaN, at any rate\n");

    double worst = 0.0;

    for (double sampleRate : { 22050.0, 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
    {
        for (auto shape : { Oscillator::Shape::sine, Oscillator::Shape::saw,
                            Oscillator::Shape::square, Oscillator::Shape::triangle })
        {
            // Up to and past Nyquist on purpose: the frequency setter is
            // supposed to clamp, and if it ever stops doing so the PolyBLEP
            // windows overlap and the output goes wild rather than quiet.
            for (double f = 20.0; f < sampleRate; f *= 1.7)
            {
                Oscillator osc;
                osc.setSampleRate (sampleRate);
                osc.setShape (shape);
                osc.setFrequency (f);
                osc.reset();

                for (int i = 0; i < 2000; ++i)
                {
                    const float s = osc.nextSample();

                    if (! std::isfinite (s))
                    {
                        fail ("not finite at " + std::to_string ((int) sampleRate) + " Hz, "
                              + std::to_string ((int) f) + " Hz");
                        return;
                    }

                    worst = std::max (worst, (double) std::fabs (s));
                }
            }
        }
    }

    // Measured at 1.0000 across the sweep above. The correction can overshoot
    // in principle, so this allows a little, but not the kind of excursion
    // that would clip a mix.
    std::printf ("    worst excursion %.4f\n", worst);
    check (worst <= 1.05, "an oscillator reached " + std::to_string (worst));
}

static void frequencyIsWhatWasAskedFor()
{
    std::printf ("a note comes out at the frequency it was given\n");

    for (double sampleRate : { 44100.0, 48000.0, 96000.0 })
    {
        for (double f : { 27.5, 110.0, 440.0, 1760.0, 5000.0 })
        {
            Oscillator osc;
            osc.setSampleRate (sampleRate);
            osc.setShape (Oscillator::Shape::sine);
            osc.setFrequency (f);
            osc.reset();

            const int n = (int) sampleRate;        // one second
            int crossings = 0;
            float previous = osc.nextSample();

            for (int i = 1; i < n; ++i)
            {
                const float s = osc.nextSample();
                if (previous <= 0.0f && s > 0.0f)
                    ++crossings;
                previous = s;
            }

            // One positive going crossing per cycle, give or take the one at
            // each end of the window.
            if (std::fabs ((double) crossings - f) > 1.5)
                fail ("asked for " + std::to_string ((int) f) + " Hz at "
                      + std::to_string ((int) sampleRate) + " Hz, counted "
                      + std::to_string (crossings) + " cycles in a second");
        }
    }
}

static void aPulseIsCentredAtEveryWidth()
{
    std::printf ("a pulse carries no DC however narrow it is\n");

    // Without the correction a pulse of duty w sits at 2w - 1, which is most
    // of full scale at the extremes. It is inaudible alone and is not once it
    // has been through a resonant filter and summed with other voices.
    for (double width : { 0.05, 0.1, 0.25, 0.5, 0.75, 0.9, 0.95 })
    {
        Oscillator osc;
        osc.setSampleRate (48000.0);
        osc.setShape (Oscillator::Shape::square);
        osc.setPulseWidth (width);
        osc.setFrequency (440.0);
        osc.reset();

        double sum = 0.0;
        const int n = 48000;

        for (int i = 0; i < n; ++i)
            sum += osc.nextSample();

        const double dc = sum / n;

        if (std::fabs (dc) > 0.02)
            fail ("pulse width " + std::to_string (width) + " sits at "
                  + std::to_string (dc) + " rather than at zero");
    }
}

static void resetAndRetuneBehave()
{
    std::printf ("reset starts where it is told, and retuning does not jump\n");

    Oscillator osc;
    osc.setSampleRate (48000.0);
    osc.setShape (Oscillator::Shape::saw);
    osc.setFrequency (440.0);

    osc.reset (0.0);
    const float first = osc.nextSample();

    osc.reset (0.0);
    check (osc.nextSample() == first, "the same reset did not give the same sample");

    // A note bend changes the increment every block. The waveform has one
    // genuine discontinuity per cycle and retuning must not add others.
    osc.reset (0.0);
    double previous = osc.nextSample();
    int jumps = 0;

    for (int i = 0; i < 20000; ++i)
    {
        osc.setFrequency (220.0 + 0.02 * i);
        const double s = osc.nextSample();

        // A saw falls by about two at its wrap and creeps up otherwise, so
        // anything else that moves by more than half is a glitch.
        if (std::fabs (s - previous) > 0.5 && s > previous)
            ++jumps;

        previous = s;
    }

    check (jumps == 0, std::to_string (jumps) + " upward jumps while the pitch was being swept");
}

int main()
{
    std::printf ("oscillator\n");

    bandLimitingBeatsTheNaiveWaveform();
    staysInRangeAndFinite();
    frequencyIsWhatWasAskedFor();
    aPulseIsCentredAtEveryWidth();
    resetAndRetuneBehave();

    if (failures > 0)
    {
        std::printf ("\n%d oscillator check(s) failed\n", failures);
        return 1;
    }

    std::printf ("all oscillator checks passed\n");
    return 0;
}
