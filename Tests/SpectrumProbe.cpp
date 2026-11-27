// ---------------------------------------------------------------------------
// Measures how a pure tone reads in a band, across every sample rate,
// transform size and bandwidth the studio offers.
//
// Not run by Tests/run.sh: a *Probe.cpp is a measurement that justified a
// design decision, kept so the decision can be rechecked. This one justifies
// the one bin refusal threshold in SpectrumAnalyser.h and supplies every
// figure that header quotes about narrow bands. It exists because an audit
// found that those figures had been measured at 48 kHz with the longest window
// and the one third octave band set, which is the single combination in the
// whole matrix where every band resolves, and then written down as general
// properties. Three of them were wrong.
//
//   g++ -std=c++20 -O2 -I Source -o /tmp/probe Tests/SpectrumProbe.cpp && /tmp/probe
//
// What it prints, in order:
//
//   1. Whether the refused bands are always the lowest ones, which is what
//      makes the display have one boundary rather than holes in the middle.
//   2. The worst reading a tone can give anywhere inside its own band, and
//      where that happens, which is the bound the test asserts.
//   3. The same for a tone at the band's own centre, which is the part a
//      longer window fixes.
//   4. How much of a tone goes undrawn at the boundary between the lowest
//      resolved band and the refused one below it, which is the one place
//      where the display really does lose energy rather than share it.
//   5. The error against band width, which is the table in the header, with
//      the configuration each row was measured at.
// ---------------------------------------------------------------------------

#include "SpectrumAnalyser.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace Spectrum;

static constexpr double twoPi = 6.283185307179586476925286766559;

static void makeSine (std::vector<float>& out, int n, double rate, double frequency)
{
    out.assign ((std::size_t) n, 0.0f);

    for (int i = 0; i < n; ++i)
        out[(std::size_t) i] = (float) std::sin (twoPi * frequency * (double) i / rate);
}

/** The tone is fed to both legs, which is what the panel does and what the
    scale is referenced to, so a tone the display captures entirely reads
    0.0 dB. */
static bool read (Analyser& analyser, int n, double rate, double frequency,
                  double& inBandDb, double& wholeDisplayDb, int band)
{
    std::vector<float> sine;
    makeSine (sine, n, rate, frequency);

    analyser.clear();

    if (! analyser.push (sine.data(), sine.data(), n, 1.0 / 30.0))
        return false;

    inBandDb = analyser.levelDb (band);

    double summed = 0.0;

    for (int i = 0; i < analyser.getNumBands(); ++i)
        if (analyser.resolves (i))
            summed += std::pow (10.0, (analyser.levelDb (i) - fullScaleReferenceDb) / 10.0);

    wholeDisplayDb = summed > 0.0 ? 10.0 * std::log10 (summed) + fullScaleReferenceDb : -200.0;
    return true;
}

int main()
{
    const double rates[5] = { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
    const int    perOctaves[2] = { 3, 6 };

    // ---- 1. are the refused bands always the lowest ones? ----
    std::printf ("Refusals are a prefix of the display?\n");
    bool prefixEverywhere = true;

    for (const double rate : rates)
        for (const int size : transformSizes)
            for (const int perOctave : perOctaves)
            {
                Analyser analyser;
                analyser.configure (size, rate, perOctave);

                bool seenResolved = false;

                for (int i = 0; i < analyser.getNumBands(); ++i)
                {
                    if (analyser.resolves (i))
                        seenResolved = true;
                    else if (seenResolved)
                        prefixEverywhere = false;
                }
            }

    std::printf ("  %s\n\n", prefixEverywhere
                                 ? "yes, at every rate, size and bandwidth: band width grows with "
                                   "frequency and the bin width does not, so there is exactly one "
                                   "boundary and it is at the bottom"
                                 : "NO, there is a hole in the middle of some display");

    // ---- 2, 3 and 4 ----
    double worstInBand = 0.0, worstCentred = 0.0, worstUndrawn = 0.0;
    std::string whereInBand, whereCentred, whereUndrawn;

    for (const double rate : rates)
        for (const int size : transformSizes)
            for (const int perOctave : perOctaves)
            {
                Analyser analyser;
                analyser.configure (size, rate, perOctave);

                if (! analyser.isConfigured())
                    continue;

                int lowestResolved = -1;

                for (int i = 0; i < analyser.getNumBands(); ++i)
                    if (analyser.resolves (i))
                    {
                        lowestResolved = i;
                        break;
                    }

                for (int band = 0; band < analyser.getNumBands(); ++band)
                {
                    if (! analyser.resolves (band))
                        continue;

                    const auto& b = analyser.getBands()[(std::size_t) band];

                    const std::string where = std::to_string ((int) rate) + " Hz, " + std::to_string (size)
                                            + ", 1/" + std::to_string (perOctave) + ", band at "
                                            + std::to_string ((int) b.centre) + " Hz, "
                                            + std::to_string (analyser.bandWeight (band)) + " bins";

                    for (int step = 0; step <= 40; ++step)
                    {
                        const double fraction  = (double) step / 40.0;
                        const double frequency = b.lower + fraction * (b.upper - b.lower);

                        double inBand = 0.0, whole = 0.0;

                        if (! read (analyser, size, rate, frequency, inBand, whole, band))
                            continue;

                        if (inBand < worstInBand)
                        {
                            worstInBand = inBand;
                            whereInBand = where + ", tone at " + std::to_string ((int) frequency) + " Hz";
                        }

                        // Energy undrawn, which can only happen where the band
                        // next door is refused: between two resolved bands the
                        // lobe lands in one or the other.
                        if (band == lowestResolved && whole < worstUndrawn)
                        {
                            worstUndrawn = whole;
                            whereUndrawn = where + ", tone at " + std::to_string ((int) frequency) + " Hz";
                        }
                    }

                    double centred = 0.0, whole = 0.0;

                    if (read (analyser, size, rate, b.centre, centred, whole, band)
                        && centred < worstCentred)
                    {
                        worstCentred = centred;
                        whereCentred = where;
                    }
                }
            }

    std::printf ("Worst reading for a tone anywhere inside its own band\n");
    std::printf ("  %.3f dB   (%s)\n\n", worstInBand, whereInBand.c_str());

    std::printf ("Worst reading for a tone at its band's own centre\n");
    std::printf ("  %.3f dB   (%s)\n\n", worstCentred, whereCentred.c_str());

    std::printf ("Worst total over the WHOLE display, for a tone in the lowest resolved band\n");
    std::printf ("  %.3f dB, so %.1f per cent of the tone is drawn nowhere   (%s)\n\n",
                 worstUndrawn,
                 100.0 * (1.0 - std::pow (10.0, worstUndrawn / 10.0)),
                 whereUndrawn.c_str());

    // ---- 5. the error against band width ----
    //
    // Collected over the whole matrix rather than at one rate, because the
    // widths available at 48 kHz do not go below 1.57 bins and the header's
    // table used to show a 1.2 bin row that does not exist there.
    std::printf ("Error at the band's centre against the band's width in bins,\n"
                 "and the error anywhere in the band, over every configuration:\n");
    std::printf ("  %-8s %-10s %-10s %s\n", "bins", "centre dB", "worst dB", "measured at");

    const double widths[8] = { 1.0, 1.25, 1.5, 2.0, 2.5, 3.0, 4.0, 8.0 };

    for (int w = 0; w + 1 < 8; ++w)
    {
        double bestFitCentre = 1.0, bestFitWorst = 1.0, bestFitWidth = 0.0;
        std::string at;

        for (const double rate : rates)
            for (const int size : transformSizes)
                for (const int perOctave : perOctaves)
                {
                    Analyser analyser;
                    analyser.configure (size, rate, perOctave);

                    for (int band = 0; band < analyser.getNumBands(); ++band)
                    {
                        if (! analyser.resolves (band))
                            continue;

                        const double width = analyser.bandWeight (band);

                        if (width < widths[w] || width >= widths[w + 1])
                            continue;

                        const auto& b = analyser.getBands()[(std::size_t) band];

                        double centred = 0.0, whole = 0.0;

                        if (! read (analyser, size, rate, b.centre, centred, whole, band))
                            continue;

                        double worst = centred;

                        for (int step = 0; step <= 40; ++step)
                        {
                            const double frequency = b.lower
                                                   + ((double) step / 40.0) * (b.upper - b.lower);
                            double inBand = 0.0;

                            if (read (analyser, size, rate, frequency, inBand, whole, band))
                                worst = std::min (worst, inBand);
                        }

                        if (centred < bestFitCentre)
                        {
                            bestFitCentre = centred;
                            bestFitWorst  = worst;
                            bestFitWidth  = width;
                            at = std::to_string ((int) rate) + " Hz, " + std::to_string (size)
                               + ", 1/" + std::to_string (perOctave) + ", "
                               + std::to_string ((int) b.centre) + " Hz";
                        }
                    }
                }

        if (bestFitWidth > 0.0)
            std::printf ("  %-8.2f %-10.2f %-10.2f %s\n",
                         bestFitWidth, bestFitCentre, bestFitWorst, at.c_str());
    }

    return 0;
}
