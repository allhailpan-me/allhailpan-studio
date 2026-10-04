#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

// ---------------------------------------------------------------------------
// Warp markers: the arithmetic that turns an arrangement beat into a position
// in the source audio when a clip's timing is pinned rather than uniform.
//
// A warp marker pins one point in the source audio to one position on the
// arrangement grid, and the audio between two consecutive markers is stretched
// to fit the span between them. This is what Ableton Live's warp markers mean,
// and the behaviour is modelled on the Live reference manual: a marker "locks a
// specific point in a sample, such as a transient, to a particular place in the
// timeline", the material between markers is stretched or compressed to suit,
// and the material before the first and after the last marker carries on at the
// rate of the segment next to it.
//
// The clip's left edge is always an implicit marker, pinning source second
// `offset` to the clip's start. That is what keeps `Clip::offset` meaning the
// same thing whether or not a clip is warped, and it is why a clip with no
// markers at all behaves exactly as it did before warping existed: the whole
// clip is then one segment at the fallback rate, which is the clip's single
// stretch ratio.
//
// Rates here are "source seconds per beat", not a stretch ratio, because that
// is the one quantity that does not change when the project tempo does. Raising
// the tempo shortens a beat in real time, so the same source seconds are squeezed
// into less time and the audio stretches further. That is the point of warping:
// markers stay on the grid.
//
// This header is deliberately free of JUCE so the mapping can be compiled on
// its own under sanitizers. See Tests/WarpMapTest.cpp.
// ---------------------------------------------------------------------------

struct WarpMarker
{
    double source = 0.0;   // seconds into the source audio
    double beat   = 0.0;   // beats after the clip's start

    bool operator== (const WarpMarker&) const = default;
};

// A marker has to clear its predecessor in both coordinates by at least this
// much to be usable. Two markers at the same beat would ask for a segment of
// zero length, and two at the same source position would ask the stretcher to
// hold a single sample for a stretch of time. Both come up in practice, from
// double clicks and from markers dragged onto each other, so they are dropped
// rather than defended against further down.
inline constexpr double kMinWarpBeatSpan   = 1.0e-4;   // beats, 50us at 120bpm
inline constexpr double kMinWarpSourceSpan = 1.0e-5;   // seconds

// Far more markers than any performance needs: a five minute take at 174bpm is
// under nine hundred beats. The cap is here so that a corrupt or hostile
// project file cannot hand the audio thread a segment table of any size it
// likes.
inline constexpr size_t kMaxWarpMarkers = 4096;

/** Puts a marker list into the form the lookups below rely on: sorted by beat,
    strictly increasing in both beat and source position, and entirely after the
    clip's left edge, which is an implicit marker at (offset, beat 0).

    Markers that break that order are dropped, which is what makes dragging one
    marker past another, or stacking two on top of each other, harmless. The
    survivor is the earlier one by beat, so dragging a marker onto its neighbour
    leaves the neighbour rather than silently moving it.
*/
inline void normaliseWarpMarkers (std::vector<WarpMarker>& markers, double offset)
{
    // Anything that is not a number goes first. Sorting with one still in the
    // list would break the comparator's strict weak ordering, which is
    // undefined behaviour, and a project file is not to be trusted that far.
    markers.erase (std::remove_if (markers.begin(), markers.end(),
                                   [] (const WarpMarker& m)
                                   {
                                       return ! (std::isfinite (m.beat) && std::isfinite (m.source));
                                   }),
                   markers.end());

    std::stable_sort (markers.begin(), markers.end(),
                      [] (const WarpMarker& a, const WarpMarker& b) { return a.beat < b.beat; });

    double prevBeat = 0.0, prevSource = offset;
    std::vector<WarpMarker> kept;
    kept.reserve (markers.size());

    for (const auto& m : markers)
    {
        if (m.beat < prevBeat + kMinWarpBeatSpan || m.source < prevSource + kMinWarpSourceSpan)
            continue;
        kept.push_back (m);
        prevBeat   = m.beat;
        prevSource = m.source;

        if (kept.size() >= kMaxWarpMarkers)
            break;
    }

    markers.swap (kept);
}

/** The rate a clip with no markers reads at, in source seconds per beat.

    This is the single `stretch` ratio expressed the same way as a warp segment,
    so that an unwarped clip is just a one segment warp.
*/
inline double warpFallbackSlope (double bpm, double stretch) noexcept
{
    const double secondsPerBeat = 60.0 / std::max (1.0e-6, bpm);
    return secondsPerBeat / std::max (1.0e-9, stretch);
}

/** Source seconds at a beat measured from the clip's start.

    Walks the markers, so it is for the message thread and the interface. The
    audio thread uses the precomputed segments from buildWarpSegments instead.
    Markers must already be normalised.
*/
inline double warpSourceAtBeat (const std::vector<WarpMarker>& markers, double offset,
                                double fallbackSlope, double beat) noexcept
{
    if (markers.empty())
        return offset + beat * fallbackSlope;

    double b0 = 0.0, s0 = offset;

    for (size_t i = 0; i < markers.size(); ++i)
    {
        const double b1 = markers[i].beat, s1 = markers[i].source;
        const double slope = std::max (1.0e-12, (s1 - s0) / (b1 - b0));

        // The last segment's rate carries on past the last marker, and the
        // first segment's rate carries on back before the clip's start, which
        // is what the renderer needs to place the head of the file.
        if (beat < b1 || i + 1 == markers.size())
            return s0 + (beat - b0) * slope;

        b0 = b1;
        s0 = s1;
    }

    return s0;   // not reachable: the loop always returns on its last pass
}

/** The inverse: the beat, measured from the clip's start, at a source position.
    Used for the clip's length in beats and for placing markers under the mouse.
    Markers must already be normalised, which is what makes this invertible.
*/
inline double warpBeatAtSource (const std::vector<WarpMarker>& markers, double offset,
                                double fallbackSlope, double source) noexcept
{
    if (markers.empty())
        return (source - offset) / std::max (1.0e-12, fallbackSlope);

    double b0 = 0.0, s0 = offset;

    for (size_t i = 0; i < markers.size(); ++i)
    {
        const double b1 = markers[i].beat, s1 = markers[i].source;
        const double slope = std::max (1.0e-12, (s1 - s0) / (b1 - b0));

        if (source < s1 || i + 1 == markers.size())
            return b0 + (source - s0) / slope;

        b0 = b1;
        s0 = s1;
    }

    return b0;   // not reachable
}

/** The stretch ratio a clip needs so that, with no markers, it reads at a
    given rate. The inverse of warpFallbackSlope, used when a cut or a trim
    leaves a piece with no markers to go on.
*/
inline double warpStretchForSlope (double bpm, double slope) noexcept
{
    return 60.0 / std::max (1.0e-6, bpm) / std::max (1.0e-12, slope);
}

/** Moves a marker list onto a new left edge.

    A clip that has been cut or trimmed describes the same audio in a new frame:
    the markers keep the source positions and the grid positions they always
    had, but the beats are measured from a start that has moved by `beatShift`,
    and the implicit marker at the left edge is now at `newOffset`. Markers the
    new edge has swallowed are dropped, because the edge itself now pins that
    part of the audio.

    Together with a stretch ratio taken from warpSlopeAtBeat at the same point,
    this makes a cut inaudible: the new piece reads from the same place at the
    same rate.
*/
inline void rebaseWarpMarkers (std::vector<WarpMarker>& markers, double beatShift, double newOffset)
{
    for (auto& m : markers)
        m.beat -= beatShift;

    normaliseWarpMarkers (markers, newOffset);
}

/** The rate, in source seconds per beat, that a clip reads at around a beat.

    Needed when a clip is cut or trimmed: the new piece has to carry on reading
    at the rate the old one was reading at that point, or the cut would be
    audible as a change of speed. Markers must already be normalised.
*/
inline double warpSlopeAtBeat (const std::vector<WarpMarker>& markers, double offset,
                               double fallbackSlope, double beat) noexcept
{
    if (markers.empty())
        return fallbackSlope;

    double b0 = 0.0, s0 = offset;

    for (size_t i = 0; i < markers.size(); ++i)
    {
        const double b1 = markers[i].beat, s1 = markers[i].source;
        const double slope = std::max (1.0e-12, (s1 - s0) / (b1 - b0));

        if (beat < b1 || i + 1 == markers.size())
            return slope;

        b0 = b1;
        s0 = s1;
    }

    return fallbackSlope;   // not reachable
}

// One leg of the piecewise mapping, in absolute arrangement beats, with its
// rate worked out in advance so the audio thread does no division per sample.
struct WarpSegment
{
    double beat   = 0.0;   // arrangement beats where this segment starts
    double source = 0.0;   // source seconds at that beat
    double slope  = 0.0;   // source seconds per beat, from here until the next
};

/** Appends the segments for one clip, in absolute arrangement beats.

    Always writes at least one segment, so a caller can read the table without
    checking for an empty one. The last segment's rate is the one that carries
    on to the end of the clip. Markers must already be normalised.

    `sourceScale` converts source seconds into the time base of the audio the
    engine will actually read. It is 1 when reading the file itself, and the
    clip's stretch ratio when reading a copy that Rubber Band has already
    stretched by that much: in that copy one second of audio covers 1/stretch
    seconds of the original. Scaling the table rather than the markers is what
    lets the overall tempo match keep its pitch while the markers only have to
    describe the drift left over.
*/
inline void buildWarpSegments (const std::vector<WarpMarker>& markers, double offset,
                               double fallbackSlope, double startBeat,
                               std::vector<WarpSegment>& out, double sourceScale = 1.0)
{
    const double scale = std::max (1.0e-9, sourceScale);
    out.push_back ({ startBeat, offset * scale, std::max (1.0e-12, fallbackSlope) * scale });

    double b0 = 0.0, s0 = offset;

    for (const auto& m : markers)
    {
        const double slope = std::max (1.0e-12, (m.source - s0) / (m.beat - b0));
        out[out.size() - 1].slope = slope * scale;
        out.push_back ({ startBeat + m.beat, m.source * scale, slope * scale });   // tail rate, replaced if more follow
        b0 = m.beat;
        s0 = m.source;
    }
}

/** The audio thread's lookup into that table.

    `cursor` is kept between samples, so a block that walks forward costs one
    comparison per sample and no division. It is corrected rather than trusted,
    so looping the transport back over a warped clip is safe. No allocation, no
    lock, no branch on anything but the table itself, which is what the audio
    thread requires.
*/
inline double warpSourceAtSegment (const WarpSegment* segments, int count,
                                   int& cursor, double beat) noexcept
{
    if (segments == nullptr || count <= 0)
        return 0.0;

    cursor = cursor < 0 ? 0 : (cursor >= count ? count - 1 : cursor);

    while (cursor + 1 < count && segments[cursor + 1].beat <= beat)
        ++cursor;
    while (cursor > 0 && segments[cursor].beat > beat)
        --cursor;

    const auto& s = segments[cursor];
    return s.source + (beat - s.beat) * s.slope;
}
