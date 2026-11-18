// ---------------------------------------------------------------------------
// The measurement that chose the slew limiter in GainRamp.h over a one pole,
// kept so the choice can be rechecked rather than argued from memory.
//
// Not run by Tests/run.sh: a *Probe.cpp is a measurement that justified a
// design decision, not a check that something still works.
//
// It exists because the first version of that decision carried a number that
// was wrong. The header and the test said a one pole "would take about 30 dB
// off this reading rather than 11.7", which was an estimate nothing computed,
// and an audit measured it at 42.6. That is the shape of fault CLAUDE.md is
// about, applied to the argument instead of to the signal: a figure that is
// present, plausible, wrong, and in the direction that flatters the decision
// already made. So the figure now comes from here.
//
// What is measured: a fader dragged from unity to silence over a third of a
// second, written by the interface sixty times a second, which is what
// actually reaches the mixer when somebody rides a fader. Four ways of
// applying it:
//
//   ideal     the gain moving continuously, which is what was asked for
//   stepped   one constant per block, which is what the mixer did before
//   slew      the slew limiter in GainRamp.h, which is what it does now
//   one pole  Ardour's, a = 156.825 / fs, a 6.3765 ms time constant
//             (libs/ardour/amp.cc:195-213)
//
// And two numbers for each: the largest change the gain makes in one sample,
// which is what a click is, and how much of the error against a continuous
// drag lands above 2 kHz, which is where a click is heard and a fader move is
// not.
//
//   g++ -std=c++20 -O2 -I Tests/juce-stub -I Source -o probe Tests/GainRampProbe.cpp
// ---------------------------------------------------------------------------

#include "GainRamp.h"
#include "RealFFT.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

static const double rate      = 48000.0;
static const int    blockSize = 512;
static const int    total     = 32768;
static const int    uiPeriod  = 800;                       // 60 a second
static const int    dragSamples = (int) (rate * 0.3);

static double idealAt (int sample)
{
    return std::max (0.0, 1.0 - (double) sample / (double) dragSamples);
}

/** What the interface has written by the time this block starts. */
static double targetAt (int blockStart)
{
    return idealAt ((blockStart / uiPeriod) * uiPeriod);
}

int main()
{
    std::vector<double> ideal ((size_t) total), stepped ((size_t) total),
                        slew ((size_t) total), pole ((size_t) total);

    for (int i = 0; i < total; ++i)
        ideal[(size_t) i] = idealAt (i);

    const double step = GainRamp::stepPerSample (rate, GainRamp::levelSeconds);

    // Ardour's coefficient, which is the small w approximation to a 25 Hz
    // corner: a = 1 - exp(-2 pi f / fs) ~= 6.2 f / fs.
    const double a = 156.825 / rate;

    GainRamp::Ramp ramp { 1.0 };
    double lpf = 1.0;

    for (int at = 0; at < total; at += blockSize)
    {
        const int    n      = std::min (blockSize, total - at);
        const double target = targetAt (at);

        for (int i = 0; i < n; ++i)
        {
            stepped[(size_t) (at + i)] = target;
            slew   [(size_t) (at + i)] = ramp.next (target, step);

            // Multiply then update, which is Ardour's order (amp.cc:210-213),
            // so the first sample of a move still gets the old gain. The snap
            // is GAIN_COEFF_DELTA, 1e-5 of linear gain, which is what makes a
            // one pole terminate at all (amp.cc:219-221).
            pole[(size_t) (at + i)] = lpf;
            lpf += a * (target - lpf);

            if (std::abs (lpf - target) < 1.0e-5)
                lpf = target;
        }
    }

    RealFFT fft (total);
    std::vector<std::complex<double>> bins ((size_t) fft.getNumBins());
    const int firstHighBin = (int) (2000.0 * (double) total / rate);

    struct Case { const char* name; const std::vector<double>* gains; };
    const Case cases[] = { { "ideal", &ideal }, { "stepped", &stepped },
                           { "slew", &slew }, { "one pole", &pole } };

    double high[4] = { 0.0, 0.0, 0.0, 0.0 };
    double jump[4] = { 0.0, 0.0, 0.0, 0.0 };

    for (int c = 0; c < 4; ++c)
    {
        for (size_t i = 1; i < cases[c].gains->size(); ++i)
            jump[c] = std::max (jump[c], std::abs ((*cases[c].gains)[i] - (*cases[c].gains)[i - 1]));

        std::vector<float> error ((size_t) total);
        for (int i = 0; i < total; ++i)
            error[(size_t) i] = (float) ((*cases[c].gains)[(size_t) i] - ideal[(size_t) i]);

        fft.forward (error.data(), bins.data());

        double energy = 0.0;
        for (int k = firstHighBin; k < fft.getNumBins(); ++k)
            energy += std::norm (bins[(size_t) k]);

        const double rms = std::sqrt (2.0 * energy) / (double) total;
        high[c] = rms > 0.0 ? 20.0 * std::log10 (rms) : -1000.0;

        std::printf ("%-9s largest change in one sample %.6f    error above 2 kHz %8.2f dB\n",
                     cases[c].name, jump[c], high[c]);
    }

    std::printf ("\nagainst the per block constant the mixer used to apply:\n");
    std::printf ("  the slew limiter removes %5.2f dB of it, and %5.2f dB of the largest change\n",
                 high[1] - high[2], 20.0 * std::log10 (jump[1] / jump[2]));
    std::printf ("  a one pole would remove %5.2f dB of it, and %5.2f dB of the largest change\n",
                 high[1] - high[3], 20.0 * std::log10 (jump[1] / jump[3]));

    // Why the better one was not taken, in the two numbers that decide it.
    GainRamp::Ramp toSilence { 1.0 };
    int slewSamples = 0;
    while (! toSilence.settled (0.0) && slewSamples < (int) rate)
    {
        toSilence.next (0.0, step);
        ++slewSamples;
    }

    double g = 1.0;
    int poleSamples = 0, poleMinus40 = 0;
    while (g != 0.0 && poleSamples < (int) rate)
    {
        g += a * (0.0 - g);
        if (std::abs (g) < 1.0e-5)
            g = 0.0;
        ++poleSamples;

        if (poleMinus40 == 0 && g <= 0.01)
            poleMinus40 = poleSamples;
    }

    std::printf ("\nand what each costs a mute, which has to be silent rather than nearly so:\n");
    std::printf ("  the slew limiter reaches exact silence in %6.2f ms\n",
                 1000.0 * (double) slewSamples / rate);
    std::printf ("  a one pole reaches -40 dB in %6.2f ms and exact silence in %6.2f ms,\n"
                 "  and only because of the snap: left alone it never arrives\n",
                 1000.0 * (double) poleMinus40 / rate,
                 1000.0 * (double) poleSamples / rate);

    return 0;
}
