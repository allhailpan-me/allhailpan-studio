// ---------------------------------------------------------------------------
// Checks the beat to source mapping that warp markers describe.
//
// WarpMap.h has no JUCE in it precisely so this can be built and run on its
// own, with sanitizers, in a few seconds. CI runs it on every push, and
// ./Tests/run.sh runs it the same way here.
//
// The mapping is the kind of arithmetic that goes wrong quietly. A warp that is
// a hair off at one marker does not fail, it drifts, and a producer only finds
// out when the take no longer sits with the drums. So the properties are
// checked exhaustively rather than at a few hand picked points, and over
// thousands of randomly generated marker sets, including the degenerate ones
// that a mouse produces: markers dragged past each other, dropped on top of
// each other, and placed before the start of the clip.
// ---------------------------------------------------------------------------

#include "WarpMap.h"

#include <cstdio>
#include <cstdlib>
#include <random>
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

static void checkClose (double a, double b, double tol, const std::string& what)
{
    if (! (std::abs (a - b) <= tol))
        fail (what + "  (" + std::to_string (a) + " vs " + std::to_string (b)
                    + ", off by " + std::to_string (std::abs (a - b)) + ")");
}

// ---------------------------------------------------------------------------
// The properties every normalised marker set has to satisfy, whatever it is.

static void checkInvariants (const std::vector<WarpMarker>& markers, double offset,
                             double fallbackSlope, double startBeat, double sweepBeats,
                             const std::string& label)
{
    // The left edge of the clip is an implicit marker, so the map starts there.
    checkClose (warpSourceAtBeat (markers, offset, fallbackSlope, 0.0), offset,
                1.0e-12, label + ": clip start does not read from its offset");

    // Exact at every marker. This is the one a producer notices: a marker is a
    // promise that this point in the audio lands on this beat.
    for (size_t i = 0; i < markers.size(); ++i)
    {
        checkClose (warpSourceAtBeat (markers, offset, fallbackSlope, markers[i].beat),
                    markers[i].source, 1.0e-9,
                    label + ": marker " + std::to_string (i) + " is not exact");

        checkClose (warpBeatAtSource (markers, offset, fallbackSlope, markers[i].source),
                    markers[i].beat, 1.0e-9,
                    label + ": marker " + std::to_string (i) + " is not exact inverted");
    }

    // Normalisation is supposed to leave the list strictly increasing in both
    // coordinates, and clear of the implicit marker at the clip's start.
    double prevBeat = 0.0, prevSource = offset;
    for (size_t i = 0; i < markers.size(); ++i)
    {
        check (markers[i].beat > prevBeat,
               label + ": marker " + std::to_string (i) + " does not advance in beats");
        check (markers[i].source > prevSource,
               label + ": marker " + std::to_string (i) + " does not advance in the source");
        prevBeat   = markers[i].beat;
        prevSource = markers[i].source;
    }

    // Build the table the audio thread reads and sweep the clip finely, which
    // covers monotonic, continuous, and the two paths agreeing.
    std::vector<WarpSegment> segments;
    buildWarpSegments (markers, offset, fallbackSlope, startBeat, segments);
    check (! segments.empty(), label + ": no segments built");
    check (segments.size() == markers.size() + 1, label + ": wrong number of segments");

    for (size_t i = 0; i < segments.size(); ++i)
    {
        check (segments[i].slope > 0.0, label + ": segment " + std::to_string (i) + " has no rate");
        if (i > 0)
        {
            check (segments[i].beat > segments[i - 1].beat,
                   label + ": segment " + std::to_string (i) + " is out of order");
            check (segments[i].source > segments[i - 1].source,
                   label + ": segment " + std::to_string (i) + " reads backwards");
        }
    }

    const int    steps = 4001;
    const double step  = sweepBeats / (steps - 1);
    int cursor = 0;
    double prev = -1.0e300;

    for (int i = 0; i < steps; ++i)
    {
        const double beat = i * step;
        const double here = warpSourceAtBeat (markers, offset, fallbackSlope, beat);

        // Monotonic: the read position never goes backwards, or the audio would
        // stutter, and the map would stop being invertible.
        check (here > prev, label + ": not monotonic at beat " + std::to_string (beat));

        // Continuous: consecutive samples never jump by more than the steepest
        // rate in the map allows. A jump would be an audible click at a marker.
        double steepest = 0.0;
        for (const auto& s : segments)
            steepest = std::max (steepest, s.slope);
        if (i > 0)
            check (here - prev <= steepest * step * (1.0 + 1.0e-9) + 1.0e-12,
                   label + ": jumps at beat " + std::to_string (beat));

        // The audio thread's cursor walk has to agree with the plain walk the
        // interface uses, or what is heard and what is drawn would disagree.
        const double fromSegments = warpSourceAtSegment (segments.data(), (int) segments.size(),
                                                         cursor, startBeat + beat);
        checkClose (fromSegments, here, 1.0e-9,
                    label + ": segment table disagrees at beat " + std::to_string (beat));

        // Round trip through the inverse.
        checkClose (warpBeatAtSource (markers, offset, fallbackSlope, here), beat,
                    1.0e-7, label + ": round trip fails at beat " + std::to_string (beat));

        prev = here;
    }

    // The cursor is corrected rather than trusted, so the transport looping
    // back over a warped clip has to give the same answers as playing forward.
    for (int i = steps - 1; i >= 0; --i)
    {
        const double beat = i * step;
        const double fromSegments = warpSourceAtSegment (segments.data(), (int) segments.size(),
                                                         cursor, startBeat + beat);
        checkClose (fromSegments, warpSourceAtBeat (markers, offset, fallbackSlope, beat), 1.0e-9,
                    label + ": segment table disagrees playing backwards at beat "
                          + std::to_string (beat));
    }

    // A cursor left anywhere at all, including out of range, still answers.
    for (int c : { -1000, -1, 0, 1, (int) segments.size(), (int) segments.size() + 1000 })
    {
        int probe = c;
        const double beat = sweepBeats * 0.5;
        checkClose (warpSourceAtSegment (segments.data(), (int) segments.size(), probe,
                                         startBeat + beat),
                    warpSourceAtBeat (markers, offset, fallbackSlope, beat), 1.0e-9,
                    label + ": a stale cursor changes the answer");
    }
}

// ---------------------------------------------------------------------------

static void testNoMarkers()
{
    std::printf ("no markers behave exactly as a single stretch ratio\n");

    for (double stretch : { 0.25, 0.5, 1.0, 1.5, 2.0, 4.0 })
        for (double bpm : { 60.0, 120.0, 174.0 })
        {
            const double slope  = warpFallbackSlope (bpm, stretch);
            const double offset = 1.25;
            std::vector<WarpMarker> none;

            for (double beat = 0.0; beat < 16.0; beat += 0.125)
                checkClose (warpSourceAtBeat (none, offset, slope, beat),
                            offset + beat * slope, 1.0e-12,
                            "unwarped clip does not match its stretch ratio");

            // The clip's length in beats has to come out the same way it did
            // before warping existed, or every existing project moves.
            const double length = 3.7;
            checkClose (warpBeatAtSource (none, offset, slope, offset + length),
                        length * stretch * bpm / 60.0, 1.0e-9,
                        "unwarped clip changes length");

            checkInvariants (none, offset, slope, 8.0, 16.0, "no markers");
        }
}

static void testExactAndPiecewise()
{
    std::printf ("markers are exact, and the audio between them is linear\n");

    // A performance that drifts: the second bar was played slow, the third fast.
    const double offset = 0.0;
    const double bpm = 120.0, spb = 0.5;
    std::vector<WarpMarker> markers {
        { 2.10, 4.0 },     // source 2.10s lands on beat 4
        { 4.30, 8.0 },     // played slow: 2.20s of audio over 4 beats
        { 6.00, 12.0 },    // played fast: 1.70s over 4 beats
    };
    normaliseWarpMarkers (markers, offset);
    check (markers.size() == 3, "a well formed marker set was thinned");

    const double slope = warpFallbackSlope (bpm, 1.0);

    // Halfway between two markers reads halfway through their audio.
    checkClose (warpSourceAtBeat (markers, offset, slope, 6.0), (2.10 + 4.30) / 2.0, 1.0e-12,
                "midpoint between markers is not linear");
    checkClose (warpSourceAtBeat (markers, offset, slope, 10.0), (4.30 + 6.00) / 2.0, 1.0e-12,
                "midpoint between markers is not linear");

    // The head reads back from the first marker at the first segment's rate.
    checkClose (warpSourceAtBeat (markers, offset, slope, 2.0), 2.10 / 2.0, 1.0e-12,
                "head segment does not run at the first segment's rate");

    // The tail carries on at the last segment's rate.
    checkClose (warpSourceAtBeat (markers, offset, slope, 16.0), 6.00 + (6.00 - 4.30), 1.0e-12,
                "tail does not carry on at the last segment's rate");

    // The local stretch, which is what the stretcher is asked for, should be
    // slower than real time where the player was behind and faster where ahead.
    const double slowRatio = (8.0 - 4.0) * spb / (4.30 - 2.10);
    const double fastRatio = (12.0 - 8.0) * spb / (6.00 - 4.30);
    check (slowRatio < 1.0, "the slow bar should be compressed");
    check (fastRatio > 1.0, "the fast bar should be stretched");

    checkInvariants (markers, offset, slope, 0.0, 20.0, "drifting performance");
}

static void testDegenerateMarkers()
{
    std::printf ("markers out of order, stacked, and before the clip start\n");

    const double offset = 2.0;
    const double slope  = warpFallbackSlope (120.0, 1.0);

    // Added in the wrong order: sorting has to put them right, not drop them.
    {
        std::vector<WarpMarker> m { { 5.0, 8.0 }, { 3.0, 4.0 }, { 7.0, 12.0 } };
        normaliseWarpMarkers (m, offset);
        check (m.size() == 3, "sorting dropped a usable marker");
        check (m[0].beat == 4.0 && m[1].beat == 8.0 && m[2].beat == 12.0,
               "markers were not sorted by beat");
        checkInvariants (m, offset, slope, 0.0, 20.0, "out of order");
    }

    // Two markers on exactly the same beat: one has to go, or the segment
    // between them has zero length and the rate is infinite.
    {
        std::vector<WarpMarker> m { { 3.0, 4.0 }, { 5.0, 4.0 } };
        normaliseWarpMarkers (m, offset);
        check (m.size() == 1, "stacked markers were both kept");
        check (m[0].source == 3.0, "the wrong one of two stacked markers survived");
        checkInvariants (m, offset, slope, 0.0, 20.0, "same beat");
    }

    // Two markers on exactly the same source position: keeping both would ask
    // the stretcher to hold one sample across a span of beats.
    {
        std::vector<WarpMarker> m { { 3.0, 4.0 }, { 3.0, 8.0 } };
        normaliseWarpMarkers (m, offset);
        check (m.size() == 1, "two markers on one sample were both kept");
        checkInvariants (m, offset, slope, 0.0, 20.0, "same source");
    }

    // Exactly identical markers, which is what a double click produces.
    {
        std::vector<WarpMarker> m { { 3.0, 4.0 }, { 3.0, 4.0 }, { 3.0, 4.0 } };
        normaliseWarpMarkers (m, offset);
        check (m.size() == 1, "duplicate markers were kept");
        checkInvariants (m, offset, slope, 0.0, 20.0, "duplicates");
    }

    // A marker crossed over its neighbour, which is what dragging one too far
    // does. The earlier one by beat survives and the crossing one is dropped,
    // so the map stays monotonic instead of reading backwards.
    {
        std::vector<WarpMarker> m { { 5.0, 4.0 }, { 3.0, 8.0 } };
        normaliseWarpMarkers (m, offset);
        check (m.size() == 1, "a crossed marker was kept");
        check (m[0].beat == 4.0 && m[0].source == 5.0, "the wrong marker survived a crossing");
        checkInvariants (m, offset, slope, 0.0, 20.0, "crossed");
    }

    // Before the clip's left edge, in either coordinate. The left edge is an
    // implicit marker, so anything at or before it cannot be honoured.
    {
        std::vector<WarpMarker> m { { 1.0, 4.0 },      // source is before the offset
                                    { 4.0, -2.0 },     // beat is before the clip
                                    { 5.0, 8.0 } };
        normaliseWarpMarkers (m, offset);
        check (m.size() == 1, "markers before the clip start were kept");
        check (m[0].beat == 8.0, "the wrong marker survived");
        checkInvariants (m, offset, slope, 0.0, 20.0, "before the start");
    }

    // A marker exactly on the clip's left edge.
    {
        std::vector<WarpMarker> m { { offset, 0.0 }, { 5.0, 8.0 } };
        normaliseWarpMarkers (m, offset);
        check (m.size() == 1, "a marker on the clip start was kept");
        checkInvariants (m, offset, slope, 0.0, 20.0, "on the start");
    }

    // Not a number, in case a corrupt project file carries one.
    {
        std::vector<WarpMarker> m { { std::nan (""), 4.0 },
                                    { 5.0, std::nan ("") },
                                    { 1.0e308, 1.0e308 },
                                    { 5.0, 8.0 } };
        normaliseWarpMarkers (m, offset);
        for (const auto& k : m)
            check (std::isfinite (k.beat) && std::isfinite (k.source),
                   "a marker that is not a number survived");
        checkInvariants (m, offset, slope, 0.0, 20.0, "not a number");
    }

    // Every marker unusable: the clip has to fall back to its stretch ratio.
    {
        std::vector<WarpMarker> m { { 1.0, -1.0 }, { 0.5, -4.0 } };
        normaliseWarpMarkers (m, offset);
        check (m.empty(), "an unusable marker set was not emptied");
        checkClose (warpSourceAtBeat (m, offset, slope, 4.0), offset + 4.0 * slope, 1.0e-12,
                    "an emptied marker set does not fall back to the stretch ratio");
    }
}

static void testEmptyAfterNormaliseIsUnwarped()
{
    std::printf ("one marker still pins, and the rest of the clip follows it\n");

    const double offset = 0.5;
    const double slope  = warpFallbackSlope (120.0, 1.0);
    std::vector<WarpMarker> m { { 2.5, 4.0 } };   // 2.0s of audio over 4 beats
    normaliseWarpMarkers (m, offset);

    checkClose (warpSourceAtBeat (m, offset, slope, 4.0), 2.5, 1.0e-12, "the single marker is not exact");
    checkClose (warpSourceAtBeat (m, offset, slope, 2.0), 1.5, 1.0e-12, "the head is not linear");
    checkClose (warpSourceAtBeat (m, offset, slope, 8.0), 4.5, 1.0e-12, "the tail does not follow the marker");
    checkInvariants (m, offset, slope, 0.0, 20.0, "one marker");
}

// ---------------------------------------------------------------------------
// Cutting a warped clip in two. Project::split works in the source domain
// through the warp map and then rebases the right hand piece, and the property
// that matters is that the cut is inaudible: every beat after the cut has to
// read from exactly the same place it read from before.

static void checkSplitIsSilent (std::vector<WarpMarker> markers, double offset, double length,
                                double bpm, double stretch, double cutBeats,
                                const std::string& label)
{
    normaliseWarpMarkers (markers, offset);
    const double slope = warpFallbackSlope (bpm, stretch);

    // What Project::split does.
    const double cutSource = warpSourceAtBeat (markers, offset, slope, cutBeats) - offset;
    const double rightStretch = warpStretchForSlope (bpm, warpSlopeAtBeat (markers, offset, slope, cutBeats));
    const double rightOffset  = offset + cutSource;
    const double rightLength  = length - cutSource;
    auto rightMarkers = markers;
    rebaseWarpMarkers (rightMarkers, cutBeats, rightOffset);
    const double rightSlope = warpFallbackSlope (bpm, rightStretch);

    // The two pieces together must read exactly what the original read.
    const double total = warpBeatAtSource (markers, offset, slope, offset + length);
    for (int i = 0; i <= 2000; ++i)
    {
        const double beat = cutBeats + i * (std::max (0.0, total - cutBeats) / 2000.0);
        checkClose (warpSourceAtBeat (rightMarkers, rightOffset, rightSlope, beat - cutBeats),
                    warpSourceAtBeat (markers, offset, slope, beat), 1.0e-9,
                    label + ": the right hand piece of a split reads elsewhere at beat "
                          + std::to_string (beat));
    }

    // The lengths have to add up, in beats as well as in source seconds.
    checkClose (warpBeatAtSource (markers, offset, slope, offset + cutSource)
                  + warpBeatAtSource (rightMarkers, rightOffset, rightSlope, rightOffset + rightLength),
                total, 1.0e-7, label + ": the two pieces of a split do not add up");
}

static void testSplit()
{
    std::printf ("cutting a warped clip in two is inaudible\n");

    const std::vector<WarpMarker> drift { { 2.10, 4.0 }, { 4.30, 8.0 }, { 6.00, 12.0 } };

    // Cuts before the first marker, between markers, exactly on a marker, and
    // past the last one, which is the case with no markers left to inherit.
    for (double cut : { 1.0, 2.0, 4.0, 6.0, 8.0, 10.0, 12.0, 13.5, 15.0 })
        checkSplitIsSilent (drift, 0.0, 7.5, 120.0, 1.0, cut,
                            "cut at beat " + std::to_string (cut));

    // And with an offset, an odd tempo and a stretch ratio in play.
    for (double cut : { 0.5, 3.0, 7.0, 11.0, 14.0 })
        checkSplitIsSilent (drift, 0.0, 7.5, 174.0, 1.7, cut,
                            "cut at 174bpm at beat " + std::to_string (cut));

    // An unwarped clip must split exactly where it always did.
    for (double cut : { 1.0, 4.0, 7.0 })
        checkSplitIsSilent ({}, 1.25, 4.0, 140.0, 0.8, cut,
                            "unwarped cut at beat " + std::to_string (cut));

    // Random cuts of random warped clips.
    std::mt19937 rng (414243);
    std::uniform_real_distribution<double> sourceOf (0.05, 12.0);
    std::uniform_real_distribution<double> beatOf (0.05, 24.0);
    std::uniform_int_distribution<int> countOf (1, 8);

    for (int trial = 0; trial < 300; ++trial)
    {
        std::vector<WarpMarker> m;
        for (int i = 0, n = countOf (rng); i < n; ++i)
            m.push_back ({ sourceOf (rng), beatOf (rng) });

        normaliseWarpMarkers (m, 0.0);
        if (m.empty())
            continue;

        const double slope = warpFallbackSlope (120.0, 1.0);
        const double length = 14.0;
        const double total = warpBeatAtSource (m, 0.0, slope, length);
        if (! (total > 0.2))
            continue;

        std::uniform_real_distribution<double> cutOf (0.01, total - 0.01);
        checkSplitIsSilent (m, 0.0, length, 120.0, 1.0, cutOf (rng),
                            "random split " + std::to_string (trial));
    }
}

// ---------------------------------------------------------------------------
// Reading a copy that Rubber Band has already stretched.
//
// The overall tempo match still goes through the stretcher, so it keeps its
// pitch, and the markers only describe the drift left over. The segment table
// is scaled into the stretched copy's time base, and the property that matters
// is that the result is the same audio: a read position in the copy divided by
// the stretch ratio is the read position in the original file.

static void testSourceScale()
{
    std::printf ("markers read a pre-stretched copy at the same places\n");

    std::vector<WarpMarker> m { { 2.10, 4.0 }, { 4.30, 8.0 }, { 6.00, 12.0 } };
    normaliseWarpMarkers (m, 0.25);

    for (double stretch : { 0.25, 0.5, 1.0, 1.7, 2.0, 4.0 })
    {
        const double offset = 0.25;
        const double slope  = warpFallbackSlope (120.0, stretch);

        std::vector<WarpSegment> raw, scaled;
        buildWarpSegments (m, offset, slope, 3.0, raw, 1.0);
        buildWarpSegments (m, offset, slope, 3.0, scaled, stretch);

        check (raw.size() == scaled.size(), "scaling changed the number of segments");

        int c1 = 0, c2 = 0;
        for (int i = 0; i <= 2000; ++i)
        {
            const double beat = 3.0 + i * (20.0 / 2000.0);
            const double a = warpSourceAtSegment (raw.data(), (int) raw.size(), c1, beat);
            const double b = warpSourceAtSegment (scaled.data(), (int) scaled.size(), c2, beat);

            // Reading the stretched copy at b is reading the original at b/stretch.
            checkClose (b / stretch, a, 1.0e-9 * std::max (1.0, std::abs (a)),
                        "a scaled segment table reads the wrong part of the file");
        }

        // A clip whose markers happen to sit exactly on the uniform line should
        // read the stretched copy at exactly real time, so nothing is repitched
        // and the result is precisely what Rubber Band produced.
        std::vector<WarpMarker> uniform;
        for (int k = 1; k <= 4; ++k)
            uniform.push_back ({ offset + k * 4.0 * slope, k * 4.0 });
        normaliseWarpMarkers (uniform, offset);

        std::vector<WarpSegment> flat;
        buildWarpSegments (uniform, offset, slope, 0.0, flat, stretch);
        for (const auto& seg : flat)
            checkClose (seg.slope, 60.0 / 120.0, 1.0e-9,
                        "an undrifted warp does not read a stretched copy at real time");
    }
}

static void testRandom()
{
    std::printf ("ten thousand random marker sets\n");

    std::mt19937 rng (20261004);
    std::uniform_int_distribution<int> countOf (0, 24);
    std::uniform_real_distribution<double> beatOf (-4.0, 40.0);
    std::uniform_real_distribution<double> sourceOf (-2.0, 20.0);
    std::uniform_real_distribution<double> offsetOf (0.0, 4.0);
    std::uniform_real_distribution<double> stretchOf (0.25, 4.0);
    std::uniform_real_distribution<double> bpmOf (40.0, 220.0);
    std::uniform_int_distribution<int> coarse (0, 3);

    for (int trial = 0; trial < 10000; ++trial)
    {
        const double offset = offsetOf (rng);
        const double slope  = warpFallbackSlope (bpmOf (rng), stretchOf (rng));

        std::vector<WarpMarker> m;
        const int n = countOf (rng);
        for (int i = 0; i < n; ++i)
        {
            // A coarse grid every so often, so stacked and crossed markers come
            // up often rather than only by luck.
            if (coarse (rng) == 0)
                m.push_back ({ std::round (sourceOf (rng) * 2.0) / 2.0,
                               std::round (beatOf (rng)) });
            else
                m.push_back ({ sourceOf (rng), beatOf (rng) });
        }

        normaliseWarpMarkers (m, offset);

        // Cheap version of the full invariant check, over the whole clip.
        std::vector<WarpSegment> segments;
        buildWarpSegments (m, offset, slope, 0.0, segments);

        double prev = -1.0e300;
        int cursor = 0;
        for (int i = 0; i <= 600; ++i)
        {
            const double beat = i * (44.0 / 600.0);
            const double here = warpSourceAtBeat (m, offset, slope, beat);
            if (! (here > prev))
            {
                fail ("random trial " + std::to_string (trial) + " is not monotonic at beat "
                      + std::to_string (beat));
                break;
            }
            const double fromSegments = warpSourceAtSegment (segments.data(), (int) segments.size(),
                                                             cursor, beat);
            if (! (std::abs (fromSegments - here) <= 1.0e-9 * std::max (1.0, std::abs (here))))
            {
                fail ("random trial " + std::to_string (trial) + " segment table disagrees at beat "
                      + std::to_string (beat));
                break;
            }
            const double back = warpBeatAtSource (m, offset, slope, here);
            if (! (std::abs (back - beat) <= 1.0e-6 * std::max (1.0, std::abs (beat))))
            {
                fail ("random trial " + std::to_string (trial) + " round trip fails at beat "
                      + std::to_string (beat));
                break;
            }
            prev = here;
        }

        for (const auto& k : m)
            if (std::abs (warpSourceAtBeat (m, offset, slope, k.beat) - k.source) > 1.0e-9)
            {
                fail ("random trial " + std::to_string (trial) + " is not exact at a marker");
                break;
            }
    }
}

static void testTempoIndependence()
{
    std::printf ("markers hold their grid position at any tempo\n");

    // The whole point: a marker pins to a beat, so changing the project tempo
    // must not move which part of the audio lands on that beat.
    const double offset = 0.0;
    std::vector<WarpMarker> m { { 2.1, 4.0 }, { 4.3, 8.0 }, { 6.0, 12.0 } };
    normaliseWarpMarkers (m, offset);

    for (double bpm : { 60.0, 90.0, 120.0, 174.0, 200.0 })
    {
        const double slope = warpFallbackSlope (bpm, 1.0);
        for (double beat = 0.0; beat <= 12.0; beat += 0.25)
            checkClose (warpSourceAtBeat (m, offset, slope, beat),
                        warpSourceAtBeat (m, offset, warpFallbackSlope (120.0, 1.0), beat),
                        1.0e-12, "the tempo moved a warped read position");
    }
}

int main()
{
    std::printf ("warp map\n");
    testNoMarkers();
    testExactAndPiecewise();
    testDegenerateMarkers();
    testEmptyAfterNormaliseIsUnwarped();
    testTempoIndependence();
    testSplit();
    testSourceScale();
    testRandom();

    if (failures == 0)
        std::printf ("all warp map checks passed\n");
    else
        std::printf ("%d warp map checks FAILED\n", failures);

    return failures == 0 ? 0 : 1;
}
