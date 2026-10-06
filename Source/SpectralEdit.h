#pragma once
#include "Spectrogram.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

//==============================================================================
/** An edit to a region of the time against frequency plane: take this band
    out, over this stretch, by this much.

    This is the model behind the spectral repair tools: a cough under a vocal
    line, a chair creak between phrases, a ring of mains hum sitting on a
    harmonic. You draw a box around the offending energy and pull it down, and
    everything outside the box is supposed to survive untouched.

    The arithmetic here is all about the edge of that box, because the obvious
    implementation, zeroing every bin inside it and leaving every bin outside
    alone, sounds wrong in a way that is easy to blame on something else.

    Across frequency, a block of bins set to zero with its neighbours left at
    full level is a rectangular window applied to the spectrum. Multiplying in
    one domain is convolving in the other, and the transform of a rectangle is
    a Dirichlet kernel, which does not decay quickly: the edit rings across
    the whole analysis window, so a notch taken out of one note smears a tick
    over the whole frame it lived in. Across time it is the same argument a
    quarter turn round. A bin that is on in one frame and off in the next is a
    step in that bin's envelope, and a step has energy at every frequency, so
    switching a band off abruptly sprays a click up and down the spectrum.

    The standard remedy is to taper the mask rather than switching it, and
    that is what `regionGainAt` does: a raised cosine ramp from untouched to
    the requested gain, over a width the caller chooses, on all four sides.
    The cost is that the edit is a little less surgical than the box drawn on
    the screen, which is the right way round: a repair that reaches slightly
    too far is a repair, and one with a hard edge is a new artefact standing
    where the old one was.

    Two deliberate choices beyond that.

    The gain is applied to the complex bin as a real multiplier, so it scales
    the magnitude and leaves the phase exactly where the analysis found it.
    Phase is what makes the surrounding material rejoin itself across the
    repair, and there is nothing to be gained by inventing a new one.

    And a gain of zero is offered but is rarely what is wanted. Removing all
    of the energy in a band leaves a hole, and a hole in a broadband sound is
    as audible as the noise was: the ear notices the silence where the room
    used to be. Pulling the region down by a known amount leaves the noise
    floor continuous through the repair. The default below reflects that.

    No JUCE, so the tests can reach it.
*/
struct SpectralRegion
{
    double startSeconds = 0.0;
    double endSeconds = 0.0;

    double lowHertz = 0.0;
    double highHertz = 0.0;

    /** A linear multiplier on the magnitude inside the region. One leaves it
        alone, zero takes it out entirely, and something in between is almost
        always the better repair. */
    double gain = 0.125;

    /** How far either side of the region, in time, the gain ramps between
        untouched and `gain`. The useful floor is the length of one analysis
        window: ramping faster than the analysis can resolve asks the frames
        to do something they cannot represent. */
    double timeFadeSeconds = 0.02;

    /** The same either side in frequency. A few bins wide is enough; the
        useful floor is the spacing of one bin. */
    double frequencyFadeHertz = 50.0;
};

//==============================================================================
namespace SpectralEditDetail
{
    constexpr double pi = 3.14159265358979323846;

    /** Turns a position in frames or bins into an index into a container of
        `count` entries.

        Regions are drawn by a person on a picture and routinely run off the
        edge of the clip, so this is the normal case rather than the defensive
        one. The clamping is done before the conversion to an integer and not
        after, because a region ending at a large frequency, or at a time past
        the end of a long clip, produces a double well outside the range of an
        int, and converting one of those is undefined behaviour rather than a
        large int. A position that is not a number clamps to zero, since
        neither comparison holds for it. */
    inline int toIndex (double position, int count) noexcept
    {
        if (count <= 0)
            return 0;

        if (! (position > 0.0))
            return 0;

        const double last = (double) (count - 1);

        return position < last ? (int) position : count - 1;
    }
}

//==============================================================================
/** The ramp from untouched to covered: zero outside, one inside, and a raised
    cosine across the fade. The raised cosine is used rather than a straight
    line because its slope is zero at both ends, so the mask joins the
    untouched material smoothly instead of with a corner, and a corner is a
    discontinuity in the first derivative that rings for the same reason a
    step does, only more quietly. */
inline double spectralRegionCoverage (double x, double low, double high, double fade) noexcept
{
    if (high < low)
        std::swap (low, high);

    if (x >= low && x <= high)
        return 1.0;

    if (fade <= 0.0)
        return 0.0;

    const double distance = x < low ? low - x : x - high;

    // Written as the negation so that a position that is not a number, which
    // fails every comparison, falls out here as untouched rather than being
    // carried into the cosine and multiplied into somebody's audio. A region
    // comes from a rectangle somebody dragged, and a drag against a degenerate
    // scale is one divide away from producing one.
    if (! (distance < fade))
        return 0.0;

    const double t = 1.0 - distance / fade;

    return 0.5 - 0.5 * std::cos (SpectralEditDetail::pi * t);
}

/** The multiplier a region asks for at one point of the plane. One means
    untouched. */
inline double regionGainAt (const SpectralRegion& region, double seconds, double hertz) noexcept
{
    const double inTime = spectralRegionCoverage (seconds, region.startSeconds, region.endSeconds,
                                                  std::max (0.0, region.timeFadeSeconds));

    if (inTime <= 0.0)
        return 1.0;

    const double inFrequency = spectralRegionCoverage (hertz, region.lowHertz, region.highHertz,
                                                       std::max (0.0, region.frequencyFadeHertz));

    if (inFrequency <= 0.0)
        return 1.0;

    // Separable: the corners of the box get the product of the two ramps,
    // which is what makes the mask smooth diagonally as well as along each
    // axis.
    const double coverage = inTime * inFrequency;

    // Likewise negated: a gain that is not a number, or is below zero, is
    // taken as silence rather than passed on.
    const double gain = ! (region.gain > 0.0) ? 0.0 : region.gain;

    return 1.0 + coverage * (gain - 1.0);
}

/** Applies one region to an analysed spectrogram, in place.

    Only the frames and bins the region can reach are visited, fade included.
    A region that falls entirely outside the clip, or below zero hertz, or
    above Nyquist, is clamped away to nothing here rather than reaching the
    frame store: the bounds are the reason this is a function and not a loop
    at the call site. */
inline void applySpectralRegion (Spectrogram& spectrogram, const SpectralRegion& region)
{
    if (! spectrogram.isPrepared() || spectrogram.getNumFrames() <= 0)
        return;

    const double timeFade = std::max (0.0, region.timeFadeSeconds);
    const double frequencyFade = std::max (0.0, region.frequencyFadeHertz);

    const double firstSecond = std::min (region.startSeconds, region.endSeconds) - timeFade;
    const double lastSecond = std::max (region.startSeconds, region.endSeconds) + timeFade;
    const double lowHertz = std::min (region.lowHertz, region.highHertz) - frequencyFade;
    const double highHertz = std::max (region.lowHertz, region.highHertz) + frequencyFade;

    const int numFrames = spectrogram.getNumFrames();
    const int numBins = spectrogram.getNumBins();

    // These bounds only decide which frames and bins are worth visiting. What
    // each one is multiplied by is decided by `regionGainAt` from the frame's
    // own time and the bin's own frequency, and that returns one outside the
    // region and its fade, so a range that is a little too wide costs a few
    // multiplications by one and nothing else. Flooring the near edge and
    // taking the ceiling of the far one is what makes it a little too wide
    // rather than a little too narrow, with no reliance on how the arithmetic
    // rounds at the boundary.
    const int firstFrame = SpectralEditDetail::toIndex (std::floor (spectrogram.secondsToFrame (firstSecond)), numFrames);
    const int lastFrame = SpectralEditDetail::toIndex (std::ceil (spectrogram.secondsToFrame (lastSecond)), numFrames);
    const int firstBin = SpectralEditDetail::toIndex (std::floor (spectrogram.hertzToBin (lowHertz)), numBins);
    const int lastBin = SpectralEditDetail::toIndex (std::ceil (spectrogram.hertzToBin (highHertz)), numBins);

    for (int f = firstFrame; f <= lastFrame; ++f)
    {
        const double seconds = spectrogram.frameToSeconds (f);
        auto* bins = spectrogram.frame (f);

        for (int b = firstBin; b <= lastBin; ++b)
            bins[b] *= regionGainAt (region, seconds, spectrogram.binToHertz (b));
    }
}

inline void applySpectralRegions (Spectrogram& spectrogram, const std::vector<SpectralRegion>& regions)
{
    for (const auto& region : regions)
        applySpectralRegion (spectrogram, region);
}
