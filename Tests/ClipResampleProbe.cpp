// ---------------------------------------------------------------------------
// Where every figure ClipResampler.h quotes comes from.
//
// Not run by ./Tests/run.sh: it is a measurement that justified the design,
// kept so the design can be rechecked, and it takes a minute rather than a
// second. The convention is WaveTableProbe.cpp's and the reason is the same:
// two of the numbers in that header were arrived at by measurement after a
// published formula gave the wrong answer, and a choice whose justification
// cannot be re-run is a choice nobody can revisit.
//
//   g++ -std=c++20 -O2 -I Source -I Tests/juce-stub \
//       -o /tmp/probe Tests/ClipResampleProbe.cpp && /tmp/probe
//
// What it measures, and which claim in the header each table is behind:
//
//   1. The tilt the old two point read put on every trimmed clip, against the
//      closed form, so the table in the header is reproducible.
//   2. The tap count, against the two things it has to buy: a flat passband to
//      20 kHz on ordinary 44.1 kHz material, and rejection of the one source
//      tone that folds onto exactly 20 kHz. This is the table that says the
//      Kaiser length formula's answer fails the specification it was given.
//   3. The table density, which is the measurement that says 512 entries per
//      zero crossing is where a denser table stops buying anything, and why
//      storing the taps as float is enough.
//   4. The per phase DC gain, with and without the normalisation, which is the
//      measurement that says that normalisation is a nicety here rather than
//      the necessity it was in the oversampler.
//   5. What it costs, against what it replaced.
// ---------------------------------------------------------------------------

#include "ClipResampler.h"

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

static constexpr double pi = 3.14159265358979323846;

//==============================================================================
// Reading helpers. The oracle throughout is the analytic ideal, for the reason
// ClipResamplerTest.cpp gives: the read is linear phase with no group delay,
// so sin(2 pi f p / R) at the position a sample was read from is what a perfect
// read would have given, and an error is an error wherever it lands.

enum class How { bandlimited, linear };

struct Pass { double residualDb, levelDb; };

static Pass sweep (const ClipResampler::Kernel& k, How how, double tone, double sourceRate,
                   double step, double offset, int outputSamples = 12000)
{
    const int pad    = 8192;
    const int frames = (int) (outputSamples * step) + 2 * pad + 8;

    std::vector<float> source ((std::size_t) frames);

    for (int i = 0; i < frames; ++i)
        source[(std::size_t) i] = (float) std::sin (2.0 * pi * tone * i / sourceRate);

    double errorPower = 0.0, idealPower = 0.0, outputPower = 0.0;
    double position = (double) pad + offset;

    for (int i = 0; i < outputSamples; ++i)
    {
        double value;

        if (how == How::bandlimited)
        {
            value = ClipResampler::read (k, source.data(), source.data(), frames, position, step).left;
        }
        else
        {
            const int f0 = (int) position;
            value = source[(std::size_t) f0]
                  + (source[(std::size_t) (f0 + 1)] - source[(std::size_t) f0]) * (position - f0);
        }

        const double ideal = std::sin (2.0 * pi * tone * position / sourceRate);

        errorPower  += (value - ideal) * (value - ideal);
        idealPower  += ideal * ideal;
        outputPower += value * value;
        position    += step;
    }

    return { 10.0 * std::log10 (errorPower / idealPower + 1.0e-300),
             10.0 * std::log10 (outputPower / ((double) outputSamples * 0.5) + 1.0e-300) };
}

//==============================================================================
int main()
{
    // -----------------------------------------------------------------------
    std::printf ("1. WHAT THE OLD READ DID TO A TRIMMED CLIP\n\n");
    std::printf ("   At a step of exactly one with a constant fractional offset, two point\n");
    std::printf ("   linear interpolation is the one zero FIR (1-d) + d z^-1. It is linear and\n");
    std::printf ("   time invariant, so there is no distortion at all: the whole defect is this\n");
    std::printf ("   tilt, and its depth depends on where the clip happened to be trimmed.\n\n");
    std::printf ("   48 kHz session        d = 0.25    d = 0.5   closed form at d = 0.5\n");

    for (double f : { 1000.0, 5000.0, 10000.0, 15000.0, 18000.0, 20000.0 })
    {
        const auto mag = [f] (double d)
        {
            const double w = 2.0 * pi * f / 48000.0;
            const std::complex<double> h = (1.0 - d) + d * std::exp (std::complex<double> (0.0, -w));
            return 20.0 * std::log10 (std::abs (h));
        };

        std::printf ("   %7.0f Hz            %8.3f   %8.3f   %8.3f  (cos(pi f / fs))\n",
                     f, mag (0.25), mag (0.5),
                     20.0 * std::log10 (std::cos (pi * f / 48000.0)));
    }

    // -----------------------------------------------------------------------
    std::printf ("\n\n2. THE TAP COUNT\n\n");
    std::printf ("   (a) The passband, reading a 44.1 kHz file into a 48 kHz session. This is\n");
    std::printf ("       what ordinary material needs: flat to 20 kHz.\n\n");
    std::printf ("   taps    16 kHz      18 kHz      20 kHz      21 kHz\n");

    for (int crossings : { 16, 24, 32, 42, 48, 64 })
    {
        const ClipResampler::Kernel k (crossings, 512, 120.0);
        std::printf ("   %4d ", 2 * crossings);

        for (double f : { 16000.0, 18000.0, 20000.0, 21000.0 })
            std::printf ("  %+9.4f", sweep (k, How::bandlimited, f, 44100.0,
                                            44100.0 / 48000.0, 0.37, 6000).levelDb);

        std::printf ("\n");
    }

    std::printf ("\n   (b) The rejection of the one source tone that folds onto exactly 20 kHz.\n");
    std::printf ("       Say where an alias lands or the figure means nothing: these two cases\n");
    std::printf ("       put it at the top of the protected band, which is the worst place it\n");
    std::printf ("       can be, and the tone sits a few hundred hertz inside the designed\n");
    std::printf ("       stopband edge, which is where the empirical length formula is least\n");
    std::printf ("       accurate.\n\n");
    // The formula's own answer, worked from the requirement rather than
    // circularly from the length that was chosen. A 44.1 kHz session protected
    // to 20 kHz leaves 2.05 kHz between the top of the band and the cutoff,
    // and the transition is symmetric about the cutoff, so it can be no wider
    // than twice that as a fraction of the rate.
    const double needed = 2.0 * (0.5 * 44100.0 - 20000.0) / 44100.0;

    std::printf ("       A 44.1 kHz session protected to 20 kHz needs a transition no wider\n");
    std::printf ("       than 2*(22050-20000)/44100 = %.5f cycles per sample, and the Kaiser\n", needed);
    std::printf ("       length formula then asks for (120 - 7.95)/(14.36 * %.5f) = %.2f taps.\n\n",
                 needed, ClipResampler::kaiserTaps (120.0, needed));
    std::printf ("       Each figure below is the worst over every place an alias can land\n");
    std::printf ("       between 10 and 20 kHz, not one tone: a figure that moves across the\n");
    std::printf ("       specification when the test tone moves by three hertz is not a figure\n");
    std::printf ("       to choose a design by, and the first draft of this table did exactly\n");
    std::printf ("       that and reported a failure that was not there.\n\n");
    std::printf ("   taps    88.2k into 44.1k      44.1k clip at 1.2x     worst lands at\n");

    for (int crossings : { 32, 42, 48, 56 })
    {
        const ClipResampler::Kernel k (crossings, 512, 120.0);
        double worstA = -1.0e300, worstB = -1.0e300, whereA = 0.0, whereB = 0.0;

        for (double lands = 10000.0; lands <= 20000.5; lands += 100.0)
        {
            const double cycles = 1.0 - lands / 44100.0;
            const double toneA = cycles * 88200.0 / 2.0;
            const double toneB = cycles * 44100.0 / 1.2;

            if (toneA < 0.5 * 88200.0)
            {
                const double level = sweep (k, How::bandlimited, toneA, 88200.0, 2.0, 0.37).levelDb;
                if (level > worstA) { worstA = level; whereA = lands; }
            }

            if (toneB < 0.5 * 44100.0)
            {
                const double level = sweep (k, How::bandlimited, toneB, 44100.0, 1.2, 0.37).levelDb;
                if (level > worstB) { worstB = level; whereB = lands; }
            }
        }

        std::printf ("   %4d      %9.2f dB           %9.2f dB        %.0f / %.0f Hz%s\n",
                     2 * crossings, worstA, worstB, whereA, whereB,
                     2 * crossings == 84 ? "   <- the formula's answer" : "");
    }

    {
        const double toneA = (1.0 - 20000.0 / 44100.0) * 88200.0 / 2.0;
        std::printf ("\n   and the old read on the worst of those: %.2f dB\n",
                     sweep (ClipResampler::Kernel(), How::linear, toneA, 88200.0, 2.0, 0.37).levelDb);
        std::printf ("\n   84 taps meets the specification by 0.08 dB, which is to say exactly.\n");
        std::printf ("   96 was chosen for a decibel of margin against an empirical formula, and\n");
        std::printf ("   that is a judgement rather than a measurement. What the measurement does\n");
        std::printf ("   say is that going lower is a cliff and not a slope: 64 taps is 65 dB\n");
        std::printf ("   worse than 84, because the transition width goes as one over the length,\n");
        std::printf ("   so a shorter kernel's stopband has not begun by the time the alias\n");
        std::printf ("   arrives.\n");
    }

    // -----------------------------------------------------------------------
    std::printf ("\n   (c) Why the cutoff sits exactly at the lower Nyquist rather than below\n");
    std::printf ("       it. The loss at 20 kHz reading a 44.1 kHz file into a 48 kHz\n");
    std::printf ("       session. Backing the cutoff off buys transition width with passband,\n");
    std::printf ("       and passband is the thing being fixed.\n\n");
    std::printf ("   taps   cutoff 0.90   cutoff 0.95   at the Nyquist\n");

    for (int crossings : { 16, 32, 48 })
    {
        std::printf ("   %4d ", 2 * crossings);

        for (double cutoff : { 0.90, 0.95, 1.00 })
            std::printf ("  %+11.5f", sweep (ClipResampler::Kernel (crossings, 512, 120.0, cutoff),
                                             How::bandlimited, 20000.0, 44100.0,
                                             44100.0 / 48000.0, 0.37, 6000).levelDb);

        std::printf ("\n");
    }

    std::printf ("\n       The cutoff is a constructor parameter for this table and for nothing\n");
    std::printf ("       else: the shipped kernel is always at the Nyquist, which is Smith's\n");
    std::printf ("       rule exactly. It exists because an earlier draft of the header quoted\n");
    std::printf ("       three figures here from a measurement nothing in the repository could\n");
    std::printf ("       reproduce.\n");

    // -----------------------------------------------------------------------
    std::printf ("\n\n3. THE TABLE DENSITY\n\n");
    std::printf ("   Reading between two table entries is itself a linear interpolation, so the\n");
    std::printf ("   table has to be dense enough that it does not become the error. Smith's\n");
    std::printf ("   bound, which his own text says is pessimistic for a windowed design, puts\n");
    std::printf ("   512 entries per zero crossing at -116 dB. Measured against the ideal it is\n");
    std::printf ("   better than that, and then it STOPS IMPROVING: that is the floor of storing\n");
    std::printf ("   the taps as float arriving, which is also the answer to whether they should\n");
    std::printf ("   be stored as double.\n\n");
    std::printf ("      L    bound 0.412/L^2   measured residual (88.2k into 44.1k, 6 kHz)\n");

    for (int entries : { 64, 128, 256, 512, 1024, 2048 })
    {
        const ClipResampler::Kernel k (48, entries, 120.0);
        std::printf ("   %5d      %8.2f dB      %9.2f dB\n", entries,
                     20.0 * std::log10 (0.412 / ((double) entries * (double) entries)),
                     sweep (k, How::bandlimited, 6000.0, 88200.0, 2.0, 0.37).residualDb);
    }

    // -----------------------------------------------------------------------
    std::printf ("\n\n4. THE PER PHASE DC GAIN\n\n");
    std::printf ("   Each fractional position uses its own tap set, which is an independent FIR\n");
    std::printf ("   whose taps have to sum to one. The residual is a gain that varies with the\n");
    std::printf ("   fractional position, which is a modulation of a steady signal.\n\n");
    std::printf ("   The unnormalised figure below is built from the header's own besselI0 and\n");
    std::printf ("   kaiserBeta with the one normalising step left out, rather than from a\n");
    std::printf ("   separate implementation of the window, so what is being measured really is\n");
    std::printf ("   the worth of that step and nothing else.\n\n");

    {
        const ClipResampler::Kernel k;
        const int L = k.entriesPerCrossing, count = k.entryCount();

        std::vector<double> raw ((std::size_t) count);
        const double beta = ClipResampler::kaiserBeta (k.attenuationDb);
        const double normaliser = ClipResampler::besselI0 (beta);

        for (int j = 0; j < count; ++j)
        {
            const double fraction = (double) j / (double) (count - 1);
            const double inside = 1.0 - fraction * fraction;
            const double window = ClipResampler::besselI0 (beta * std::sqrt (inside < 0.0 ? 0.0 : inside))
                                / normaliser;
            double sinc;

            if (j % L == 0)
                sinc = j == 0 ? 1.0 : 0.0;
            else
            {
                const double x = pi * (double) j / (double) L;
                sinc = std::sin (x) / x;
            }

            raw[(std::size_t) j] = sinc * window;
        }

        double worstRaw = 0.0;

        for (int phase = 0; phase <= L; ++phase)
        {
            double sum = 0.0;

            for (int i = -k.crossings + 1; i <= k.crossings; ++i)
            {
                const int entry = std::abs (i * L - phase);

                if (entry < count)
                    sum += raw[(std::size_t) entry];
            }

            worstRaw = std::max (worstRaw, std::abs (sum - 1.0));
        }

        double worstDone = 0.0;

        for (int phase = 0; phase <= L; ++phase)
            worstDone = std::max (worstDone, std::abs (k.phaseSum (phase) - 1.0));

        std::printf ("   without the normalisation: %.3e  (%.1f dB)\n",
                     worstRaw, 20.0 * std::log10 (worstRaw + 1.0e-300));
        std::printf ("   with it:                   %.3e  (%.1f dB)\n",
                     worstDone, 20.0 * std::log10 (worstDone + 1.0e-300));
        std::printf ("\n   So against a 120 dB target the raw table was already adequate, and\n");
        std::printf ("   what is left after normalising is the float storage rather than the\n");
        std::printf ("   window. Both are quoted in the header, because the magnitude is what\n");
        std::printf ("   decides whether the step is a necessity or a nicety, and here it is a\n");
        std::printf ("   nicety that costs nothing at build time.\n");
    }

    // -----------------------------------------------------------------------
    std::printf ("\n\n5. WHAT A READ CAN PRODUCE FROM FULL SCALE MATERIAL\n\n");
    std::printf ("   The one place this change alters the signal rather than improving it.\n");
    std::printf ("   Linear interpolation was a convex combination of two samples and could\n");
    std::printf ("   never leave the input's range. A windowed sinc overshoots, which is the\n");
    std::printf ("   same fact TruePeak.h exists for: the waveform a converter reconstructs\n");
    std::printf ("   from a square wave really does go above the largest sample in the file.\n\n");

    {
        const ClipResampler::Kernel k;
        double worstGain = 0.0;

        for (int phase = 0; phase <= k.entriesPerCrossing; ++phase)
            worstGain = std::max (worstGain, k.phaseGain (phase));

        std::printf ("   worst case gain, the sum of one phase's tap magnitudes: %.4f (%+.2f dB)\n",
                     worstGain, 20.0 * std::log10 (worstGain));

        const int frames = 8000, centre = 4000;

        for (int period : { 960, 480, 109, 48 })
        {
            std::vector<float> square ((std::size_t) frames);

            for (int i = 0; i < frames; ++i)
                square[(std::size_t) i] = ((i / (period / 2)) % 2) == 0 ? 1.0f : -1.0f;

            double peak = 0.0;

            for (int i = 0; i < period * 100; ++i)
            {
                const double position = (double) centre + i * 0.01;
                peak = std::max (peak, std::abs ((double) ClipResampler::read (
                    k, square.data(), square.data(), frames, position, 1.0).left));
            }

            std::printf ("   a full scale square wave at %6.1f Hz reads %.4f (%+.2f dB)\n",
                         48000.0 / period, peak, 20.0 * std::log10 (peak));
        }

        std::printf ("\n   So a master limited to exactly full scale, imported at 44.1 kHz into a\n");
        std::printf ("   48 kHz session, reads above full scale on its insert, and a bounce to a\n");
        std::printf ("   fixed point word length can clip where it did not before.\n");
    }

    // -----------------------------------------------------------------------
    std::printf ("\n\n6. WHAT IT COSTS\n\n");
    std::printf ("   One stereo clip, per output sample, on whatever this is running on. The\n");
    std::printf ("   tap count grows with the step because the kernel stretches, which is\n");
    std::printf ("   inherent to the method: a clip played four times fast costs four times the\n");
    std::printf ("   arithmetic.\n\n");

    {
        const ClipResampler::Kernel k;
        const int frames = 1 << 20;

        std::vector<float> source ((std::size_t) frames);

        for (int i = 0; i < frames; ++i)
            source[(std::size_t) i] = (float) std::sin (0.013 * i);

        std::printf ("   step      taps    ns/sample   one clip at 48 kHz   eight clips\n");

        for (double step : { 1.0, 44100.0 / 48000.0, 48000.0 / 44100.0, 2.0, 4.0 })
        {
            const int runs = 200000;
            double accumulated = 0.0, position = 1000.37;

            const auto started = std::chrono::steady_clock::now();

            for (int i = 0; i < runs; ++i)
            {
                const auto f = ClipResampler::read (k, source.data(), source.data(), frames,
                                                    position, step);
                accumulated += (double) f.left + (double) f.right;
                position += step;

                if (position > frames - 4000)
                    position = 1000.37;
            }

            const auto finished = std::chrono::steady_clock::now();
            const double ns = std::chrono::duration<double, std::nano> (finished - started).count() / runs;

            std::printf ("   %8.5f  %4d    %8.1f    %8.2f%% of a core  %8.2f%%   (sum %.3g)\n",
                         step, 2 * ClipResampler::halfTapsForStep (k, step), ns,
                         ns * 48000.0 * 1.0e-7, ns * 48000.0 * 1.0e-7 * 8.0, accumulated);
        }

        {
            const int runs = 200000;
            double accumulated = 0.0, position = 1000.37;

            const auto started = std::chrono::steady_clock::now();

            for (int i = 0; i < runs; ++i)
            {
                const int f0 = (int) position;
                const double d = position - f0;
                const double v = source[(std::size_t) f0]
                               + (source[(std::size_t) (f0 + 1)] - source[(std::size_t) f0]) * d;
                accumulated += v + v;
                position += 1.0;

                if (position > frames - 4000)
                    position = 1000.37;
            }

            const auto finished = std::chrono::steady_clock::now();
            const double ns = std::chrono::duration<double, std::nano> (finished - started).count() / runs;

            std::printf ("   the old read   2    %8.2f    %8.3f%% of a core                 (sum %.3g)\n",
                         ns, ns * 48000.0 * 1.0e-7, accumulated);
        }

        std::printf ("\n   The obvious next step, which is deliberately not taken here: the tap\n");
        std::printf ("   index is recomputed per tap from a floating point distance, and a\n");
        std::printf ("   fixed point position stepped by a constant would hoist all of that out\n");
        std::printf ("   of the loop. At a step of one the tap set is also constant for a whole\n");
        std::printf ("   block, so the read degenerates to one fixed FIR with no table lookup at\n");
        std::printf ("   all. Both are real and neither was needed to afford this.\n");
        std::printf ("\n   Table: %d entries, taps and differences, %.0f kB.\n",
                     k.entryCount(), k.entryCount() * 2 * 4 / 1024.0);
    }

    return 0;
}
