// ---------------------------------------------------------------------------
// Checks that a spectral edit takes out what it was drawn around and leaves
// everything else alone.
//
// Two things have to be true for a repair to be worth having, and they pull
// against each other. The edit has to actually remove the cough, and the music
// either side of it has to come back as though nothing happened. Any mask will
// manage the first. The second is what the fades are for, and what this
// measures.
//
// The headline check is the one that justifies the tapered edges existing at
// all. A band of bins switched off with its neighbours left at full level is a
// rectangular window applied to the spectrum, and multiplying in one domain is
// convolving in the other: the transform of a rectangle is a kernel that does
// not decay, so the edit rings for the whole length of the analysis window.
// Feeding an impulse through the edit shows that ringing directly, as energy
// sitting a long way from where the impulse was. Measured here at 48 kHz with
// a 2048 point window:
//
//   hard edge           ringing 33.6 dB below the impulse
//   100 Hz of taper     39.3 dB
//   200 Hz of taper     51.9 dB
//   400 Hz of taper     66.7 dB
//
// A hard edge is the naive version and sounds like a tick added where the
// problem used to be. The thresholds below sit well inside those numbers.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include "SpectralEdit.h"

#include <cmath>
#include <cstdio>
#include <random>
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
static constexpr double rate = 48000.0;
static constexpr int fftSize = 2048;

//==============================================================================
/** Runs a clip through an analysis, one edit and a resynthesis, which is what
    the editor will do to a selection. */
static std::vector<float> edited (const std::vector<float>& input, const SpectralRegion& region)
{
    Spectrogram spectrogram;
    spectrogram.prepare (fftSize, fftSize / 4, rate);
    spectrogram.analyse (input.data(), (int) input.size());
    applySpectralRegion (spectrogram, region);

    std::vector<float> output (input.size(), 0.0f);
    spectrogram.synthesise (output.data(), (int) input.size());

    return output;
}

/** The energy a signal carries between two frequencies, by analysing it and
    totalling the bins that fall in the band.

    The frames at either end are left out. A test tone that simply stops at the
    end of the buffer is a step, and a step has energy everywhere, so those
    frames carry a wideband click that has nothing to do with the edit being
    measured and would sit in the band at about fifty decibels down: loud
    enough to be mistaken for an edit that did not work. */
static double bandEnergy (const std::vector<float>& signal, double low, double high)
{
    Spectrogram spectrogram;
    spectrogram.prepare (fftSize, fftSize / 4, rate);
    spectrogram.analyse (signal.data(), (int) signal.size());

    // One analysis window either side, in frames.
    const int margin = fftSize / (fftSize / 4);

    double total = 0.0;

    for (int f = margin; f < spectrogram.getNumFrames() - margin; ++f)
    {
        const auto* bins = spectrogram.frame (f);

        for (int b = 0; b < spectrogram.getNumBins(); ++b)
        {
            const double hertz = spectrogram.binToHertz (b);

            if (hertz >= low && hertz <= high)
                total += std::norm (bins[b]);
        }
    }

    return total;
}

static double decibels (double ratio)
{
    return 10.0 * std::log10 (std::max (ratio, 1.0e-30));
}

static std::vector<float> tone (int length, double hertz, double amplitude)
{
    std::vector<float> signal ((size_t) length);

    for (int i = 0; i < length; ++i)
        signal[(size_t) i] = (float) (amplitude * std::sin (2.0 * pi * hertz * (double) i / rate));

    return signal;
}

//==============================================================================
static void aTaperedEdgeRingsFarLessThanAHardOne()
{
    const int length = 16384;
    const int impulseAt = length / 2;

    std::vector<float> impulse ((size_t) length, 0.0f);
    impulse[(size_t) impulseAt] = 1.0f;

    double previous = -1.0e30;
    double hard = 0.0, tapered = 0.0;

    for (double fade : { 0.0, 100.0, 200.0, 400.0 })
    {
        SpectralRegion region;
        region.startSeconds = -1.0;
        region.endSeconds = 10.0;
        region.timeFadeSeconds = 0.0;
        region.lowHertz = 2000.0;
        region.highHertz = 6000.0;
        region.gain = 0.0;
        region.frequencyFadeHertz = fade;

        const auto output = edited (impulse, region);

        double core = 0.0, ringing = 0.0;

        for (int i = 0; i < length; ++i)
        {
            const double energy = (double) output[(size_t) i] * (double) output[(size_t) i];

            if (std::abs (i - impulseAt) <= fftSize / 8)
                core += energy;
            else
                ringing += energy;
        }

        const double below = decibels (core / std::max (ringing, 1.0e-30));

        std::printf ("    %4.0f Hz of taper: ringing %.1f dB below the impulse\n", fade, below);

        // Wider tapers ring less, every step of the way. A single pair of
        // numbers could be a coincidence; a monotonic run of them is the
        // mechanism.
        check (below > previous, "a taper of " + std::to_string ((int) fade)
                                 + " Hz rang more than the narrower one before it");
        previous = below;

        if (fade == 0.0)
            hard = below;

        if (fade == 400.0)
            tapered = below;
    }

    check (tapered - hard > 25.0, "tapering the edge only improved the ringing by "
                                  + std::to_string (tapered - hard) + " dB");
}

static void whatIsCoveredGoesAndWhatIsNotStays()
{
    const int length = 24000;

    std::vector<float> both ((size_t) length);
    const auto low = tone (length, 1000.0, 0.5);
    const auto high = tone (length, 5000.0, 0.5);

    for (int i = 0; i < length; ++i)
        both[(size_t) i] = low[(size_t) i] + high[(size_t) i];

    SpectralRegion region;
    region.startSeconds = -1.0;
    region.endSeconds = 10.0;
    region.timeFadeSeconds = 0.0;
    region.lowHertz = 4500.0;
    region.highHertz = 5500.0;
    region.gain = 0.0;
    region.frequencyFadeHertz = 200.0;

    const auto output = edited (both, region);

    const double removedBefore = bandEnergy (both, 4800.0, 5200.0);
    const double removedAfter = bandEnergy (output, 4800.0, 5200.0);
    const double keptBefore = bandEnergy (both, 800.0, 1200.0);
    const double keptAfter = bandEnergy (output, 800.0, 1200.0);

    const double removal = decibels (removedAfter / removedBefore);
    const double damage = decibels (keptAfter / keptBefore);

    std::printf ("    the covered tone fell %.1f dB, the one next to it moved %.3f dB\n",
                 removal, damage);

    check (removal < -60.0, "the tone inside the region only fell " + std::to_string (-removal) + " dB");
    check (std::fabs (damage) < 0.05, "the tone outside the region moved by "
                                      + std::to_string (damage) + " dB");
}

static void anEditStaysInsideItsOwnStretchOfTime()
{
    const int length = 48000;
    const auto input = tone (length, 2000.0, 0.5);

    SpectralRegion region;
    region.startSeconds = 0.4;
    region.endSeconds = 0.6;
    region.timeFadeSeconds = 0.02;
    region.lowHertz = 0.0;
    region.highHertz = 24000.0;
    region.gain = 0.0;
    region.frequencyFadeHertz = 0.0;

    const auto output = edited (input, region);

    // Outside the region, plus its fade, plus the half window either side that
    // any frame touching the region reaches into.
    const int safe = (int) (0.1 * rate);
    const int first = (int) (0.4 * rate) - safe;
    const int last = (int) (0.6 * rate) + safe;

    double outside = 0.0;

    for (int i = 0; i < length; ++i)
        if (i < first || i > last)
            outside = std::max (outside, std::fabs ((double) output[(size_t) i] - (double) input[(size_t) i]));

    double inside = 0.0;

    for (int i = (int) (0.45 * rate); i < (int) (0.55 * rate); ++i)
        inside = std::max (inside, std::fabs ((double) output[(size_t) i]));

    std::printf ("    outside the edit the signal moved by %.2e, inside it %.2e remains\n",
                 outside, inside);

    check (outside < 1.0e-4, "material outside the edited stretch moved by " + std::to_string (outside));
    check (inside < 1.0e-3, "the edited stretch still carries " + std::to_string (inside));
}

static void aGainOfOneChangesNothing()
{
    const int length = 8000;
    const auto input = tone (length, 3000.0, 0.6);

    SpectralRegion region;
    region.startSeconds = 0.0;
    region.endSeconds = 0.1;
    region.lowHertz = 2000.0;
    region.highHertz = 4000.0;
    region.gain = 1.0;
    region.timeFadeSeconds = 0.05;
    region.frequencyFadeHertz = 500.0;

    const auto output = edited (input, region);

    double worst = 0.0;

    for (int i = 0; i < length; ++i)
        worst = std::max (worst, std::fabs ((double) output[(size_t) i] - (double) input[(size_t) i]));

    check (worst < 1.0e-5, "a region asking for no change moved the signal by " + std::to_string (worst));
}

static void theRampIsSmoothAndGoesTheRightWay()
{
    const double low = 100.0, high = 200.0, fade = 50.0;

    check (std::fabs (spectralRegionCoverage (150.0, low, high, fade) - 1.0) < 1.0e-12,
           "the middle of a region is not fully covered");
    check (std::fabs (spectralRegionCoverage (low, low, high, fade) - 1.0) < 1.0e-12,
           "the edge of a region is not fully covered");
    check (spectralRegionCoverage (low - fade, low, high, fade) == 0.0,
           "the far end of the fade is not clear of the region");
    check (spectralRegionCoverage (low - fade - 1.0, low, high, fade) == 0.0,
           "something beyond the fade is still covered");
    check (spectralRegionCoverage (1000.0, low, high, fade) == 0.0, "a long way away is still covered");

    // Monotonic across the fade, and with no step anywhere along it, which is
    // the whole reason for a raised cosine rather than a straight line or a
    // switch.
    double previous = 0.0;

    for (int i = 0; i <= 500; ++i)
    {
        const double x = low - fade + fade * (double) i / 500.0;
        const double c = spectralRegionCoverage (x, low, high, fade);

        check (c >= previous - 1.0e-12, "the ramp into a region goes backwards at " + std::to_string (x));
        check (c - previous < 0.02, "the ramp into a region steps by " + std::to_string (c - previous)
                                    + " at " + std::to_string (x));
        previous = c;
    }

    check (std::fabs (previous - 1.0) < 1.0e-9, "the ramp does not reach the region it leads into");

    // A region given back to front is the same region. Somebody dragging a box
    // upwards and to the left produces one on every other attempt.
    check (std::fabs (spectralRegionCoverage (150.0, high, low, fade) - 1.0) < 1.0e-12,
           "a region given back to front is not the same region");

    // With no fade asked for, the mask is the box and nothing outside it.
    check (spectralRegionCoverage (99.0, low, high, 0.0) == 0.0, "a region with no fade bled outside itself");
    check (spectralRegionCoverage (150.0, low, high, 0.0) == 1.0, "a region with no fade lost its middle");
}

static void thePhaseIsLeftWhereItWasFound()
{
    const int length = 8000;
    const auto input = tone (length, 3000.0, 0.6);

    Spectrogram before, after;
    before.prepare (fftSize, fftSize / 4, rate);
    after.prepare (fftSize, fftSize / 4, rate);
    before.analyse (input.data(), length);
    after.analyse (input.data(), length);

    SpectralRegion region;
    region.startSeconds = -1.0;
    region.endSeconds = 10.0;
    region.timeFadeSeconds = 0.0;
    region.lowHertz = 2500.0;
    region.highHertz = 3500.0;
    region.gain = 0.25;
    region.frequencyFadeHertz = 300.0;

    applySpectralRegion (after, region);

    double worst = 0.0;

    for (int f = 0; f < after.getNumFrames(); ++f)
    {
        const auto* was = before.frame (f);
        const auto* is = after.frame (f);

        for (int b = 0; b < after.getNumBins(); ++b)
        {
            // Only where there was something to have a phase. An empty bin's
            // angle is noise.
            if (std::abs (was[b]) < 1.0e-6)
                continue;

            double turn = std::arg (is[b]) - std::arg (was[b]);

            while (turn > pi) turn -= 2.0 * pi;
            while (turn < -pi) turn += 2.0 * pi;

            worst = std::max (worst, std::fabs (turn));
        }
    }

    check (worst < 1.0e-9, "an edit turned the phase by " + std::to_string (worst) + " radians");
}

static void regionsOffTheEndOfTheWorldAreHarmless()
{
    const int length = 4000;
    const auto input = tone (length, 1000.0, 0.5);

    const double nan = std::nan ("");
    const double huge = 1.0e300;

    SpectralRegion regions[6];

    // Entirely before the clip.
    regions[0] = { -50.0, -40.0, 100.0, 200.0, 0.0, 0.01, 10.0 };
    // Entirely after it.
    regions[1] = { 900.0, 1000.0, 100.0, 200.0, 0.0, 0.01, 10.0 };
    // Below direct current.
    regions[2] = { 0.0, 0.1, -5000.0, -1000.0, 0.0, 0.01, 10.0 };
    // Above Nyquist.
    regions[3] = { 0.0, 0.1, 40000.0, 90000.0, 0.0, 0.01, 10.0 };
    // Numbers no scale should ever produce, but a divide by a zoom level of
    // zero will.
    regions[4] = { -huge, huge, -huge, huge, 1.0, huge, huge };
    regions[5] = { nan, nan, nan, nan, nan, nan, nan };

    for (int i = 0; i < 6; ++i)
    {
        const auto output = edited (input, regions[i]);

        double worst = 0.0;

        for (int s = 0; s < length; ++s)
        {
            check (std::isfinite (output[(size_t) s]), "region " + std::to_string (i)
                                                       + " produced something that is not finite");
            worst = std::max (worst, std::fabs ((double) output[(size_t) s] - (double) input[(size_t) s]));
        }

        check (worst < 1.0e-4, "region " + std::to_string (i) + ", which reaches nothing inside the clip, "
                               "moved the signal by " + std::to_string (worst));
    }
}

static void severalRegionsStack()
{
    const int length = 24000;

    std::vector<float> three ((size_t) length);
    const auto a = tone (length, 1000.0, 0.3);
    const auto b = tone (length, 5000.0, 0.3);
    const auto c = tone (length, 9000.0, 0.3);

    for (int i = 0; i < length; ++i)
        three[(size_t) i] = a[(size_t) i] + b[(size_t) i] + c[(size_t) i];

    std::vector<SpectralRegion> regions;

    for (double centre : { 5000.0, 9000.0 })
    {
        SpectralRegion region;
        region.startSeconds = -1.0;
        region.endSeconds = 10.0;
        region.timeFadeSeconds = 0.0;
        region.lowHertz = centre - 400.0;
        region.highHertz = centre + 400.0;
        region.gain = 0.0;
        region.frequencyFadeHertz = 200.0;
        regions.push_back (region);
    }

    Spectrogram spectrogram;
    spectrogram.prepare (fftSize, fftSize / 4, rate);
    spectrogram.analyse (three.data(), length);
    applySpectralRegions (spectrogram, regions);

    std::vector<float> output ((size_t) length, 0.0f);
    spectrogram.synthesise (output.data(), length);

    check (decibels (bandEnergy (output, 4800.0, 5200.0) / bandEnergy (three, 4800.0, 5200.0)) < -60.0,
           "the first of two regions did not take its tone out");
    check (decibels (bandEnergy (output, 8800.0, 9200.0) / bandEnergy (three, 8800.0, 9200.0)) < -60.0,
           "the second of two regions did not take its tone out");
    check (std::fabs (decibels (bandEnergy (output, 800.0, 1200.0) / bandEnergy (three, 800.0, 1200.0))) < 0.05,
           "two regions between them damaged the tone neither of them covered");
}

int main()
{
    std::printf ("spectral edit\n");

    aTaperedEdgeRingsFarLessThanAHardOne();
    whatIsCoveredGoesAndWhatIsNotStays();
    anEditStaysInsideItsOwnStretchOfTime();
    aGainOfOneChangesNothing();
    theRampIsSmoothAndGoesTheRightWay();
    thePhaseIsLeftWhereItWasFound();
    regionsOffTheEndOfTheWorldAreHarmless();
    severalRegionsStack();

    if (failures > 0)
    {
        std::printf ("\n%d spectral edit check(s) failed\n", failures);
        return 1;
    }

    std::printf ("all spectral edit checks passed\n");
    return 0;
}
