// ---------------------------------------------------------------------------
// Checks the comp: which take is heard where, and what happens at the joins.
//
// Positions here are seconds into the folder, which is the unit CompModel.h
// works in and the reason it survives a tempo change. See the header.
//
// CompModel.h has no JUCE in it so this can be built and run on its own, with
// sanitizers, in a few seconds. CI runs it on every push and ./Tests/run.sh
// runs it the same way here.
//
// Comping is arithmetic that fails quietly. A gap between two spans is a hole
// in the performance that nobody notices until the mix; an overlap is two
// takes of one voice playing at once, which sounds like a chorus effect that
// came from nowhere; a crossfade that does not sum to unity power dips or
// peaks at every join, and a vocal comped from eight passes then has eight
// little dents in it. None of those stop anything, so they are checked
// exhaustively rather than at a few hand picked points, and over thousands of
// randomly generated comps including the degenerate ones a mouse produces:
// regions dragged backwards, dragged onto existing boundaries, dragged to
// nothing, and takes deleted in whatever order the producer felt like.
// ---------------------------------------------------------------------------

#include "CompModel.h"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <string>
#include <utility>

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
// The properties a normalised segment list has to satisfy, whatever it is.

static void checkSegmentInvariants (const std::vector<CompSegment>& segs,
                                    int numTakes, double folderSeconds,
                                    const std::string& label)
{
    check (! segs.empty(), label + ": a comp over a folder with takes is never empty");
    if (segs.empty())
        return;

    checkClose (segs.front().at, 0.0, 0.0,
                label + ": the comp does not open at the folder's start");

    for (size_t i = 0; i < segs.size(); ++i)
    {
        check (segs[i].take >= 0 && segs[i].take < numTakes,
               label + ": segment " + std::to_string (i) + " names a take that is not there");
        check (segs[i].at >= 0.0 && segs[i].at < folderSeconds,
               label + ": segment " + std::to_string (i) + " sits outside the folder");

        if (i > 0)
        {
            check (segs[i].at > segs[i - 1].at,
                   label + ": boundaries are not strictly increasing at " + std::to_string (i));
            check (segs[i].take != segs[i - 1].take,
                   label + ": two neighbouring segments name the same take at " + std::to_string (i));
        }
    }
}

// The spans have to tile the folder exactly. This is the gaps and overlaps
// property: a gap is silence the producer did not ask for, an overlap is two
// performances at once.
static void checkSpansTile (const std::vector<CompSpan>& spans, double folderSeconds,
                            const std::string& label)
{
    check (! spans.empty(), label + ": no spans at all");
    if (spans.empty())
        return;

    checkClose (spans.front().start, 0.0, 0.0, label + ": the first span does not start at zero");
    checkClose (spans.back().end, folderSeconds, 1.0e-12,
                label + ": the last span does not reach the folder's end");

    for (size_t i = 0; i < spans.size(); ++i)
    {
        check (spans[i].end > spans[i].start,
               label + ": span " + std::to_string (i) + " is empty or runs backwards");

        if (i > 0)
            checkClose (spans[i].start, spans[i - 1].end, 0.0,
                        label + ": span " + std::to_string (i) + " does not meet the one before it");
    }
}

// A take is read only where it has audio. Anything else is a read off the end
// of a buffer, which on the audio thread takes the whole studio down.
static void checkReadsInsideTakes (const std::vector<CompSpan>& spans,
                                   const std::vector<CompTakeSpan>& takes,
                                   const std::string& label)
{
    for (size_t i = 0; i < spans.size(); ++i)
    {
        const auto& s = spans[i];
        if (! s.isAudible())
            continue;

        const auto& t = takes[(size_t) s.take];
        check (s.readFrom >= t.start - 1.0e-12,
               label + ": span " + std::to_string (i) + " reads before take " + std::to_string (s.take) + " begins");
        check (s.readTo <= t.end() + 1.0e-12,
               label + ": span " + std::to_string (i) + " reads past the end of take " + std::to_string (s.take));
    }
}

// The windows at two consecutive boundaries must not overlap, or three takes
// sound at once inside one of them and the equal-power identity says nothing
// about what that sums to.
static void checkFadeWindowsDisjoint (const std::vector<CompSpan>& spans, const std::string& label)
{
    for (size_t i = 0; i < spans.size(); ++i)
    {
        const auto f = spans[i].fade();
        check (f.inTo <= f.outFrom + 1.0e-12,
               label + ": the two crossfade windows of span " + std::to_string (i) + " overlap");
    }
}

// A read range is the stretch of a take that is actually played, so it runs
// forwards or it is empty. Left inverted it is a length below zero by the time
// the engine turns it into a clip, and nothing downstream is written expecting
// one. It can only arise where a take's audio misses its span entirely, which
// the randomised cases below produce often.
static void checkReadRangesOrdered (const std::vector<CompSpan>& spans, const std::string& label)
{
    for (size_t i = 0; i < spans.size(); ++i)
        if (! (spans[i].readTo >= spans[i].readFrom))
            fail (label + ": the read range of span " + std::to_string (i) + " runs backwards");
}

// A join is as wide as the crossfade the folder asked for, and narrower only
// where a span beside it is too short to hold one.
//
// Worth stating because the width was the one thing about a join that nothing
// here measured. Every identity above is scale free: doubling every crossfade
// leaves the equal-power sum at unity, the windows disjoint and the reads in
// range, so a folder asking for ten milliseconds could have been given twenty
// at every join with nothing to say so.
static void checkJoinWidths (const std::vector<CompSpan>& spans, double crossfade,
                             const std::string& label)
{
    for (size_t i = 0; i < spans.size(); ++i)
    {
        const auto& s = spans[i];

        if (! (s.fadeIn * 2.0 <= crossfade + 1.0e-12))
            fail (label + ": the join at the start of span " + std::to_string (i)
                        + " is wider than the crossfade asked for");
        if (! (s.fadeOut * 2.0 <= crossfade + 1.0e-12))
            fail (label + ": the join at the end of span " + std::to_string (i)
                        + " is wider than the crossfade asked for");

        // Where both spans either side of a join are longer than the whole
        // crossfade, nothing is narrowing it and it has to be exactly what was
        // asked for.
        if (i > 0 && spans[i - 1].end - spans[i - 1].start > crossfade
                  && s.end - s.start > crossfade
                  && ! (std::abs (s.fadeIn * 2.0 - crossfade) <= 1.0e-9))
            fail (label + ": the join before span " + std::to_string (i) + " is "
                        + std::to_string (s.fadeIn * 2.0) + " wide, not the "
                        + std::to_string (crossfade) + " it was asked for");
    }
}

// `compTakeAt` and `buildCompSpans` are two copies of one decision: which take
// is heard at a point. The engine reads the spans and the playlist draws from
// compTakeAt, so a disagreement between them is a comp that plays one thing
// and is drawn as another. A second copy of an answer is a second thing that
// can be wrong, which this project has met often enough to check rather than
// assume.
static void checkSpansAgreeWithTakeAt (const std::vector<CompSpan>& spans,
                                       const std::vector<CompSegment>& segs,
                                       const std::string& label)
{
    for (size_t i = 0; i < spans.size(); ++i)
    {
        const double mid = 0.5 * (spans[i].start + spans[i].end);
        if (compTakeAt (segs, mid) != spans[i].take)
            fail (label + ": span " + std::to_string (i) + " plays take "
                        + std::to_string (spans[i].take) + " where compTakeAt reads "
                        + std::to_string (compTakeAt (segs, mid)));
    }
}

int main()
{
    std::printf ("CompModel\n");

    // -----------------------------------------------------------------------
    // The equal-power law itself. This is the one number a producer hears: a
    // join that dips is a dent in the vocal, a join that peaks is a bump, and
    // a linear fade gives a dip of about three decibels in the middle of
    // every single one.
    {
        for (int i = 0; i <= 2000; ++i)
        {
            const double t   = i / 2000.0;
            const double in  = compFadeInGain (t);
            const double out = compFadeOutGain (t);
            checkClose (in * in + out * out, 1.0, 1.0e-12,
                        "equal power law does not hold at t = " + std::to_string (t));
        }

        checkClose (compFadeInGain (0.0),  0.0, 1.0e-15, "a fade in does not start from silence");
        checkClose (compFadeInGain (1.0),  1.0, 1.0e-15, "a fade in does not reach unity");
        checkClose (compFadeOutGain (0.0), 1.0, 1.0e-15, "a fade out does not start at unity");
        checkClose (compFadeOutGain (1.0), 0.0, 1.0e-15, "a fade out does not reach silence");

        // The midpoint is where a linear crossfade loses its 3dB. Equal power
        // puts both gains at 1/sqrt(2), so the powers are a half each.
        checkClose (compFadeInGain (0.5),  std::sqrt (0.5), 1.0e-15, "midpoint fade in is not 1/sqrt(2)");
        checkClose (compFadeOutGain (0.5), std::sqrt (0.5), 1.0e-15, "midpoint fade out is not 1/sqrt(2)");

        // Out of range arguments are clamped rather than left to run off the
        // ends of the quarter cycle, because the audio thread evaluates these
        // from a position in the folder, and that position is not to be trusted.
        checkClose (compFadeInGain (-5.0),  0.0, 1.0e-15, "a fade in below zero is not clamped");
        checkClose (compFadeOutGain (9.0),  0.0, 1.0e-15, "a fade out above one is not clamped");
    }

    // -----------------------------------------------------------------------
    // An uncomped folder: one take, no joins, the whole folder.
    {
        const double len = 32.0;
        std::vector<CompSegment> segs;
        normaliseCompSegments (segs, 1, len);
        checkSegmentInvariants (segs, 1, len, "fresh folder");
        check (segs.size() == 1, "a fresh folder has more than one segment");

        const std::vector<CompTakeSpan> takes { { 0.0, len } };
        const auto spans = buildCompSpans (segs, takes, len, 0.02);
        checkSpansTile (spans, len, "fresh folder");
        check (spans.size() == 1, "a fresh folder plays as more than one span");
        check (spans[0].fadeIn == 0.0 && spans[0].fadeOut == 0.0,
               "a folder with no joins still has a crossfade in it");

        // The clip's own edges never fade. A comp that faded in at the start
        // would swallow the first syllable of the performance.
        checkClose (compFadeGain (spans[0].fade(), 0.0), 1.0, 0.0, "the folder's start is faded in");
        checkClose (compFadeGain (spans[0].fade(), len), 1.0, 0.0, "the folder's end is faded out");
    }

    // -----------------------------------------------------------------------
    // No takes at all, and a clip with no length. Both come up: a folder whose
    // last take was deleted, and a folder trimmed to nothing mid drag.
    {
        std::vector<CompSegment> segs { { 0.0, 0 }, { 4.0, 1 } };
        normaliseCompSegments (segs, 0, 16.0);
        check (segs.empty(), "a comp survived having no takes to point at");

        segs = { { 0.0, 0 }, { 4.0, 1 } };
        normaliseCompSegments (segs, 2, 0.0);
        check (segs.empty(), "a comp survived its folder having no length");

        const auto spans = buildCompSpans ({ { 0.0, 0 } }, {}, 16.0, 0.02);
        check (spans.empty(), "spans were built for a folder with no takes");
    }

    // -----------------------------------------------------------------------
    // What a project file can hand over. Everything above reaches the
    // normaliser through setCompRegion, which has already tidied the list, so
    // the hostile cases only arrive from a file: segments out of order, two on
    // the same spot, a take index that is not there, a boundary past the end
    // of the clip, a comp that does not start at the beginning, and a NaN.
    {
        const double len = 16.0;

        std::vector<CompSegment> late { { 6.0, 1 }, { 10.0, 2 } };
        normaliseCompSegments (late, 3, len);
        checkSegmentInvariants (late, 3, len, "a comp that starts late");
        check (compTakeAt (late, 0.0) == 1,
               "the stretch before the first boundary did not adopt the take after it");

        std::vector<CompSegment> jumbled { { 9.0, 2 }, { 0.0, 0 }, { 4.0, 1 }, { 4.0, 3 } };
        normaliseCompSegments (jumbled, 4, len);
        checkSegmentInvariants (jumbled, 4, len, "a jumbled comp");
        check (compTakeAt (jumbled, 5.0) == 3,
               "where two boundaries landed on one spot, the later one did not win");

        std::vector<CompSegment> rubbish {
            { 0.0, 0 }, { 2.0, 99 }, { 5.0, -4 },
            { len + 8.0, 1 }, { std::nan (""), 1 }, { -3.0, 2 }
        };
        normaliseCompSegments (rubbish, 3, len);
        checkSegmentInvariants (rubbish, 3, len, "a comp full of rubbish");

        // The cap exists so a file cannot hand the audio thread a table of any
        // size it likes.
        std::vector<CompSegment> huge;
        for (size_t i = 0; i < kMaxCompSegments * 2; ++i)
            huge.push_back ({ (double) i * 1.0e-3, (int) (i % 2) });
        normaliseCompSegments (huge, 2, 1.0e9);
        check (huge.size() <= kMaxCompSegments, "the segment cap did not hold");
        checkSegmentInvariants (huge, 2, 1.0e9, "a comp over the cap");
    }

    // -----------------------------------------------------------------------
    // A comp made the way a producer makes one: drag a region of one pass,
    // then another. Everything outside a drag has to be left exactly as it
    // was, which is the whole promise of comping per section.
    {
        const double len = 32.0;
        const int    takeCount = 4;
        std::vector<CompSegment> segs;

        setCompRegion (segs, 8.0, 16.0, 2, takeCount, len);
        checkSegmentInvariants (segs, takeCount, len, "one region");

        check (compTakeAt (segs, 0.0)  == 0, "the start of the clip changed when a middle region was set");
        check (compTakeAt (segs, 7.9)  == 0, "just before the region changed");
        check (compTakeAt (segs, 8.0)  == 2, "the region does not begin where it was dragged from");
        check (compTakeAt (segs, 12.0) == 2, "the middle of the region is not the take it was given");
        check (compTakeAt (segs, 16.0) == 0, "the region did not end where it was dragged to");
        check (compTakeAt (segs, 31.0) == 0, "the end of the clip changed");

        // A second region inside the first splits it, leaving the take either
        // side of the new one.
        setCompRegion (segs, 10.0, 12.0, 3, takeCount, len);
        checkSegmentInvariants (segs, takeCount, len, "nested region");
        check (compTakeAt (segs, 9.0)  == 2, "the part of the first region before the second was lost");
        check (compTakeAt (segs, 11.0) == 3, "the second region did not take over");
        check (compTakeAt (segs, 13.0) == 2, "the part of the first region after the second was lost");

        // Dragged backwards, which is half of all drags.
        std::vector<CompSegment> backwards;
        setCompRegion (backwards, 16.0, 8.0, 2, takeCount, len);
        check (backwards == std::vector<CompSegment> { { 0.0, 0 }, { 8.0, 2 }, { 16.0, 0 } },
               "a region dragged right to left is not the same region");

        // A drag that went nowhere changes nothing.
        auto before = segs;
        setCompRegion (segs, 20.0, 20.0, 1, takeCount, len);
        check (segs == before, "a drag of no width still edited the comp");

        // A region covering the whole folder leaves exactly one span.
        std::vector<CompSegment> all;
        setCompRegion (all, -5.0, len + 5.0, 1, takeCount, len);
        check (all == std::vector<CompSegment> { { 0.0, 1 } },
               "a region over the whole folder did not replace the comp");
    }

    // -----------------------------------------------------------------------
    // Takes deleted out of order. A take is addressed by position, so deleting
    // one in the middle renumbers everything above it, and a comp that was not
    // renumbered with it plays a different pass than the one that was chosen.
    {
        const double len = 32.0;
        std::vector<CompSegment> segs;
        setCompRegion (segs, 0.0,  8.0,  0, 4, len);
        setCompRegion (segs, 8.0,  16.0, 1, 4, len);
        setCompRegion (segs, 16.0, 24.0, 2, 4, len);
        setCompRegion (segs, 24.0, 32.0, 3, 4, len);
        check (segs.size() == 4, "four regions did not make four segments");

        // Delete take 1. What was take 2 is now take 1 and take 3 is now 2,
        // and the stretch that pointed at 1 falls back to whatever sits there.
        compTakeRemoved (segs, 1, 3, len);
        checkSegmentInvariants (segs, 3, len, "after a middle take went");
        check (compTakeAt (segs, 4.0)  == 0, "deleting take 1 disturbed the stretch using take 0");
        check (compTakeAt (segs, 20.0) == 1, "take 2 was not renumbered to 1");
        check (compTakeAt (segs, 28.0) == 2, "take 3 was not renumbered to 2");
        check (compTakeAt (segs, 12.0) == 1, "the orphaned stretch did not fall back to a take that exists");

        // Delete the last one, then the first, then what is left.
        compTakeRemoved (segs, 2, 2, len);
        checkSegmentInvariants (segs, 2, len, "after the last take went");
        compTakeRemoved (segs, 0, 1, len);
        checkSegmentInvariants (segs, 1, len, "after the first take went");
        check (segs.size() == 1, "one take left should leave one segment");
        compTakeRemoved (segs, 0, 0, len);
        check (segs.empty(), "removing the only take left a comp behind");
    }

    // -----------------------------------------------------------------------
    // Randomised comps. This is where the real bugs have come from in this
    // codebase, so the generated cases include the awkward ones on purpose:
    // regions far shorter than the crossfade, crossfades far wider than the
    // clip, and takes that do not cover the whole folder.
    {
        std::mt19937 rng (20261004);

        for (int iteration = 0; iteration < 4000; ++iteration)
        {
            std::uniform_real_distribution<double> lenDist (0.5, 64.0);
            std::uniform_int_distribution<int>     takeDist (1, 8);
            std::uniform_int_distribution<int>     regionDist (0, 12);

            const double len       = lenDist (rng);
            const int    takeCount = takeDist (rng);

            // Half the iterations give every take the whole folder, which is
            // what loop recording produces and the case where unity power has
            // to hold everywhere. The other half punches takes in and out, so
            // the clamping is exercised.
            const bool full = (iteration % 2) == 0;
            std::vector<CompTakeSpan> takes;
            for (int t = 0; t < takeCount; ++t)
            {
                if (full)
                {
                    takes.push_back ({ 0.0, len });
                }
                else
                {
                    std::uniform_real_distribution<double> startDist (0.0, len);
                    const double start = startDist (rng);
                    std::uniform_real_distribution<double> runDist (0.0, len - start);
                    takes.push_back ({ start, runDist (rng) });
                }
            }

            std::vector<CompSegment> segs;
            const int regions = regionDist (rng);
            for (int r = 0; r < regions; ++r)
            {
                std::uniform_real_distribution<double> atDist (-2.0, len + 2.0);
                std::uniform_int_distribution<int>     whichDist (0, takeCount - 1);
                setCompRegion (segs, atDist (rng), atDist (rng), whichDist (rng), takeCount, len);
            }

            // An untouched comp is legitimately empty: it means "the first
            // take, all the way through", and normalising is what spells that
            // out. The project normalises on every edit and on load, so the
            // invariants are checked on the spelled out form.
            normaliseCompSegments (segs, takeCount, len);
            checkSegmentInvariants (segs, takeCount, len, "random comp");

            std::uniform_real_distribution<double> xfadeDist (0.0, len * 1.5);
            const double crossfade = (iteration % 7 == 0) ? 0.0 : xfadeDist (rng);

            const auto spans = buildCompSpans (segs, takes, len, crossfade);
            checkSpansTile (spans, len, "random comp");
            checkReadsInsideTakes (spans, takes, "random comp");
            checkReadRangesOrdered (spans, "random comp");
            checkFadeWindowsDisjoint (spans, "random comp");
            checkJoinWidths (spans, crossfade, "random comp");
            checkSpansAgreeWithTakeAt (spans, segs, "random comp");

            // Unity power, everywhere, when every take covers the folder. This
            // is the property that says the joins neither dip nor peak, and it
            // is checked right across the comp rather than only at the joins,
            // because a window that is slightly the wrong width shows up as a
            // dip just off centre.
            if (full && ! spans.empty())
            {
                for (int k = 0; k <= 600; ++k)
                {
                    const double at = len * k / 601.0;   // never exactly the end
                    checkClose (compTotalPower (spans, at), 1.0, 1.0e-9,
                                "random comp: the comp does not sum to unity power at "
                                    + std::to_string (at) + "s");
                }

                // And exactly at every join, where a linear fade would read a half.
                for (size_t i = 1; i < spans.size(); ++i)
                    checkClose (compTotalPower (spans, spans[i].start), 1.0, 1.0e-9,
                                "random comp: the join at " + std::to_string (spans[i].start)
                                    + " does not sum to unity power");
            }

            // Removing takes in whatever order, which is what a producer does
            // once the comp is settled and the rejects are in the way.
            auto survivors = takes;
            auto comp      = segs;
            int  remaining = takeCount;
            while (remaining > 0)
            {
                std::uniform_int_distribution<int> victim (0, remaining - 1);
                const int index = victim (rng);
                survivors.erase (survivors.begin() + index);
                --remaining;
                compTakeRemoved (comp, index, remaining, len);

                if (remaining == 0)
                {
                    check (comp.empty(), "a comp outlived the last of its takes");
                    break;
                }

                checkSegmentInvariants (comp, remaining, len, "after a take was deleted");
                const auto after = buildCompSpans (comp, survivors, len, crossfade);
                checkSpansTile (after, len, "after a take was deleted");
                checkReadsInsideTakes (after, survivors, "after a take was deleted");
                checkReadRangesOrdered (after, "after a take was deleted");
                checkSpansAgreeWithTakeAt (after, comp, "after a take was deleted");
            }
        }
    }

    // -----------------------------------------------------------------------
    // A take that stops early. The comp still covers the folder, but the take is
    // read only where it has audio, and nothing asks for a sample past its end
    // even though the crossfade wants to reach there.
    {
        const double len = 16.0;
        const std::vector<CompTakeSpan> takes { { 0.0, len }, { 0.0, 6.0 } };

        std::vector<CompSegment> segs;
        setCompRegion (segs, 4.0, 12.0, 1, 2, len);

        const auto spans = buildCompSpans (segs, takes, len, 0.25);
        checkSpansTile (spans, len, "short take");
        checkReadsInsideTakes (spans, takes, "short take");

        // The span given to the short take is cut off where its audio is.
        const CompSpan* shortSpan = nullptr;
        for (const auto& s : spans)
            if (s.take == 1)
                shortSpan = &s;

        check (shortSpan != nullptr, "the short take got no span at all");
        if (shortSpan != nullptr)
            checkClose (shortSpan->readTo, 6.0, 1.0e-12,
                        "the short take is read past the end of its audio");
    }

    // -----------------------------------------------------------------------
    // What a drag is for, as opposed to what it leaves behind.
    //
    // Everything above checks the shape of the result: that it is normalised,
    // that the spans tile the folder, that the joins sum to unity power. None
    // of it asks the question the gesture exists to answer, which is whether
    // the take asked for is heard across the stretch it was asked for and
    // nothing outside that stretch moved. Four thousand randomised comps were
    // being checked for structure and never once for meaning.
    //
    // The distinction is not academic, and the cheapest demonstration is one
    // line of the header. `setCompRegion` reads what plays at the far edge of
    // the drag before erasing the boundaries inside it, and the comment above
    // that line says why. Read it afterwards instead and every structural
    // check in this file still passes: the result is normalised, it tiles, it
    // sums to unity. What it does is hand the stretch after the drag to
    // whatever was playing before it, so comping over the end of pass three
    // silently deletes the whole of pass four's stretch. A line of the vocal
    // is gone and the arrangement looks exactly right.
    {
        std::mt19937 rng (20261023);

        // Clear of the tenth of a millisecond within which two boundaries are
        // one boundary, so a sample point is never inside the snapping.
        const double margin = 1.0e-3;

        for (int iteration = 0; iteration < 20000; ++iteration)
        {
            std::uniform_real_distribution<double> lenDist (0.5, 64.0);
            std::uniform_int_distribution<int>     takeDist (1, 8);
            const double len       = lenDist (rng);
            const int    takeCount = takeDist (rng);

            std::uniform_real_distribution<double> atDist (-2.0, len + 2.0);
            std::uniform_int_distribution<int>     whichDist (0, takeCount - 1);
            std::uniform_int_distribution<int>     regionDist (0, 6);

            // A comp somebody has already been working on, so the drag has
            // existing boundaries to land on, swallow and run past.
            std::vector<CompSegment> segs;
            const int regions = regionDist (rng);
            for (int r = 0; r < regions; ++r)
                setCompRegion (segs, atDist (rng), atDist (rng), whichDist (rng), takeCount, len);

            normaliseCompSegments (segs, takeCount, len);
            const auto before = segs;

            double from = atDist (rng), to = atDist (rng);
            const int take = whichDist (rng);
            setCompRegion (segs, from, to, take, takeCount, len);

            checkSegmentInvariants (segs, takeCount, len, "after one more drag");

            // The gesture in its own terms: dragged right to left is the same
            // region, and a drag is held inside the folder.
            if (to < from)
                std::swap (from, to);
            from = std::max (0.0, from);
            to   = std::min (to, len);

            // The message is built only when something is wrong. Concatenating
            // one per sample point costs more than every other check in this
            // file put together, and there are four million of them.
            const auto where = [&] (double at)
            {
                return "a drag of [" + std::to_string (from) + ", " + std::to_string (to)
                         + "] to take " + std::to_string (take) + " in a folder of "
                         + std::to_string (len) + " seconds, read at " + std::to_string (at);
            };

            if (to - from < kMinCompSpan)
            {
                check (segs == before, "a drag of no width still edited the comp");
                continue;
            }

            for (int k = 0; k <= 200; ++k)
            {
                const double at = len * k / 201.0;

                if (at > from + margin && at < to - margin)
                {
                    if (compTakeAt (segs, at) != take)
                        fail (where (at) + ": the take dragged for is not the take heard");
                }
                else if (at < from - margin || at > to + margin)
                {
                    if (compTakeAt (segs, at) != compTakeAt (before, at))
                        fail (where (at) + ": outside the drag, take "
                                + std::to_string (compTakeAt (before, at)) + " became take "
                                + std::to_string (compTakeAt (segs, at)));
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // A crossfade width that is not a width.
    //
    // Not reachable through the project today: `ProjectIO` clamps the stored
    // value to nought to 250 ms on load and `Project` clamps it the same way
    // on the way in, so the guards in `buildCompSpans` are the second line
    // rather than the first. A guard nothing exercises is a guard nobody
    // notices going, and this one has a specific failure behind it rather than
    // a crash: a negative half width pulls every read range inward from its
    // own boundary instead of past it, so the comp acquires a short silence at
    // every join. Holes in the performance, in the one piece of arithmetic
    // whose entire purpose is that there are none.
    {
        const double len = 32.0;
        const std::vector<CompTakeSpan> takes { { 0.0, len }, { 0.0, len } };
        std::vector<CompSegment> segs;
        setCompRegion (segs, 8.0, 16.0, 1, 2, len);
        check (segs.size() == 3, "the two join case did not come out with two joins");

        for (const double width : { std::numeric_limits<double>::quiet_NaN(),
                                    std::numeric_limits<double>::infinity(),
                                    -std::numeric_limits<double>::infinity(),
                                    -4.0, -1.0e-9 })
        {
            const std::string label = "a crossfade of " + std::to_string (width);
            const auto spans = buildCompSpans (segs, takes, len, width);

            checkSpansTile (spans, len, label);
            checkReadsInsideTakes (spans, takes, label);
            checkReadRangesOrdered (spans, label);
            checkFadeWindowsDisjoint (spans, label);
            checkSpansAgreeWithTakeAt (spans, segs, label);

            // No join at all, rather than one of some unimaginable width.
            for (size_t i = 0; i < spans.size(); ++i)
            {
                checkClose (spans[i].fadeIn,  0.0, 0.0, label + ": a fade in appeared");
                checkClose (spans[i].fadeOut, 0.0, 0.0, label + ": a fade out appeared");
            }

            // And every take is read right up to both its boundaries, so
            // there is no hole anywhere across the folder.
            for (int k = 0; k <= 4000; ++k)
            {
                const double at = len * k / 4001.0;
                if (! (std::abs (compTotalPower (spans, at) - 1.0) <= 1.0e-12))
                    fail (label + ": the comp does not sum to unity power at "
                                + std::to_string (at) + "s");
            }
        }
    }

    // -----------------------------------------------------------------------
    // A boundary sitting on, or a hair before, the folder's own end.
    //
    // `setCompRegion` will not make one: it clamps the drag to the folder and
    // then declines to close a region that reaches the end. So this shape only
    // arrives from a file written by something else, which is exactly the case
    // the guard in `normaliseCompSegments` is for. Such a boundary governs
    // nothing, and kept it asks for a final span of no length, or of less
    // length than the tenth of a millisecond below which two boundaries are
    // one boundary.
    {
        const double len = 16.0;
        const std::vector<CompTakeSpan> takes { { 0.0, len }, { 0.0, len } };

        for (const double at : { len, len - kMinCompSpan * 0.5, len - kMinCompSpan,
                                 len + 1.0, len * 2.0 })
        {
            std::vector<CompSegment> segs { { 0.0, 0 }, { 8.0, 1 }, { at, 0 } };
            normaliseCompSegments (segs, 2, len);

            const std::string label = "a boundary at " + std::to_string (at)
                                        + " in a folder of " + std::to_string (len);
            checkSegmentInvariants (segs, 2, len, label);
            check (segs.size() == 2,
                   label + ": the boundary at the folder's end was not dropped");

            const auto spans = buildCompSpans (segs, takes, len, 0.25);
            checkSpansTile (spans, len, label);
            checkJoinWidths (spans, 0.25, label);
            checkSpansAgreeWithTakeAt (spans, segs, label);
        }
    }

    // -----------------------------------------------------------------------
    // The later edit wins where two boundaries land on one spot, on a list
    // long enough that the sort has to earn it.
    //
    // There is a check on this above, on three segments, and it is not enough.
    // The rule is implemented by sorting stably and letting each duplicate
    // overwrite the one before it, and on a short list an unstable sort is
    // stable anyway: libstdc++ insertion sorts anything up to sixteen
    // elements, so `std::sort` in place of `std::stable_sort` passes the short
    // case and then gives a different comp on a different standard library.
    // That is the fault `PanOneBreed.h` carries its own generator to avoid,
    // arriving by another route: a project that opens differently depending on
    // who built the studio.
    //
    // Sixty four spots, each written twice. The first write of each names one
    // take and the second names another, and the two alternate along the
    // folder so that no neighbours agree and nothing merges away. Every spot
    // has to read as its second write.
    {
        const double len   = 256.0;
        const int    spots = 64;

        std::vector<CompSegment> segs;
        const auto second = [] (int i) { return 1 + (i % 2); };          // 1, 2, 1, 2, ...
        const auto first  = [] (int i) { return 1 + ((i + 1) % 2); };    // 2, 1, 2, 1, ...

        for (int i = 0; i < spots; ++i)
            segs.push_back ({ 1.0 + (double) i * 4.0, first (i) });
        for (int i = 0; i < spots; ++i)
            segs.push_back ({ 1.0 + (double) i * 4.0, second (i) });

        normaliseCompSegments (segs, 3, len);
        checkSegmentInvariants (segs, 3, len, "sixty four doubled boundaries");

        for (int i = 0; i < spots; ++i)
        {
            const double at = 1.0 + (double) i * 4.0 + 2.0;    // inside spot i
            check (compTakeAt (segs, at) == second (i),
                   "at spot " + std::to_string (i) + " the first write of the boundary won: "
                     + "take " + std::to_string (compTakeAt (segs, at)) + " rather than "
                     + std::to_string (second (i)));
        }
    }

    // -----------------------------------------------------------------------
    // A drag that does nothing, on a folder nobody has comped yet.
    //
    // This is the one state where "nothing happened" is hard to spell. An
    // untouched comp is an empty list, which means "the first take, all the
    // way through", and normalising an empty list spells that out as a single
    // segment at zero. The two are the same performance, so no check on which
    // take is heard can tell them apart, and every structural invariant holds
    // for both.
    //
    // What tells them apart is `TakeFolder::operator==`, which is what decides
    // whether the project is dirty and whether an edit goes on the undo stack.
    // So a click that misses, or a drag entirely off the end of the folder,
    // would mark the project changed and push an undo step for an edit that
    // did not happen, against the rule that undo is one step per finished edit
    // rather than one per mouse movement.
    //
    // Both guards that prevent it, the clamp of the drag into the folder and
    // the floor on its width, looked like no-ops until they were diffed on
    // this case: every other test here drags on a comp that has already been
    // normalised, where both spellings have collapsed to the same one.
    {
        const double len = 16.0;

        const std::vector<std::pair<double, double>> doNothing {
            { len + 1.0, len + 5.0 },          // entirely past the end
            { -9.0, -3.0 },                    // entirely before the start
            { 4.0, 4.0 },                      // a click, not a drag
            { 4.0, 4.0 + kMinCompSpan * 0.5 }, // a drag that went nowhere
            { len, len + 2.0 },                // starting exactly at the end
        };

        for (const auto& [from, to] : doNothing)
        {
            std::vector<CompSegment> fresh;
            setCompRegion (fresh, from, to, 1, 2, len);

            const std::string label = "a drag of [" + std::to_string (from) + ", "
                                        + std::to_string (to) + "] on a fresh folder";
            check (fresh.empty(), label + " edited the comp anyway");
        }

        // And the same drags on a comp that is already spelled out leave it
        // exactly as it was, which is the other half of the same promise.
        for (const auto& [from, to] : doNothing)
        {
            std::vector<CompSegment> segs;
            setCompRegion (segs, 4.0, 12.0, 1, 2, len);
            const auto was = segs;
            setCompRegion (segs, from, to, 0, 2, len);

            check (segs == was, "a drag of [" + std::to_string (from) + ", "
                                  + std::to_string (to) + "] on a comp edited it anyway");
        }
    }

    // -----------------------------------------------------------------------
    // A boundary whose position is not a number.
    //
    // It is dropped, and the thing to check is that it is dropped rather than
    // moved. Every position in the list is floored at zero a few lines further
    // down, and `std::max (0.0, x)` returns its first argument when the
    // comparison is false, so a boundary that is not a number does not stay
    // where it is: it lands exactly on the folder's start and takes over the
    // opening of the performance with whatever take it was carrying. An
    // infinity below zero does the same.
    //
    // There was already a case here feeding this function a list full of
    // rubbish, and it checked the invariants of what came back. That cannot
    // see this: a boundary at zero naming a take that exists satisfies every
    // one of them. What it needs is the list itself.
    {
        const double len = 16.0;

        for (const double bad : { std::numeric_limits<double>::quiet_NaN(),
                                  -std::numeric_limits<double>::infinity(),
                                  std::numeric_limits<double>::infinity() })
        {
            std::vector<CompSegment> segs { { 0.0, 0 }, { 8.0, 1 }, { bad, 2 } };
            normaliseCompSegments (segs, 3, len);

            const std::string label = "a boundary at " + std::to_string (bad);
            checkSegmentInvariants (segs, 3, len, label);
            check (segs == std::vector<CompSegment> { { 0.0, 0 }, { 8.0, 1 } },
                   label + " was moved rather than dropped");
            check (compTakeAt (segs, 1.0) == 0,
                   label + " took over the start of the performance");
        }
    }

    if (failures == 0)
        std::printf ("  ok\n");
    else
        std::printf ("  %d failure(s)\n", failures);

    return failures == 0 ? 0 : 1;
}
