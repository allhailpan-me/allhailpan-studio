// ---------------------------------------------------------------------------
// Checks the automation curve: the beat to value mapping a drawn curve
// describes, and the bend that shapes each of its segments.
//
// AutomationCurve.h has no JUCE in it precisely so this can be built and run
// on its own, with sanitizers, in a few seconds. CI runs it on every push, and
// ./Tests/run.sh runs it the same way here.
//
// This is arithmetic that fails quietly rather than loudly. A curve that
// overshoots by a percent still sounds like a sweep; it just takes a filter
// somewhere it was never drawn to go, and the producer hears a resonance peak
// they did not ask for and blames the plugin. A curve that is a hair off at a
// point looks right and plays wrong. So the properties are checked over dense
// sweeps and over tens of thousands of randomly generated curves, including
// the lists a mouse actually produces: points dropped on top of each other,
// points added in the wrong order, and bends held at the ends of their travel.
//
// The properties, in the order they are checked below:
//
//   exact      the value at a point is the value drawn there, for every bend
//   bounded    a segment never leaves the range of the two values it joins
//   monotonic  a segment that rises never falls on the way, and vice versa
//   continuous no step anywhere except where two points share a beat
//   clamped    before the first point and after the last, the curve holds
//   stable     normalising sorts, clamps, and drops nothing it should keep
// ---------------------------------------------------------------------------

#include "AutomationCurve.h"

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

// The bends worth checking every property at: straight, gentle either way,
// and held hard against both ends of the travel, where the shape function is
// closest to degenerating.
static const double kBends[] = { 0.0, 0.25, -0.25, 0.75, -0.75, 1.0, -1.0,
                                 // past the ends, as a damaged project file
                                 // would hand them over
                                 2.5, -2.5 };

// ---------------------------------------------------------------------------
// The shape function on its own, before any point list is involved.

static void testShape()
{
    std::printf ("the segment shape\n");

    for (double bend : kBends)
    {
        const std::string b = "bend " + std::to_string (bend);

        // Exact at both ends is what makes a point land on its own value
        // however its neighbour is bent.
        checkClose (autoCurveShape (0.0, bend), 0.0, 0.0, b + ": does not start at zero");
        checkClose (autoCurveShape (1.0, bend), 1.0, 0.0, b + ": does not end at one");

        // Outside the segment too, since a rounding error can put t a hair
        // either side of its range.
        checkClose (autoCurveShape (-0.5, bend), 0.0, 0.0, b + ": reads below zero before the start");
        checkClose (autoCurveShape (1.5, bend), 1.0, 0.0, b + ": reads past one after the end");

        double previous = 0.0;
        for (int i = 1; i <= 20000; ++i)
        {
            const double t = i / 20000.0;
            const double w = autoCurveShape (t, bend);

            check (w >= 0.0 && w <= 1.0, b + ": left 0..1 at t = " + std::to_string (t));
            check (w >= previous, b + ": fell back at t = " + std::to_string (t));
            previous = w;
        }

        // Strictly increasing, not merely non decreasing: a flat stretch
        // inside a segment would be a bend that had collapsed into a hold.
        check (autoCurveShape (0.5, bend) > autoCurveShape (0.25, bend),
               b + ": the middle of the segment is flat");
    }

    // A bend of zero is a straight line, exactly, because that is the case
    // nearly every curve is made of and it must not be approximately linear.
    for (int i = 0; i <= 1000; ++i)
    {
        const double t = i / 1000.0;
        checkClose (autoCurveShape (t, 0.0), t, 1.0e-15, "no bend is not a straight line");
    }

    // Mirror image either way. The two directions are inverse functions, so
    // reading one at t and the other at its own output must come back to t.
    for (double bend : { 0.1, 0.4, 0.8, 1.0 })
        for (int i = 0; i <= 500; ++i)
        {
            const double t = i / 500.0;
            checkClose (autoCurveShape (autoCurveShape (t, bend), -bend), t, 1.0e-9,
                        "bending one way then the other is not a mirror image");
        }

    // Which way is which. Positive holds the value near the point it is
    // leaving, so it must sit below the straight line, and negative above.
    check (autoCurveShape (0.5, 0.75) < 0.5, "a positive bend does not hold back");
    check (autoCurveShape (0.5, -0.75) > 0.5, "a negative bend does not run ahead");

    // At full travel the midpoint has moved a thirty-second of the way, which
    // is the documented limit. Checked so that changing the limit has to be a
    // decision rather than an accident.
    checkClose (autoCurveShape (0.5, 1.0), 1.0 / 32.0, 1.0e-12,
                "full bend is not as steep as documented");
    checkClose (autoCurveShape (0.5, -1.0), 31.0 / 32.0, 1.0e-12,
                "full bend the other way is not as steep as documented");

    // A NaN must come out as a defined value rather than propagating into the
    // parameter write. A project file is not to be trusted this far.
    checkClose (autoCurveShape (std::nan (""), 0.0), 0.0, 0.0, "a NaN position is not contained");
    check (std::isfinite (autoCurveShape (0.5, std::nan (""))), "a NaN bend is not contained");
}

// ---------------------------------------------------------------------------
// The properties every normalised curve has to satisfy, whatever it is.

static void checkInvariants (const std::vector<AutoCurvePoint>& points, const std::string& label)
{
    if (points.empty())
    {
        checkClose (autoCurveValueAt (points, 3.0, 0.42), 0.42, 0.0,
                    label + ": an empty curve does not read its fallback");
        return;
    }

    // Sorted, in range: what normalisation promises.
    for (size_t i = 1; i < points.size(); ++i)
        check (points[i].beat >= points[i - 1].beat, label + ": not sorted by beat");
    for (auto& p : points)
    {
        check (p.value >= 0.0 && p.value <= 1.0, label + ": a value is out of range");
        check (p.bend >= -1.0 && p.bend <= 1.0, label + ": a bend is out of range");
        check (std::isfinite (p.beat), label + ": a beat is not finite");
    }

    // Exact at every point. This is the one a producer notices: a point is a
    // promise that the parameter is at this value on this beat. Where points
    // share a beat the last of them is the promise, since the earlier ones
    // have been stepped past.
    for (size_t i = 0; i < points.size(); ++i)
    {
        const bool lastAtThisBeat = (i + 1 == points.size()) || points[i + 1].beat > points[i].beat;
        if (! lastAtThisBeat)
            continue;

        checkClose (autoCurveValueAt (points, points[i].beat), points[i].value, 1.0e-12,
                    label + ": point " + std::to_string (i) + " is not exact");
    }

    // Held flat outside the drawn span, at the edge and far beyond it.
    for (double away : { 1.0e-9, 0.5, 4.0, 1.0e6 })
    {
        checkClose (autoCurveValueAt (points, points.front().beat - away), points.front().value,
                    0.0, label + ": does not hold before the first point");
        checkClose (autoCurveValueAt (points, points.back().beat + away), points.back().value,
                    0.0, label + ": does not hold after the last point");
    }
    checkClose (autoCurveValueAt (points, points.back().beat), points.back().value, 0.0,
                label + ": the last point is not exact");

    // Continuity, checked where it can actually break: at the joins. Inside a
    // segment the shape is a continuous function of the beat, so the only
    // place a jump can appear is where one segment hands over to the next, and
    // that is where getting the segment lookup wrong would show.
    //
    // The approach from the left is measured a span-relative hair before the
    // join rather than a fixed distance, so the tolerance can be derived
    // rather than guessed. The steepest the shape gets is s + 1 in position
    // per unit of time, with s bounded by the bend limit, so coming in from
    // u of the way back leaves at most |rise| * (s + 1) * u to go.
    const double steepest = 1.0 / kMinAutoBendBias;   // s + 1 at full bend, which is 32
    for (size_t i = 1; i < points.size(); ++i)
    {
        const double span = points[i].beat - points[i - 1].beat;
        if (! (span > 0.0))
            continue;    // a shared beat is a step on purpose, checked separately

        const double u = 1.0e-7;
        const double rise = std::abs (points[i].value - points[i - 1].value);
        const double left = autoCurveValueAt (points, points[i].beat - span * u);

        // What the segment before is heading for is the first point on that
        // beat, which is the one the segment actually ends at. Where several
        // share the beat, the later ones are the step.
        size_t firstAtBeat = i;
        while (firstAtBeat > 0 && points[firstAtBeat - 1].beat == points[i].beat)
            --firstAtBeat;

        checkClose (left, points[firstAtBeat].value, rise * steepest * u + 1.0e-12,
                    label + ": a step appeared at the join before point " + std::to_string (i));
    }

    // In range, bounded by its own segment, and monotonic within it. Swept
    // densely rather than at a few points, because a bend going wrong goes
    // wrong in the middle of a segment where no point pins it.
    const double from = points.front().beat - 1.0;
    const double to   = points.back().beat + 1.0;
    const int    steps = 2500;

    const auto segmentFor = [&points] (double beat)
    {
        return std::upper_bound (points.begin(), points.end(), beat,
                                 [] (double b, const AutoCurvePoint& p) { return b < p.beat; });
    };

    double previous     = autoCurveValueAt (points, from);
    auto   previousSeg  = segmentFor (from);

    for (int i = 1; i <= steps; ++i)
    {
        const double beat = from + (to - from) * i / steps;
        const double v    = autoCurveValueAt (points, beat);
        const auto   hi   = segmentFor (beat);

        check (v >= 0.0 && v <= 1.0, label + ": left 0..1 at beat " + std::to_string (beat));

        if (hi != points.begin() && hi != points.end())
        {
            const auto lo = hi - 1;
            const double low  = std::min (lo->value, hi->value);
            const double high = std::max (lo->value, hi->value);

            // No overshoot. The whole reason the bend warps time rather than
            // value: a bent segment stays between the two values it joins.
            check (v >= low - 1.0e-12 && v <= high + 1.0e-12,
                   label + ": overshot its segment at beat " + std::to_string (beat));

            // Monotonic in the segment's own direction. Only compared against
            // a reading from the same segment, since two consecutive readings
            // either side of a point say nothing about either segment's shape.
            if (hi == previousSeg)
            {
                const double step = v - previous;
                if (hi->value > lo->value)
                    check (step >= -1.0e-12, label + ": a rising segment fell at beat " + std::to_string (beat));
                else if (hi->value < lo->value)
                    check (step <= 1.0e-12, label + ": a falling segment rose at beat " + std::to_string (beat));
                else
                    checkClose (v, lo->value, 1.0e-12,
                                label + ": a flat segment moved at beat " + std::to_string (beat));
            }
        }

        previous    = v;
        previousSeg = hi;
    }
}

static void testHandPicked()
{
    std::printf ("curves worth naming\n");

    checkInvariants ({}, "empty");
    checkInvariants ({ { 4.0, 0.3, 0.0 } }, "one point");
    checkInvariants ({ { 0.0, 0.0, 0.0 }, { 8.0, 1.0, 0.0 } }, "a straight rise");
    checkInvariants ({ { 0.0, 1.0, 0.9 }, { 8.0, 0.0, -0.9 } }, "a bent fall");
    checkInvariants ({ { 0.0, 0.2, 1.0 }, { 4.0, 0.9, -1.0 }, { 6.0, 0.9, 0.0 }, { 16.0, 0.1, 0.5 } },
                     "a four point sweep with both bends at full travel");

    // A single point reads as a flat offset everywhere, which is how a curve
    // is used to park a parameter somewhere for a section.
    const std::vector<AutoCurvePoint> one { { 4.0, 0.7, 0.0 } };
    for (double beat : { -100.0, 0.0, 4.0, 4.5, 1000.0 })
        checkClose (autoCurveValueAt (one, beat), 0.7, 0.0, "one point is not flat everywhere");

    // The bend on the last point shapes nothing, so changing it must not
    // change a single reading. If it ever does, the segment lookup is reading
    // the wrong end of the segment.
    std::vector<AutoCurvePoint> a { { 0.0, 0.0, 0.3 }, { 8.0, 1.0, 0.0 } };
    std::vector<AutoCurvePoint> b { { 0.0, 0.0, 0.3 }, { 8.0, 1.0, -1.0 } };
    for (int i = 0; i <= 2000; ++i)
    {
        const double beat = -1.0 + 10.0 * i / 2000.0;
        checkClose (autoCurveValueAt (a, beat), autoCurveValueAt (b, beat), 0.0,
                    "the last point's bend changed the curve");
    }

    // The flat case, which is the one that would hide a bend applied to the
    // value instead of to time: bending a segment between two equal values
    // cannot move it.
    std::vector<AutoCurvePoint> flat { { 0.0, 0.55, 1.0 }, { 8.0, 0.55, 0.0 } };
    for (int i = 0; i <= 2000; ++i)
        checkClose (autoCurveValueAt (flat, 8.0 * i / 2000.0), 0.55, 1.0e-15,
                    "a bend moved a flat segment");
}

static void testSharedBeats()
{
    std::printf ("points sharing a beat\n");

    // Two points at one beat is how an instant jump is drawn. The last one
    // wins from that beat on, and the value just before it is the one the
    // segment before was heading for.
    std::vector<AutoCurvePoint> step { { 0.0, 0.0, 0.0 }, { 4.0, 1.0, 0.0 },
                                       { 4.0, 0.0, 0.0 }, { 8.0, 0.5, 0.0 } };
    normaliseAutoCurve (step);
    check (step.size() == 4, "normalising dropped a point stacked on a beat");

    checkClose (autoCurveValueAt (step, 4.0), 0.0, 1.0e-12, "the step did not land on the beat");
    checkClose (autoCurveValueAt (step, 3.999999), 1.0, 1.0e-5, "the approach to the step is wrong");
    checkClose (autoCurveValueAt (step, 6.0), 0.25, 1.0e-12, "the segment after the step is wrong");
    checkInvariants (step, "a step");

    // Three on one beat, which a double click produces. Still the last.
    std::vector<AutoCurvePoint> three { { 0.0, 0.1, 0.0 }, { 4.0, 0.2, 0.0 },
                                        { 4.0, 0.3, 0.0 }, { 4.0, 0.4, 0.0 },
                                        { 9.0, 0.9, 0.0 } };
    normaliseAutoCurve (three);
    checkClose (autoCurveValueAt (three, 4.0), 0.4, 1.0e-12, "three on a beat did not read the last");
    checkInvariants (three, "three on a beat");

    // Every point on the same beat, which is a curve with nowhere to go. It
    // must read as a step rather than dividing by a zero span: the first of
    // the stack on the approach, the last of it from the beat onwards, which
    // is the same rule as every other stacked beat above rather than a
    // special case for this one.
    std::vector<AutoCurvePoint> all { { 2.0, 0.1, 0.0 }, { 2.0, 0.6, 0.0 }, { 2.0, 0.3, 0.0 } };
    normaliseAutoCurve (all);
    checkClose (autoCurveValueAt (all, -5.0), 0.1, 0.0, "a curve with no span misreads its approach");
    checkClose (autoCurveValueAt (all,  2.0), 0.3, 0.0, "a curve with no span misreads its own beat");
    checkClose (autoCurveValueAt (all,  5.0), 0.3, 0.0, "a curve with no span misreads afterwards");
}

static void testNormalise()
{
    std::printf ("normalising a point list\n");

    // Points added out of order, which is what drawing right to left does.
    std::vector<AutoCurvePoint> shuffled { { 8.0, 0.8, 0.0 }, { 0.0, 0.1, 0.5 },
                                           { 16.0, 0.2, 0.0 }, { 4.0, 0.4, -0.5 } };
    auto sorted = shuffled;
    normaliseAutoCurve (sorted);

    check (sorted.size() == shuffled.size(), "normalising lost a point");
    check (sorted[0].beat == 0.0 && sorted[1].beat == 4.0
           && sorted[2].beat == 8.0 && sorted[3].beat == 16.0, "normalising did not sort");

    // The bend travels with its own point, not with the position it ended up
    // in. Getting this wrong would shape the wrong segments.
    checkClose (sorted[0].bend, 0.5, 0.0, "a bend was left behind by the sort");
    checkClose (sorted[1].bend, -0.5, 0.0, "a bend was left behind by the sort");
    checkInvariants (sorted, "sorted from shuffled");

    // Order within one beat is the order given, so a point drawn on top of
    // another lands after it rather than somewhere decided by the sort, and
    // which of them the curve reads at that beat is therefore decided by the
    // drawing rather than by the standard library.
    //
    // The list is long enough on purpose. An unstable sort still happens to
    // preserve order on a handful of elements, because the implementation
    // falls back to an insertion sort below a threshold, so a short list
    // cannot tell a stable sort from an unstable one.
    std::vector<AutoCurvePoint> stacked;
    for (int beat = 0; beat < 16; ++beat)
        for (int k = 0; k < 8; ++k)
            stacked.push_back ({ (double) (15 - beat), 0.05 + 0.1 * k, 0.0 } );

    auto stackedSorted = stacked;
    normaliseAutoCurve (stackedSorted);

    check (stackedSorted.size() == stacked.size(), "normalising lost a stacked point");
    for (size_t i = 0; i < stackedSorted.size(); ++i)
    {
        // Beats ascending, and within each beat the eight values in the order
        // they were given.
        const double wantBeat  = (double) (i / 8);
        const double wantValue = 0.05 + 0.1 * (double) (i % 8);
        checkClose (stackedSorted[i].beat, wantBeat, 0.0,
                    "the sort put a stacked point on the wrong beat");
        checkClose (stackedSorted[i].value, wantValue, 1.0e-15,
                    "the sort reordered points sharing a beat");
    }

    // Out of range values and bends are brought in rather than refused: an
    // older or damaged file should load as something playable.
    std::vector<AutoCurvePoint> wild { { 0.0, -3.0, -9.0 }, { 4.0, 7.0, 9.0 } };
    normaliseAutoCurve (wild);
    checkClose (wild[0].value, 0.0, 0.0, "a value below range was not clamped");
    checkClose (wild[1].value, 1.0, 0.0, "a value above range was not clamped");
    checkClose (wild[0].bend, -1.0, 0.0, "a bend below range was not clamped");
    checkClose (wild[1].bend, 1.0, 0.0, "a bend above range was not clamped");

    // Anything that is not a number goes, because sorting a list with a NaN
    // in it is undefined behaviour and the sanitizers would be right to say so.
    std::vector<AutoCurvePoint> nasty { { 0.0, 0.5, 0.0 },
                                        { std::nan (""), 0.5, 0.0 },
                                        { 4.0, std::nan (""), 0.0 },
                                        { 6.0, 0.5, std::nan ("") },
                                        { std::numeric_limits<double>::infinity(), 0.5, 0.0 },
                                        { 8.0, 0.5, 0.0 } };
    normaliseAutoCurve (nasty);
    check (nasty.size() == 2, "normalising kept a point that is not a number");
    checkInvariants (nasty, "after dropping the nonsense");

    // The cap, so that a damaged file cannot hand the audio thread any length
    // of list it likes.
    std::vector<AutoCurvePoint> many;
    for (size_t i = 0; i < kMaxAutoCurvePoints + 500; ++i)
        many.push_back ({ (double) i, 0.5, 0.0 });
    normaliseAutoCurve (many);
    check (many.size() == kMaxAutoCurvePoints, "the point cap was not applied");

    // Normalising twice is the same as once, which is what lets every edit
    // path call it without having to know whether somebody already did.
    auto once = shuffled;
    normaliseAutoCurve (once);
    auto twice = once;
    normaliseAutoCurve (twice);
    check (once == twice, "normalising is not idempotent");

    // And the last beat, which is what the interface sizes a curve by.
    checkClose (autoCurveLastBeat (once), 16.0, 0.0, "the last beat is wrong");
    checkClose (autoCurveLastBeat ({}), 0.0, 0.0, "an empty curve has a last beat");
}

static void testRandom()
{
    std::printf ("randomly generated curves\n");

    // Hand picked cases check what was thought of. These check what was not:
    // every combination of point count, spacing, stacking and bend that the
    // generator can reach, against the properties above.
    std::mt19937 rng (0x9e3779b9);
    std::uniform_int_distribution<int> countOf (0, 9);
    std::uniform_real_distribution<double> beatOf (-4.0, 24.0);
    std::uniform_real_distribution<double> valueOf (0.0, 1.0);
    std::uniform_real_distribution<double> bendOf (-1.3, 1.3);
    std::uniform_int_distribution<int> stackOf (0, 3);

    for (int trial = 0; trial < 1200; ++trial)
    {
        std::vector<AutoCurvePoint> points;
        const int n = countOf (rng);

        for (int i = 0; i < n; ++i)
        {
            // Sometimes on a beat another point already uses, which is the
            // case the lookup has to get right and the one a mouse produces.
            const double beat = (! points.empty() && stackOf (rng) == 0)
                                  ? points[(size_t) (rng() % points.size())].beat
                                  : beatOf (rng);
            points.push_back ({ beat, valueOf (rng), bendOf (rng) });
        }

        normaliseAutoCurve (points);
        checkInvariants (points, "random curve " + std::to_string (trial));

        if (failures > 20)
        {
            std::printf ("  (stopping early, too many failures)\n");
            return;
        }
    }
}

static void testFarOut()
{
    std::printf ("curves far out in the arrangement\n");

    // A curve at bar four thousand, where a beat is a large number and the
    // difference between two of them is a small one. The division that finds
    // the position within a segment is where that loses precision, so the
    // exactness at the points is checked out there too.
    for (double origin : { 0.0, 1.0e3, 1.0e5, 1.0e7 })
    {
        std::vector<AutoCurvePoint> points { { origin, 0.2, 0.7 },
                                             { origin + 0.25, 0.9, -0.7 },
                                             { origin + 64.0, 0.1, 0.0 } };
        normaliseAutoCurve (points);

        for (auto& p : points)
            checkClose (autoCurveValueAt (points, p.beat), p.value, 1.0e-12,
                        "a point far out in the arrangement is not exact");

        // And still bounded and in range across the whole span.
        for (int i = 0; i <= 5000; ++i)
        {
            const double beat = origin + 64.0 * i / 5000.0;
            const double v = autoCurveValueAt (points, beat);
            check (v >= 0.0 && v <= 1.0, "a far out curve left 0..1");
        }
    }
}

// ---------------------------------------------------------------------------
// What a curve reading means for each kind of target.
//
// This is the quietest arithmetic in the feature. A fader whose automation
// cannot reach the top, or a pan whose automation is off centre by a hair,
// produces a mix that is subtly wrong and a number that looks reasonable,
// which is the failure this codebase has been bitten by before.

static void testControlRanges()
{
    std::printf ("the range each kind of target has\n");

    const AutoTargetKind kinds[] = { AutoTargetKind::channelParam, AutoTargetKind::insertVolume,
                                     AutoTargetKind::insertPan,    AutoTargetKind::insertFxParam };

    for (auto kind : kinds)
    {
        const std::string k = "kind " + std::to_string ((int) kind);

        // A curve reading and the control value it means are the same journey
        // in both directions, or a new curve would not start where the control
        // it is aimed at is already sitting.
        for (int i = 0; i <= 1000; ++i)
        {
            const double v = i / 1000.0;
            checkClose (autoCurveValueFor (kind, (double) autoControlValue (kind, v)), v, 1.0e-6,
                        k + ": the round trip through the control range moved the value");
        }

        // The ends of the curve are the ends of the control's travel.
        checkClose (autoCurveValueFor (kind, (double) autoControlValue (kind, 0.0)), 0.0, 1.0e-9,
                    k + ": the bottom of the curve is not the bottom of the travel");
        checkClose (autoCurveValueFor (kind, (double) autoControlValue (kind, 1.0)), 1.0, 1.0e-9,
                    k + ": the top of the curve is not the top of the travel");

        // Nothing out of range survives in either direction, since a damaged
        // project file reaches both of these.
        for (double wild : { -5.0, -1.0e9, 2.0, 1.0e9 })
        {
            const double back = autoCurveValueFor (kind, wild);
            check (back >= 0.0 && back <= 1.0, k + ": an out of range control value was not clamped");
        }

        check (std::isfinite (autoControlValue (kind, std::nan (""))),
               k + ": a NaN reading was not contained");
        checkClose (autoCurveValueFor (kind, std::nan ("")), 0.0, 0.0,
                    k + ": a NaN control value was not contained");
    }

    // The named positions, so that changing a range has to be a decision
    // rather than an accident.
    checkClose ((double) autoControlValue (AutoTargetKind::insertVolume, 1.0),
                (double) kMaxFaderGain, 1.0e-9, "a curve at the top does not reach the top of the fader");
    checkClose ((double) autoControlValue (AutoTargetKind::insertVolume, 0.0),
                0.0, 0.0, "a curve at the bottom does not silence the fader");
    checkClose (autoCurveValueFor (AutoTargetKind::insertVolume, (double) kUnityFaderGain),
                (double) kUnityFaderGain / (double) kMaxFaderGain, 1.0e-9,
                "0 dB is not where the fader's own unity is");

    checkClose ((double) autoControlValue (AutoTargetKind::insertPan, 0.5), 0.0, 1.0e-9,
                "the middle of a pan curve is not centre");
    checkClose ((double) autoControlValue (AutoTargetKind::insertPan, 0.0), -1.0, 1.0e-9,
                "the bottom of a pan curve is not hard left");
    checkClose ((double) autoControlValue (AutoTargetKind::insertPan, 1.0), 1.0, 1.0e-9,
                "the top of a pan curve is not hard right");

    // A plugin parameter is already normalised, so the mapping is the identity
    // and must stay exactly that rather than approximately.
    for (auto kind : { AutoTargetKind::channelParam, AutoTargetKind::insertFxParam })
        for (int i = 0; i <= 1000; ++i)
        {
            const double v = i / 1000.0;
            checkClose ((double) autoControlValue (kind, v), v, 1.0e-7,
                        "a plugin parameter is not passed through unchanged");
        }
}

int main()
{
    std::printf ("automation curve\n");
    testShape();
    testHandPicked();
    testSharedBeats();
    testNormalise();
    testFarOut();
    testControlRanges();
    testRandom();

    if (failures == 0)
        std::printf ("all automation curve checks passed\n");
    else
        std::printf ("%d automation curve checks FAILED\n", failures);

    return failures == 0 ? 0 : 1;
}
