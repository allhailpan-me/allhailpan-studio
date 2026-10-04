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
            checkFadeWindowsDisjoint (spans, "random comp");

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

    if (failures == 0)
        std::printf ("  ok\n");
    else
        std::printf ("  %d failure(s)\n", failures);

    return failures == 0 ? 0 : 1;
}
