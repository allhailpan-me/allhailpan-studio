// ---------------------------------------------------------------------------
// Checks punch recording: that the punch replaces what was under it instead of
// summing with it, and that the joins at either end sit outside the punch.
//
// Punch.h has no JUCE in it so this builds and runs on its own with
// sanitizers. CI runs it on every push.
//
// Everything here goes through the real buildCompSpans and the real
// compFadeGain rather than through a restatement of them, because what has to
// be true is a property of the audio that comes out and not of the numbers
// Punch.h returns. A punch that computed beautiful boundaries and then handed
// the comp model something it narrowed differently would pass a test written
// against the boundaries and fail in the room.
//
// Three faults this is aimed at, in the order they would hurt:
//
//   - The punch sums with the performance it was meant to replace, which is
//     what the engine would have done and which no studio ships. Two vocals at
//     once at full level. Checked by rendering, with the old take and the new
//     take carrying different values, and reading what comes out.
//   - The crossfade is centred on the punch point, which is the intuitive
//     reading and is wrong against the only published statement of the
//     geometry. It attenuates the first and last few milliseconds of the take
//     just punched in, which is usually the attack somebody punched to fix.
//     It sounds almost right, which is the hardest kind of wrong.
//   - The folder keeps a crossfade width the boundaries were not built for,
//     which lets the fade reach into the punch by however much they differ.
//
// Randomised over thousands of generated punches, including the ones a mouse
// and a nervous guitarist produce: a punch shorter than its own crossfade, a
// punch starting a millisecond after the transport rolled, a punch at the very
// start of the clip, a punch running off the end of it, and one sided punches.
// ---------------------------------------------------------------------------

#include "Punch.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
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

static void checkClose (double got, double want, double tol, const std::string& what)
{
    if (! (std::abs (got - want) <= tol))
        fail (what + ": got " + std::to_string (got) + ", wanted " + std::to_string (want));
}

//==============================================================================
// The comp as the engine hears it. One value per take standing in for its
// audio, summed over every span sounding at a point, at the gain the comp
// reads it at. This is what renderAudioClips does per sample, in miniature.
static double renderAt (const std::vector<CompSpan>& spans,
                        const std::vector<double>& takeValue, double at)
{
    double sum = 0.0;

    for (const auto& s : spans)
    {
        if (at < s.readFrom || at >= s.readTo)
            continue;
        sum += takeValue[(size_t) s.take] * compFadeGain (s.fade(), at);
    }

    return sum;
}

// The gain one take alone is read at. Summed over its spans, because a take
// can own more than one stretch of the folder.
static double gainOf (const std::vector<CompSpan>& spans, int take, double at)
{
    double sum = 0.0;

    for (const auto& s : spans)
    {
        if (s.take != take || at < s.readFrom || at >= s.readTo)
            continue;
        sum += compFadeGain (s.fade(), at);
    }

    return sum;
}

//==============================================================================
// One punch to check: the clip that was there, the pass that was captured, and
// the punch itself.
struct Scene
{
    double folderSeconds = 0.0;   // the clip that was there, as a folder
    double bpm           = 120.0;
    double folderStart   = 0.0;   // the clip's own position, in beats

    Punch::Range range;
    Punch::Material material;

    double inSec  = 0.0;          // the punch, in folder seconds
    double outSec = 0.0;

    std::vector<CompTakeSpan> spans() const
    {
        return { { 0.0, folderSeconds },
                 { material.from, material.to - material.from } };
    }
};

// Builds the comp the way the engine would, and returns the spans it plays.
static std::vector<CompSpan> compFor (const Scene& sc, const Punch::Result& r,
                                      std::vector<CompSegment>& comp)
{
    // Whatever the folder happened to be carrying before the punch. Ten
    // milliseconds is TakeFolder's own default, and starting from it is the
    // honest case: applyTo has to set this whether it differs or not.
    double crossfadeMs = 10.0;

    if (! Punch::applyTo (comp, crossfadeMs, r, 1, 2, sc.folderSeconds))
        return {};

    // The width the folder ends up carrying is the width the spans are built
    // with, which is the contract applyTo exists to keep. Reading it back here
    // rather than passing the result's own figure is deliberate: if applyTo
    // ever stopped setting it, every geometry check below would start failing
    // instead of silently passing against a number the folder does not have.
    return buildCompSpans (comp, sc.spans(), sc.folderSeconds, crossfadeMs * 0.001);
}

int main()
{
    std::printf ("PunchTest\n");

    // ---------------------------------------------------------------------
    // What a range is, and what it refuses to be.
    // ---------------------------------------------------------------------
    {
        check (Punch::isUsable ({ 4.0, 8.0, true, true }), "an ordinary punch");
        check (Punch::isUsable ({ 4.0, 0.0, true, false }), "Ardour's In only");
        check (Punch::isUsable ({ 0.0, 8.0, false, true }), "Ardour's Out only");

        check (! Punch::isUsable ({ 8.0, 4.0, true, true }),
               "a punch that ends before it starts was accepted");
        check (! Punch::isUsable ({ 4.0, 4.0, true, true }),
               "a punch of no length was accepted");
        check (! Punch::isUsable ({ 0.0, 0.0, false, false }),
               "no boundaries at all is not a punch and was accepted as one");

        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();

        check (! Punch::isUsable ({ nan, 8.0, true, true }), "a punch in of NaN");
        check (! Punch::isUsable ({ 4.0, nan, true, true }), "a punch out of NaN");
        check (! Punch::isUsable ({ inf, 8.0, true, true }), "a punch in of infinity");
        check (! Punch::isUsable ({ 4.0, -inf, true, true }), "a punch out of minus infinity");

        // An absent boundary carrying rubbish is still absent, because nothing
        // reads it. Refusing here would make In only unusable from a project
        // file that had never set the other number.
        check (Punch::isUsable ({ 4.0, nan, true, false }),
               "In only was refused for the value of the boundary it does not have");
    }

    // ---------------------------------------------------------------------
    // The record button's two states. Logic and Sequoia both publish the
    // distinction between armed and waiting, and armed and recording.
    // ---------------------------------------------------------------------
    {
        const Punch::Range both { 4.0, 8.0, true, true };

        check (! Punch::recordingAt (both, 3.999), "waiting before the punch in");
        check (  Punch::recordingAt (both, 4.0),   "the punch in itself records");
        check (  Punch::recordingAt (both, 6.0),   "inside the punch");
        check (! Punch::recordingAt (both, 8.0),
               "the punch out itself recorded: the range is half open, as a loop range is");
        check (! Punch::recordingAt (both, 9.0),   "after the punch out");

        const Punch::Range inOnly { 4.0, 0.0, true, false };
        check (! Punch::recordingAt (inOnly, 3.0), "In only before its marker");
        check (  Punch::recordingAt (inOnly, 4.0), "In only at its marker");
        check (  Punch::recordingAt (inOnly, 4000.0),
               "In only stopped somewhere, and it has no end boundary to stop at");

        const Punch::Range outOnly { 0.0, 8.0, false, true };
        check (  Punch::recordingAt (outOnly, -50.0),
               "Out only has no beginning boundary and did not record before its marker");
        check (  Punch::recordingAt (outOnly, 7.999), "Out only before its marker");
        check (! Punch::recordingAt (outOnly, 8.0),   "Out only at its marker");

        check (! Punch::recordingAt ({ 8.0, 4.0, true, true }, 6.0),
               "an unusable range reported that it was recording");
    }

    // ---------------------------------------------------------------------
    // Beats to folder seconds. The conversion happens once, when the punch
    // lands, and the comp is in seconds from then on.
    // ---------------------------------------------------------------------
    {
        checkClose (Punch::secondsInto (8.0, 0.0, 120.0), 4.0, 1.0e-12,
                    "eight beats at 120 bpm");
        checkClose (Punch::secondsInto (8.0, 4.0, 120.0), 2.0, 1.0e-12,
                    "four beats into a clip that starts at beat four");
        checkClose (Punch::secondsInto (8.0, 0.0, 60.0), 8.0, 1.0e-12,
                    "eight beats at 60 bpm");

        // Not clamped, and that is the point: a punch can begin before the
        // clip it is punching into, and the caller has to be able to tell.
        // Clamping this to zero would put such a punch at the clip's start.
        checkClose (Punch::secondsInto (2.0, 6.0, 120.0), -2.0, 1.0e-12,
                    "a position before the clip came back clamped rather than negative");

        checkClose (Punch::secondsInto (8.0, 0.0, 0.0), 0.0, 1.0e-12,
                    "a tempo of zero");
        checkClose (Punch::secondsInto (std::numeric_limits<double>::quiet_NaN(), 0.0, 120.0),
                    0.0, 1.0e-12, "a beat of NaN");
    }

    // ---------------------------------------------------------------------
    // Punching into nothing is not a punch.
    // ---------------------------------------------------------------------
    {
        const Punch::Range r { 4.0, 8.0, true, true };

        check (  Punch::overlaps (r, 0.0, 16.0), "a punch inside a clip");
        check (  Punch::overlaps (r, 6.0, 16.0), "a punch overhanging the clip's start");
        check (  Punch::overlaps (r, 0.0, 6.0),  "a punch overhanging the clip's end");
        check (! Punch::overlaps (r, 8.0, 16.0),
               "a punch that ends exactly where the clip starts is not on it");
        check (! Punch::overlaps (r, 0.0, 4.0),
               "a punch that starts exactly where the clip ends is not on it");
        check (! Punch::overlaps (r, 20.0, 32.0), "a punch nowhere near the clip");
        check (! Punch::overlaps (r, 4.0, 4.0),   "a clip of no length");
    }

    // ---------------------------------------------------------------------
    // THE ONE THAT MATTERS. Over thousands of generated punches: the punch is
    // heard alone and at full level across every sample of itself, the old
    // performance is heard alone and at full level outside the joins, and the
    // joins sit entirely outside the punch.
    // ---------------------------------------------------------------------
    {
        std::mt19937 rng (20261107u);
        std::uniform_real_distribution<double> unit (0.0, 1.0);

        int checked = 0, withBothJoins = 0, narrowed = 0;

        for (int trial = 0; trial < 4000; ++trial)
        {
            Scene sc;
            sc.folderSeconds = 0.5 + unit (rng) * 30.0;
            sc.bpm           = 40.0 + unit (rng) * 200.0;
            sc.folderStart   = unit (rng) * 64.0;

            // The punch, somewhere in the clip. Deliberately allowed to start
            // at the very edge and to run off the end: both happen, and both
            // are where an off by one lives.
            const double a = unit (rng) * sc.folderSeconds;
            const double b = unit (rng) * sc.folderSeconds * 1.1;
            sc.inSec  = std::min (a, b);
            sc.outSec = std::max (a, b);

            if (sc.outSec - sc.inSec < 1.0e-3)
                continue;                 // generated a stab rather than a phrase

            const int shape = trial % 8;  // one sided punches get a share
            sc.range.hasIn  = shape != 6;
            sc.range.hasOut = shape != 7;
            if (! sc.range.hasIn)  sc.inSec  = 0.0;
            if (! sc.range.hasOut) sc.outSec = sc.folderSeconds;

            sc.range.inBeat  = sc.folderStart + sc.inSec  * sc.bpm / 60.0;
            sc.range.outBeat = sc.folderStart + sc.outSec * sc.bpm / 60.0;

            // The pass. The transport usually rolled from well before the
            // punch, but a quarter of the time it rolled from barely before
            // it, which is what narrows the join.
            const bool tight = (trial % 4) == 0;
            const double lead = tight ? unit (rng) * 0.02 : 0.25 + unit (rng) * 4.0;
            const double tail = tight ? unit (rng) * 0.02 : 0.25 + unit (rng) * 4.0;

            sc.material.from          = sc.inSec - lead;
            sc.material.to            = sc.outSec + tail;
            sc.material.folderSeconds = sc.folderSeconds;

            const auto r = Punch::regionFor (sc.range, sc.folderStart, sc.bpm, sc.material);
            if (! r.valid)
                continue;

            std::vector<CompSegment> comp { { 0.0, 0 } };
            const auto spans = compFor (sc, r, comp);
            if (spans.empty())
            {
                fail ("a valid punch produced no spans to play");
                continue;
            }

            ++checked;
            const double w = r.crossfadeSeconds;
            if (w < Punch::kDefaultCrossfadeMs * 0.001 - 1.0e-12)
                ++narrowed;

            const std::string label = "trial " + std::to_string (trial);

            // Where the punch really reaches: narrowed by the clip, because a
            // comp cannot play past the end of one, and by the pass, because
            // the player decides when to stop.
            const double reachFrom = std::max (std::max (sc.inSec, 0.0), sc.material.from);
            const double reachTo   = std::min (std::min (sc.outSec, sc.folderSeconds),
                                               sc.material.to);

            // The width can never exceed what was asked for, and can never be
            // wider than the material feeding either join. A width that
            // outran its material is a fade reading off the end of a file.
            check (w <= Punch::kDefaultCrossfadeMs * 0.001 + 1.0e-12,
                   label + ": the crossfade came back wider than the one asked for");
            check (w <= reachFrom - sc.material.from + 1.0e-12,
                   label + ": the pre-crossfade is wider than the pass's lead in");
            check (w <= sc.material.to - reachTo + 1.0e-12,
                   label + ": the post-crossfade is wider than the pass's tail");
            check (w <= reachTo - reachFrom + 1.0e-12,
                   label + ": the crossfade is wider than the part of the punch that is "
                            "really being replaced");

            // And the region is the reach, shifted by half a width at each
            // end, which is what stops a punch replacing part of the phrase
            // with silence at one end and is the geometry at the other.
            checkClose (r.from, reachFrom - w * 0.5, 1.0e-9,
                        label + ": the comp region does not start half a width before the reach");
            checkClose (r.to, reachTo + w * 0.5, 1.0e-9,
                        label + ": the comp region does not end half a width after the reach");

            // THE PUNCH IS HEARD ALONE, AT FULL LEVEL, END TO END. The old
            // take must be silent and the new one at unity everywhere from the
            // punch in to the punch out. This is the assertion the summing
            // engine fails, and the one a centred crossfade fails too.
            //
            // The punch is clipped to the folder first: a comp cannot play
            // past the end of the clip it is on, so the part of a punch that
            // runs off the end is not this check's business.
            const double fromAt = std::max (std::max (sc.inSec, 0.0), sc.material.from);
            const double toAt   = std::min (std::min (sc.outSec, sc.folderSeconds),
                                            sc.material.to);

            if (toAt > fromAt)
            {
                const int steps = 64;
                for (int i = 0; i <= steps; ++i)
                {
                    // The last point is nudged inside, because the punch out
                    // is a boundary and the sample sitting on it belongs to
                    // what comes after.
                    const double t = i == steps ? toAt - 1.0e-9
                                                : fromAt + (toAt - fromAt) * (double) i / (double) steps;

                    checkClose (gainOf (spans, 1, t), 1.0, 1.0e-9,
                                label + ": the punch is not at full level at " + std::to_string (t));
                    checkClose (gainOf (spans, 0, t), 0.0, 1.0e-9,
                                label + ": the old performance is still sounding inside the punch at "
                                  + std::to_string (t));

                    // Rendered, with the two takes carrying different signals.
                    // Summing would read 1.5 here rather than 1.0, and a peak
                    // check on two different signals would not see it, which
                    // is why this is a gain and not a peak.
                    checkClose (renderAt (spans, { 0.5, 1.0 }, t), 1.0, 1.0e-9,
                                label + ": the punch did not replace what was under it at "
                                  + std::to_string (t));
                }
            }

            // THE JOINS LIE OUTSIDE THE PUNCH. Checked on the spans
            // themselves as well as by level, because this is the geometry the
            // header exists for and it deserves to be stated once directly.
            for (const auto& s : spans)
            {
                const auto f = s.fade();

                if (s.take == 1)
                {
                    if (f.inTo > f.inFrom)
                    {
                        ++withBothJoins;
                        check (f.inTo <= std::max (std::max (sc.inSec, 0.0), sc.material.from) + 1.0e-9,
                               label + ": the pre-crossfade reaches into the punch");
                        check (f.inFrom >= sc.material.from - 1.0e-9,
                               label + ": the pre-crossfade starts before the pass does");
                    }
                    if (f.outTo > f.outFrom)
                    {
                        check (f.outFrom >= std::min (std::min (sc.outSec, sc.folderSeconds),
                                                      sc.material.to) - 1.0e-9,
                               label + ": the post-crossfade starts inside the punch");
                        check (f.outTo <= sc.material.to + 1.0e-9,
                               label + ": the post-crossfade runs past the end of the pass");
                    }
                }

                // A fade window that reaches outside the take's own audio is a
                // step at the edge of the file, which is a click that no
                // crossfade explains. The comp clamps reads to a take's audio,
                // so the window has to be inside what was clamped to.
                if (s.take == 1 && s.isAudible())
                {
                    check (s.readFrom >= sc.material.from - 1.0e-9,
                           label + ": the punch is read from before the pass begins");
                    check (s.readTo <= sc.material.to + 1.0e-9,
                           label + ": the punch is read past the end of the pass");
                }
            }

            // THE OLD PERFORMANCE IS UNTOUCHED OUTSIDE THE JOINS, which is
            // the other half of what a punch promises: everything that was not
            // punched comes back exactly as it was.
            const double reachedFrom = std::max (std::max (sc.inSec, 0.0), sc.material.from);
            const double reachedTo   = std::min (std::min (sc.outSec, sc.folderSeconds),
                                                 sc.material.to);

            const double outsideBefore = reachedFrom - w - 1.0e-6;
            if (outsideBefore > 1.0e-6)
            {
                checkClose (gainOf (spans, 0, outsideBefore), 1.0, 1.0e-9,
                            label + ": the old performance is not at full level before the punch");
                checkClose (gainOf (spans, 1, outsideBefore), 0.0, 1.0e-9,
                            label + ": the punch is sounding before its own pre-crossfade");
            }

            const double outsideAfter = reachedTo + w + 1.0e-6;
            if (outsideAfter < sc.folderSeconds - 1.0e-6)
            {
                checkClose (gainOf (spans, 0, outsideAfter), 1.0, 1.0e-9,
                            label + ": the old performance did not come back after the punch");
                checkClose (gainOf (spans, 1, outsideAfter), 0.0, 1.0e-9,
                            label + ": the punch is still sounding after its own post-crossfade");
            }

            // THE COMP STILL TILES THE FOLDER. A gap is a hole in the
            // performance and an overlap is two takes at once, and a punch is
            // a new way of writing the comp, so it is a new way of breaking
            // this.
            double at = 0.0;
            for (size_t i = 0; i < spans.size(); ++i)
            {
                checkClose (spans[i].start, at, 1.0e-9,
                            label + ": span " + std::to_string (i) + " does not start where the last one ended");
                check (spans[i].end > spans[i].start - 1.0e-12,
                       label + ": span " + std::to_string (i) + " runs backwards");
                at = spans[i].end;
            }
            checkClose (at, sc.folderSeconds, 1.0e-9,
                        label + ": the comp does not reach the end of the folder");

            // EQUAL POWER THROUGH THE JOINS. The comp model's own invariant,
            // checked through the punch path because that is a new way to
            // reach it: three takes overlapping at a join would read above
            // one here and a sum to one crossfade would read above one in the
            // middle of it.
            for (const auto& s : spans)
            {
                const auto f = s.fade();
                for (int i = 1; i < 8; ++i)
                {
                    if (f.inTo > f.inFrom)
                    {
                        const double t = f.inFrom + (f.inTo - f.inFrom) * (double) i / 8.0;
                        if (t > 0.0 && t < sc.folderSeconds)
                            checkClose (compTotalPower (spans, t), 1.0, 1.0e-6,
                                        label + ": the power through a join is not one at "
                                          + std::to_string (t));
                    }
                }
            }
        }

        check (checked > 2000, "too few punches were actually checked: generated "
                                 + std::to_string (checked));
        check (narrowed > 100, "no punch had its crossfade narrowed by the material, so the "
                               "case a late record button produces went untested: "
                                 + std::to_string (narrowed));
        check (withBothJoins > 500, "almost no punch had a pre-crossfade at all, so the geometry "
                                    "the header exists for went untested: "
                                      + std::to_string (withBothJoins));

        std::printf ("  %d punches checked, %d with a narrowed join, %d with a pre-crossfade\n",
                     checked, narrowed, withBothJoins);
    }

    // ---------------------------------------------------------------------
    // The geometry, once, by hand and at numbers a person can check. The
    // randomised pass above would catch a centred crossfade, but this says
    // what the answer is rather than only that it is consistent.
    // ---------------------------------------------------------------------
    {
        Scene sc;
        sc.folderSeconds = 10.0;
        sc.bpm           = 120.0;          // two beats a second
        sc.folderStart   = 0.0;
        sc.inSec  = 4.0;
        sc.outSec = 6.0;
        sc.range  = { 8.0, 12.0, true, true };
        sc.material = { 2.0, 8.0, 10.0 };  // the pass ran from 2 s to 8 s

        const auto r = Punch::regionFor (sc.range, sc.folderStart, sc.bpm, sc.material);

        check (r.valid, "the worked example was refused");
        checkClose (r.crossfadeSeconds, 0.010, 1.0e-12,
                    "the worked example's crossfade is not the published ten milliseconds");

        // Half a width before and after: this is the whole of the geometry.
        checkClose (r.from, 4.0 - 0.005, 1.0e-12,
                    "the comp boundary is not half a crossfade before the punch in");
        checkClose (r.to, 6.0 + 0.005, 1.0e-12,
                    "the comp boundary is not half a crossfade after the punch out");

        std::vector<CompSegment> comp { { 0.0, 0 } };
        const auto spans = compFor (sc, r, comp);

        // The punch in sample itself is the one a centred fade gets wrong: it
        // would read the new take at sin(pi/4), which is 0.7071, and the old
        // one at the same, and sound like a dip.
        checkClose (gainOf (spans, 1, 4.0), 1.0, 1.0e-12,
                    "the punch is not at full level at its own first sample");
        checkClose (gainOf (spans, 0, 4.0), 0.0, 1.0e-12,
                    "the old performance is still sounding at the punch in");
        checkClose (gainOf (spans, 1, 6.0 - 1.0e-9), 1.0, 1.0e-9,
                    "the punch is not at full level at its own last sample");

        // And the fade really is a fade, in the old material, where Avid puts
        // it. Halfway through the pre-crossfade both takes read the equal
        // power midpoint.
        checkClose (gainOf (spans, 1, 4.0 - 0.005), 0.7071067811865476, 1.0e-9,
                    "the pre-crossfade's midpoint is not halfway through it");
        checkClose (gainOf (spans, 0, 4.0 - 0.005), 0.7071067811865476, 1.0e-9,
                    "the old performance is not halfway out at the pre-crossfade's midpoint");
        checkClose (gainOf (spans, 1, 4.0 - 0.0101), 0.0, 1.0e-12,
                    "the punch is sounding before its pre-crossfade starts");

        checkClose (comp.size() == 3 ? 1.0 : 0.0, 1.0,
                    0.5, "a punch inside a clip is not three comp segments");
    }

    // ---------------------------------------------------------------------
    // The folder is left carrying the width the boundaries were built for.
    // Two separate calls would make this easy to forget and impossible to
    // hear, so applyTo does both and this checks that it did.
    // ---------------------------------------------------------------------
    {
        Scene sc;
        sc.folderSeconds = 10.0;
        sc.bpm = 120.0;
        sc.inSec = 4.0; sc.outSec = 6.0;
        sc.range = { 8.0, 12.0, true, true };

        // A pass that rolled only two milliseconds before the punch, so the
        // join has to narrow to two milliseconds and the folder has to be told.
        sc.material = { 3.998, 8.0, 10.0 };

        const auto r = Punch::regionFor (sc.range, sc.folderStart, sc.bpm, sc.material);
        check (r.valid, "a tight punch was refused");
        checkClose (r.crossfadeSeconds, 0.002, 1.0e-12,
                    "the join did not narrow to the lead in the pass actually had");

        std::vector<CompSegment> comp { { 0.0, 0 } };
        double crossfadeMs = 10.0;        // the folder default, deliberately wrong for this punch
        check (Punch::applyTo (comp, crossfadeMs, r, 1, 2, sc.folderSeconds),
               "applyTo refused a valid punch");
        checkClose (crossfadeMs, 2.0, 1.0e-12,
                    "the folder kept a crossfade wider than the punch was built for, which lets "
                    "the fade reach into the punch");

        // And with the folder's own figure, the guarantee still holds.
        const auto spans = buildCompSpans (comp, sc.spans(), sc.folderSeconds, crossfadeMs * 0.001);
        checkClose (gainOf (spans, 1, 4.0), 1.0, 1.0e-12,
                    "a narrowed punch is not at full level at its own first sample");
        checkClose (gainOf (spans, 0, 4.0), 0.0, 1.0e-12,
                    "the old performance is still sounding at a narrowed punch in");
    }

    // ---------------------------------------------------------------------
    // What regionFor refuses, and leaves alone.
    // ---------------------------------------------------------------------
    {
        const Punch::Range good { 8.0, 12.0, true, true };
        const Punch::Material mat { 2.0, 8.0, 10.0 };
        const double nan = std::numeric_limits<double>::quiet_NaN();

        check (! Punch::regionFor ({ 12.0, 8.0, true, true }, 0.0, 120.0, mat).valid,
               "a backwards punch produced a region");
        check (! Punch::regionFor (good, 0.0, 120.0, { 2.0, 8.0, 0.0 }).valid,
               "a folder of no length produced a region");
        check (! Punch::regionFor (good, 0.0, 120.0, { 2.0, 8.0, nan }).valid,
               "a folder length of NaN produced a region");
        check (! Punch::regionFor (good, 0.0, 120.0, { 8.0, 2.0, 10.0 }).valid,
               "a pass that ends before it starts produced a region");
        check (! Punch::regionFor (good, 0.0, 120.0, { nan, 8.0, 10.0 }).valid,
               "a pass starting at NaN produced a region");
        check (! Punch::regionFor (good, 0.0, nan, mat).valid,
               "a tempo of NaN produced a region");

        // A punch that misses the clip. Both of these came back valid before
        // the overlap check went in, and both then changed nothing while
        // reporting that they had punched: setCompRegion clamps a region to the
        // folder and drops what is left for being too short. The second one is
        // the nastier of the two, because clamping left it a sliver half a
        // crossfade wide at the very end of the clip, so it would have put the
        // punched take over the last five milliseconds of the performance.
        check (! Punch::regionFor ({ 2.0, 4.0, true, true }, 100.0, 120.0, mat).valid,
               "a punch entirely before the clip produced a region");
        check (! Punch::regionFor ({ 20.0, 24.0, true, true }, 0.0, 120.0, mat).valid,
               "a punch starting exactly where the clip ends produced a region");
        check (! Punch::regionFor ({ 24.0, 28.0, true, true }, 0.0, 120.0, mat).valid,
               "a punch entirely after the clip produced a region");

        // Overhanging the clip's start is a real punch, though: what lands on
        // the clip is replaced and the rest of the range has nothing to do.
        // There is no join at that end, because the folder's own edge never
        // gets one.
        {
            Scene sc;
            sc.folderSeconds = 10.0;
            sc.bpm = 120.0;
            sc.material = { -3.0, 8.0, 10.0 };
            sc.range = { -4.0, 4.0, true, true };     // minus 2 s to plus 2 s
            sc.inSec = 0.0; sc.outSec = 2.0;

            const auto over = Punch::regionFor (sc.range, 0.0, sc.bpm, sc.material);
            check (over.valid, "a punch overhanging the clip's start was refused");

            std::vector<CompSegment> compOver { { 0.0, 0 } };
            const auto spansOver = compFor (sc, over, compOver);
            checkClose (gainOf (spansOver, 1, 0.0), 1.0, 1.0e-12,
                        "an overhanging punch is not at full level at the clip's start");
            checkClose (gainOf (spansOver, 1, 1.999), 1.0, 1.0e-9,
                        "an overhanging punch is not at full level up to its punch out");
            checkClose (gainOf (spansOver, 0, 3.0), 1.0, 1.0e-9,
                        "the old performance did not come back after an overhanging punch");
        }

        // A zero width crossfade is a butt join, which is what three of the
        // six studios do at a punch boundary. It is allowed, and the punch is
        // still exact across itself.
        const auto r = Punch::regionFor (good, 0.0, 120.0, mat, 0.0);
        check (r.valid, "a punch with no crossfade was refused");
        checkClose (r.crossfadeSeconds, 0.0, 1.0e-12, "a zero crossfade came back non zero");
        checkClose (r.from, 4.0, 1.0e-12, "a zero crossfade still shifted the boundary");
        checkClose (r.to, 6.0, 1.0e-12, "a zero crossfade still shifted the boundary");

        const auto rNan = Punch::regionFor (good, 0.0, 120.0, mat, nan);
        check (rNan.valid, "a crossfade of NaN should fall back rather than refuse the punch");
        checkClose (rNan.crossfadeSeconds, 0.0, 1.0e-12,
                    "a crossfade of NaN was not treated as none");

        // applyTo guards its take index, because a comp pointing at a take
        // that is not there plays the wrong pass rather than failing.
        std::vector<CompSegment> comp { { 0.0, 0 } };
        double ms = 10.0;
        const auto ok = Punch::regionFor (good, 0.0, 120.0, mat);
        check (! Punch::applyTo (comp, ms, ok, 2, 2, 10.0),
               "applyTo accepted a take index past the end of the folder");
        check (! Punch::applyTo (comp, ms, ok, -1, 2, 10.0),
               "applyTo accepted a negative take index");
        check (! Punch::applyTo (comp, ms, Punch::Result {}, 1, 2, 10.0),
               "applyTo accepted a region it had been told was invalid");
        check (comp == std::vector<CompSegment> { { 0.0, 0 } },
               "a refused punch changed the comp anyway");
        checkClose (ms, 10.0, 1.0e-12, "a refused punch changed the folder's crossfade anyway");
    }

    // ---------------------------------------------------------------------
    // WHERE A PASS SITS IN THE FOLDER IT IS PUNCHED INTO. This is the one
    // that was got wrong, and it was got wrong in the window where nothing
    // could see it: the pass was placed from where the transport rolled rather
    // than from the clip's own start, which put the audio along the folder by
    // the distance between the two and made the punch play silence, or three
    // bars of the wrong part of the performance at full level.
    //
    // The property is the only one that matters and it is worth stating
    // plainly: a sample that was played at a beat has to end up at that beat.
    // Everything else about a Placement follows from it.
    // ---------------------------------------------------------------------
    {
        std::mt19937 rng (76543u);
        std::uniform_real_distribution<double> unit (0.0, 1.0);

        int placed = 0, began = 0, beforeClip = 0;

        for (int trial = 0; trial < 4000; ++trial)
        {
            const double bpm       = 40.0 + unit (rng) * 200.0;
            const double clipStart = unit (rng) * 64.0;

            // The transport rolls in before the punch, so it lands either side
            // of the clip's own start depending on where the clip is.
            const double passStart = clipStart + (unit (rng) - 0.35) * 32.0;
            const double head      = unit (rng) * 0.4;
            const double duration  = head + 0.05 + unit (rng) * 30.0;

            const auto p = Punch::placePass (passStart, clipStart, bpm, head, duration);
            const std::string label = "placement trial " + std::to_string (trial);

            const double into = Punch::secondsInto (passStart, clipStart, bpm);
            if (into < 0.0)
                ++beforeClip;

            if (! p.valid)
            {
                // The only honest reason to refuse is that there is nothing
                // left of the pass once the part in front of the clip has been
                // taken off its front.
                check (into + duration - head <= 0.0 + 1.0e-9,
                       label + ": a pass with audio left in it was refused");
                continue;
            }

            ++placed;

            check (p.start >= 0.0, label + ": a take was placed before the folder it is in");
            check (p.offset >= head - 1.0e-12,
                   label + ": the placement read into the pass's own pre-roll");
            checkClose (p.offset + p.length, duration, 1.0e-9,
                        label + ": the placement does not run to the end of the file");

            if (p.start > 1.0e-9)
                ++began;

            // THE PROPERTY. Walk the pass and check that every sample is where
            // it was played. Written out rather than reduced to the algebra,
            // because the algebra is what was wrong.
            for (int i = 0; i <= 16; ++i)
            {
                const double y = p.length * (double) i / 16.0;

                // The beat this sample was played at: its position in the
                // source, less the pre-roll, after the pass started.
                const double playedAt = passStart + (p.offset + y - head) * bpm / 60.0;

                // Where it has to land in the folder.
                const double wanted = Punch::secondsInto (playedAt, clipStart, bpm);

                checkClose (p.start + y, wanted, 1.0e-6,
                            label + ": the sample played at beat " + std::to_string (playedAt)
                              + " is not where it was played");
            }
        }

        check (placed > 2000, "too few placements were checked: " + std::to_string (placed));
        check (began > 500, "almost no pass began after the clip it was punched into, which is "
                            "the ordinary case: " + std::to_string (began));
        check (beforeClip > 500, "almost no pass began before the clip it was punched into: "
                                   + std::to_string (beforeClip));

        std::printf ("  %d placements checked, %d starting inside the clip, %d before it\n",
                     placed, began, beforeClip);

        // And once by hand. 120 bpm, a clip at beat 8, a pass that rolled in at
        // beat 12 with a tenth of a second of pre-roll in front of it.
        const auto a = Punch::placePass (12.0, 8.0, 120.0, 0.1, 5.0);
        check (a.valid, "the worked placement was refused");
        checkClose (a.start, 2.0, 1.0e-12, "four beats at 120 bpm is not two seconds into the folder");
        checkClose (a.offset, 0.1, 1.0e-12, "the pass's own pre-roll was not kept in front of it");
        checkClose (a.length, 4.9, 1.0e-12, "the placement is not the rest of the file");

        // The same pass against a clip that starts later than it does: two
        // seconds of it are in front of the clip and come off its front.
        const auto b = Punch::placePass (12.0, 16.0, 120.0, 0.1, 5.0);
        check (b.valid, "a pass that began before the clip was refused");
        checkClose (b.start, 0.0, 1.0e-12, "a pass before the clip was given a negative position");
        checkClose (b.offset, 2.1, 1.0e-12, "the part in front of the clip was not taken off the front");
        checkClose (b.length, 2.9, 1.0e-12, "the length was not reduced with the offset");

        // Nothing left of it at all.
        check (! Punch::placePass (0.0, 64.0, 120.0, 0.0, 5.0).valid,
               "a pass entirely in front of the clip was placed anyway");
        check (! Punch::placePass (0.0, 0.0, 120.0, 6.0, 5.0).valid,
               "a pass shorter than its own pre-roll was placed anyway");
    }

    // ---------------------------------------------------------------------
    // The punch reaches only where the clip is and where the pass is. Four
    // cases, all of them things a player does, and each of them silent if the
    // clamping is missing: the comp would hand a stretch to a take with no
    // audio in it and `buildCompSpans` would clamp the read to nothing, so the
    // punch would replace part of the phrase with silence.
    // ---------------------------------------------------------------------
    {
        const Punch::Range r { 8.0, 16.0, true, true };     // 4 s to 8 s at 120 bpm

        // The player stopped halfway through the punch.
        {
            Scene sc;
            sc.folderSeconds = 20.0; sc.bpm = 120.0;
            sc.range = r; sc.inSec = 4.0; sc.outSec = 8.0;
            sc.material = { 2.0, 6.0, 20.0 };               // ran out at 6 s

            const auto got = Punch::regionFor (sc.range, 0.0, sc.bpm, sc.material);
            check (got.valid, "a punch the player stopped halfway through was refused");
            check (got.to <= 6.0 + 1.0e-9,
                   "the punch reaches past the end of the pass, so it replaces audio with nothing");

            std::vector<CompSegment> comp { { 0.0, 0 } };
            const auto spans = compFor (sc, got, comp);
            checkClose (gainOf (spans, 1, 5.0), 1.0, 1.0e-9,
                        "the punch is not at full level where the pass does reach");
            checkClose (gainOf (spans, 0, 7.0), 1.0, 1.0e-9,
                        "the old performance did not come back where the player stopped");
            checkClose (renderAt (spans, { 0.5, 1.0 }, 7.0), 0.5, 1.0e-9,
                        "the stretch past the end of the pass is not the original performance");
        }

        // The player started late, after the punch in had gone by.
        {
            Scene sc;
            sc.folderSeconds = 20.0; sc.bpm = 120.0;
            sc.range = r; sc.inSec = 4.0; sc.outSec = 8.0;
            sc.material = { 5.0, 12.0, 20.0 };              // nothing before 5 s

            const auto got = Punch::regionFor (sc.range, 0.0, sc.bpm, sc.material);
            check (got.valid, "a punch whose pass started late was refused");
            check (got.from >= 5.0 - 1.0e-9,
                   "the punch starts before the pass does, so it replaces audio with nothing");
            checkClose (got.crossfadeSeconds, 0.0, 1.0e-12,
                        "a join with no material in front of it was given a crossfade anyway");

            std::vector<CompSegment> comp { { 0.0, 0 } };
            const auto spans = compFor (sc, got, comp);
            checkClose (renderAt (spans, { 0.5, 1.0 }, 4.5), 0.5, 1.0e-9,
                        "the stretch before the pass began is not the original performance");
            checkClose (renderAt (spans, { 0.5, 1.0 }, 6.0), 1.0, 1.0e-9,
                        "the punch is not heard where the pass does reach");
        }

        // The punch hangs off the end of the clip, and only a sliver of it
        // lands. The crossfade may not be wider than what is really replaced.
        {
            Scene sc;
            sc.folderSeconds = 4.003;  sc.bpm = 120.0;      // the clip ends 3 ms into the punch
            sc.range = r; sc.inSec = 4.0; sc.outSec = 8.0;
            sc.material = { 0.0, 12.0, 4.003 };

            const auto got = Punch::regionFor (sc.range, 0.0, sc.bpm, sc.material);
            check (got.valid, "a punch overhanging the clip's end was refused");
            check (got.crossfadeSeconds <= 0.003 + 1.0e-9,
                   "the crossfade is wider than the part of the punch that lands on the clip, so "
                   "it replaces more of the performance than was punched");
        }

        // A punch that begins before the clip, played by a pass that also began
        // before it. The join at the in point is on the folder's own edge, so
        // the comp model gives it no fade, and the one at the out point must
        // still get its full width: the lead the pass has in front of the
        // folder is lead, and narrowing the only width the folder has because
        // of it would shorten the join at the other end for no reason.
        {
            Scene sc;
            sc.folderSeconds = 20.0; sc.bpm = 120.0;
            sc.range = { -4.0, 8.0, true, true };       // minus 2 s to plus 4 s
            sc.inSec = 0.0; sc.outSec = 4.0;
            // The pass rolled in only 5 ms before the punch in, but the punch
            // in is before the clip, so what the join at the other end can
            // have is governed by what the pass has in front of the *folder*
            // and not by what it has in front of a punch point the comp will
            // never put a boundary at. Measured from the wrong one, the only
            // width the folder has comes back at 5 ms instead of 10.
            sc.material = { -2.005, 12.0, 20.0 };

            const auto got = Punch::regionFor (sc.range, 0.0, sc.bpm, sc.material);
            check (got.valid, "a punch beginning before the clip was refused");
            checkClose (got.crossfadeSeconds, Punch::kDefaultCrossfadeMs * 0.001, 1.0e-12,
                        "the join at the punch out was narrowed by lead the pass had in front "
                        "of the clip, which no join needs");

            std::vector<CompSegment> comp { { 0.0, 0 } };
            const auto spans = compFor (sc, got, comp);
            checkClose (gainOf (spans, 1, 0.0), 1.0, 1.0e-12,
                        "the punch is not at full level at the folder's start");
            checkClose (gainOf (spans, 1, 4.0 - 1.0e-9), 1.0, 1.0e-9,
                        "the punch is not at full level up to its punch out");
            checkClose (gainOf (spans, 0, 4.011), 1.0, 1.0e-9,
                        "the old performance did not come back after the punch out");
        }

        // And the floor on a punch at all, which the header claims is the same
        // figure the comp refuses a segment shorter than.
        {
            Scene sc;
            sc.folderSeconds = 20.0; sc.bpm = 120.0;
            sc.material = { 0.0, 20.0, 20.0 };

            const double justUnder = kMinCompSpan * 0.5 * 120.0 / 60.0;
            const double justOver  = kMinCompSpan * 4.0 * 120.0 / 60.0;

            check (! Punch::regionFor ({ 8.0, 8.0 + justUnder, true, true },
                                       0.0, sc.bpm, sc.material).valid,
                   "a punch shorter than the comp's own minimum segment was accepted");
            check (Punch::regionFor ({ 8.0, 8.0 + justOver, true, true },
                                     0.0, sc.bpm, sc.material).valid,
                   "a punch longer than the comp's own minimum segment was refused");
        }
    }

    // ---------------------------------------------------------------------
    // One sided punches, which is Ardour's In only and Out only.
    // ---------------------------------------------------------------------
    {
        Scene sc;
        sc.folderSeconds = 10.0;
        sc.bpm = 120.0;
        sc.material = { 2.0, 11.0, 10.0 };

        // In only: records from the marker on, with no end boundary. One join,
        // at the marker, and nothing at the folder's end.
        sc.range = { 8.0, 0.0, true, false };
        sc.inSec = 4.0; sc.outSec = sc.folderSeconds;
        const auto in = Punch::regionFor (sc.range, 0.0, sc.bpm, sc.material);
        check (in.valid, "In only was refused");
        checkClose (in.from, 4.0 - 0.005, 1.0e-12, "In only did not shift its one boundary");
        // Past the folder's end, which `setCompRegion` clamps back: the shift is
        // applied at both ends and an end that has fallen on the folder's own
        // edge gets no fade from the comp model anyway.
        checkClose (in.to, 10.0 + 0.005, 1.0e-12, "In only did not run to the end of the folder");

        std::vector<CompSegment> compIn { { 0.0, 0 } };
        const auto spansIn = compFor (sc, in, compIn);
        checkClose (gainOf (spansIn, 1, 4.0), 1.0, 1.0e-12,
                    "In only is not at full level at its marker");
        checkClose (gainOf (spansIn, 1, 9.999), 1.0, 1.0e-9,
                    "In only stopped before the end of the folder");
        check (compIn.size() == 2, "In only is not two comp segments");

        // Out only: records until the marker, with no beginning boundary.
        sc.range = { 0.0, 12.0, false, true };
        sc.inSec = 0.0; sc.outSec = 6.0;
        sc.material = { -1.0, 8.0, 10.0 };   // rolled from before the clip
        const auto out = Punch::regionFor (sc.range, 0.0, sc.bpm, sc.material);
        check (out.valid, "Out only was refused");
        checkClose (out.from, -0.005, 1.0e-12,
                    "Out only did not start at the folder, before the clamp");
        checkClose (out.to, 6.0 + 0.005, 1.0e-12, "Out only did not shift its one boundary");

        std::vector<CompSegment> compOut { { 0.0, 0 } };
        const auto spansOut = compFor (sc, out, compOut);
        checkClose (gainOf (spansOut, 1, 0.0), 1.0, 1.0e-12,
                    "Out only is not at full level at the start of the folder");
        checkClose (gainOf (spansOut, 1, 5.999), 1.0, 1.0e-9,
                    "Out only is not at full level up to its marker");
        checkClose (gainOf (spansOut, 0, 6.5), 1.0, 1.0e-9,
                    "the old performance did not come back after Out only");
    }

    // ---------------------------------------------------------------------
    // THE FOLDER A PUNCH IS WORKED OUT AGAINST HAS TO BE THE FOLDER IT LANDS
    // IN.
    //
    // `regionFor` clamps the punch to the length of the folder it is told
    // about, so that figure has to be the one the folder actually ends up
    // with. A folder is as long as its longest take reaches, and a punch pass
    // reaches its own position plus its own length: the transport rolls in
    // before the punch and the clip began earlier still, so a punch pass
    // always sits some way into the folder, and whenever it runs past the end
    // of the clip that was there the folder grows.
    //
    // Told a folder shorter than the one it lands in, the punch is clipped at
    // the old clip's end and the rest of it is handed back to a take with no
    // audio in it. That is silence, in playback and in the bounce alike, while
    // the punch reports that it happened and the pass sits in the folder
    // holding the performance. This is the fault `Punch::passReach` exists to
    // prevent, and it had shipped.
    //
    // `longestSeconds` is written out here rather than called, because it is
    // the authority `passReach` has to agree with: calling the thing under
    // test to work out what to expect of it would prove only that it equals
    // itself.
    // ---------------------------------------------------------------------
    {
        auto longestSeconds = [] (const std::vector<CompTakeSpan>& takes)
        {
            double longest = 0.0;
            for (const auto& t : takes)
                longest = std::max (longest, std::max (0.0, t.start) + t.length);
            return longest;
        };

        // The worked case, which is the measurement in passReach's comment: a
        // ten second clip punched from nine to twelve seconds, with the pass
        // occupying folder seconds eight to thirteen. Two seconds of the take
        // just punched in were silent before this was fixed.
        {
            const std::vector<CompTakeSpan> takes { { 0.0, 10.0 },      // the original
                                                    { 8.0,  5.0 } };    // the punched pass
            const double folderSeconds = longestSeconds (takes);
            checkClose (folderSeconds, 13.0, 1.0e-12,
                        "a pass reaching thirteen seconds did not make a thirteen second folder");

            checkClose (Punch::passReach (takes[1].start, takes[1].length), 13.0, 1.0e-12,
                        "passReach disagrees with the length the folder is sized to");

            const Punch::Range range { 18.0, 24.0, true, true };   // 9 s to 12 s at 120 bpm
            const Punch::Material material { takes[1].start, takes[1].end(), folderSeconds };
            const auto r = Punch::regionFor (range, 0.0, 120.0, material);
            check (r.valid, "the worked overhanging punch was refused");

            std::vector<CompSegment> comp { { 0.0, 0 } };
            double crossfadeMs = 10.0;
            check (Punch::applyTo (comp, crossfadeMs, r, 1, 2, folderSeconds),
                   "the worked overhanging punch was not applied");

            const auto spans = buildCompSpans (comp, takes, folderSeconds, crossfadeMs * 0.001);

            // Every one of these read nought before the fix, from ten seconds
            // on: the punch stopped at the end of the clip that was there.
            for (double t : { 9.5, 10.5, 11.0, 11.9, 11.999 })
            {
                checkClose (gainOf (spans, 1, t), 1.0, 1.0e-9,
                            "the overhanging punch is not at full level at " + std::to_string (t));
                checkClose (gainOf (spans, 0, t), 0.0, 1.0e-9,
                            "the old performance is still sounding inside the overhanging punch at "
                              + std::to_string (t));
            }
        }

        // Randomised, with the pass deliberately allowed to run past the end
        // of the clip it is punching into, which is what the generator above
        // never does: there `material.folderSeconds` is the clip's own length
        // and the folder never grows. That is the blind spot the fault lived
        // in for five nights.
        std::mt19937 rng (20261112u);
        std::uniform_real_distribution<double> unit (0.0, 1.0);

        int checked = 0, grew = 0;

        for (int trial = 0; trial < 4000; ++trial)
        {
            const double clipSeconds = 1.0 + unit (rng) * 20.0;
            const double bpm         = 40.0 + unit (rng) * 200.0;
            const double clipStart   = unit (rng) * 64.0;

            // Where the transport rolled in, and how long the player kept
            // going. Two thirds of the time they play past the end of the
            // clip, which is what makes the folder grow.
            const double passStart  = unit (rng) * clipSeconds * 0.8;
            const double passLength = (trial % 3 == 0 ? 0.2 + unit (rng) * 2.0
                                                      : 1.0 + unit (rng) * 20.0);

            const std::vector<CompTakeSpan> takes { { 0.0, clipSeconds },
                                                   { passStart, passLength } };
            const double folderSeconds = longestSeconds (takes);

            // The figure the caller now works the folder's length out with has
            // to be the same one.
            checkClose (Punch::passReach (passStart, passLength),
                        std::max (0.0, passStart) + passLength, 1.0e-12,
                        "passReach is not a pass's position plus its length");

            // And over the signs this loop never generates. A pass placed by
            // `placePass` is never at a negative position, but
            // `longestSeconds` walks every take in the folder including ones a
            // caller placed itself, and the two have to agree on all of them
            // or the whole point of the function is lost. Floored on the
            // position and not on the length, exactly as `longestSeconds` is:
            // two deliberate breakages of those floors survived a version of
            // this check that only ever fed it positives.
            {
                const double awkwardStart  = -5.0 + unit (rng) * 10.0;
                const double awkwardLength = -5.0 + unit (rng) * 10.0;

                checkClose (Punch::passReach (awkwardStart, awkwardLength),
                            std::max (0.0, awkwardStart) + awkwardLength, 1.0e-12,
                            "passReach and longestSeconds disagree at "
                              + std::to_string (awkwardStart) + ", "
                              + std::to_string (awkwardLength));
            }

            if (folderSeconds > clipSeconds + 1.0e-9)
                ++grew;

            // A punch somewhere inside the pass, so there is always material
            // for it, and often past the end of the old clip.
            const double a = passStart + unit (rng) * passLength;
            const double b = passStart + unit (rng) * passLength;
            const double inSec  = std::min (a, b);
            const double outSec = std::max (a, b);

            if (outSec - inSec < 1.0e-3)
                continue;

            const Punch::Range range { clipStart + inSec  * bpm / 60.0,
                                       clipStart + outSec * bpm / 60.0, true, true };
            const Punch::Material material { passStart, passStart + passLength, folderSeconds };

            const auto r = Punch::regionFor (range, clipStart, bpm, material);
            if (! r.valid)
                continue;

            std::vector<CompSegment> comp { { 0.0, 0 } };
            double crossfadeMs = 10.0;
            if (! Punch::applyTo (comp, crossfadeMs, r, 1, 2, folderSeconds))
            {
                fail ("growth trial " + std::to_string (trial) + ": a valid punch was not applied");
                continue;
            }

            const auto spans = buildCompSpans (comp, takes, folderSeconds, crossfadeMs * 0.001);
            if (spans.empty())
            {
                fail ("growth trial " + std::to_string (trial) + ": no spans to play");
                continue;
            }

            // Where the punch can really reach, worked out from the folder the
            // comp is actually built over.
            const double reachFrom = std::max (std::max (inSec, 0.0), passStart);
            const double reachTo   = std::min (std::min (outSec, folderSeconds),
                                               passStart + passLength);
            if (reachTo - reachFrom < 1.0e-6)
                continue;

            ++checked;

            const int steps = 32;
            for (int i = 0; i <= steps; ++i)
            {
                const double t = i == steps ? reachTo - 1.0e-9
                                            : reachFrom + (reachTo - reachFrom) * (double) i / (double) steps;

                checkClose (gainOf (spans, 1, t), 1.0, 1.0e-9,
                            "growth trial " + std::to_string (trial)
                              + ": the punch is not at full level at " + std::to_string (t));

                // And it replaced what was under it rather than summing with
                // it, read as the engine reads it.
                checkClose (renderAt (spans, { 0.5, 1.0 }, t), 1.0, 1.0e-9,
                            "growth trial " + std::to_string (trial)
                              + ": the punch did not replace what was under it at " + std::to_string (t));
            }
        }

        check (checked > 2000, "too few growing folders were checked: " + std::to_string (checked));
        check (grew > 1000, "almost no generated pass ran past the end of the clip it was punched "
                            "into, so the case the fault lived in was not exercised: "
                              + std::to_string (grew));
        std::printf ("  %d punches into a growing folder checked, %d where the folder grew\n",
                     checked, grew);
    }

    // ---------------------------------------------------------------------
    // A NEGATIVE CROSSFADE WIDTH IS NO WIDTH.
    //
    // `regionFor` takes the width as a parameter with a default, so a caller
    // can hand it anything, and Pro Tools exposes exactly this figure as a
    // preference. Left unfloored, a negative width makes `half` negative and
    // the two boundaries are then shifted *into* the punch rather than out of
    // it: the take that was there plays at full level for half a width at each
    // end of the punch, with no fade to explain it. The mutation that removed
    // the floor survived the whole suite, because nothing passed a negative.
    // ---------------------------------------------------------------------
    {
        const Punch::Material mat { 2.0, 8.0, 10.0 };
        const Punch::Range range { 8.0, 12.0, true, true };    // 4 s to 6 s at 120 bpm

        const auto r = Punch::regionFor (range, 0.0, 120.0, mat, -0.02);

        check (r.valid, "a negative crossfade should fall back rather than refuse the punch");
        checkClose (r.crossfadeSeconds, 0.0, 1.0e-12,
                    "a negative crossfade width came back negative");
        checkClose (r.from, 4.0, 1.0e-12,
                    "a negative crossfade shifted the in boundary into the punch");
        checkClose (r.to, 6.0, 1.0e-12,
                    "a negative crossfade shifted the out boundary into the punch");

        // And heard: the punch is still alone and at full level from end to
        // end, which is what the shift going the wrong way would break.
        const std::vector<CompTakeSpan> takes { { 0.0, 10.0 }, { 2.0, 6.0 } };
        std::vector<CompSegment> comp { { 0.0, 0 } };
        double crossfadeMs = 10.0;
        check (Punch::applyTo (comp, crossfadeMs, r, 1, 2, 10.0),
               "a punch with a negative crossfade was not applied");
        checkClose (crossfadeMs, 0.0, 1.0e-12,
                    "the folder kept a crossfade a negative width was not built for");

        const auto spans = buildCompSpans (comp, takes, 10.0, crossfadeMs * 0.001);
        for (double t : { 4.0, 4.001, 5.0, 5.999 })
        {
            checkClose (gainOf (spans, 1, t), 1.0, 1.0e-9,
                        "a negative crossfade left the punch below full level at " + std::to_string (t));
            checkClose (gainOf (spans, 0, t), 0.0, 1.0e-9,
                        "a negative crossfade left the old performance sounding inside the punch at "
                          + std::to_string (t));
        }
    }

    // ---------------------------------------------------------------------
    // THE FIGURE IN THE HEADER FOR WIDENING A PUNCHED FOLDER AFTERWARDS.
    //
    // Punch.h says the geometry holds at the moment of punching rather than
    // for ever: widen a punched folder's crossfade afterwards and the fade
    // walks back into the punch, "which measures 1.8 dB off the attack at
    // 50 ms where 10 was asked for". That number was stated and nothing
    // reproduced it, so it is reproduced here. Nothing offers that control
    // today; this is here so that the day something does, the cost of it is
    // already written down and checked.
    // ---------------------------------------------------------------------
    {
        const Punch::Material mat { 2.0, 8.0, 10.0 };
        const Punch::Range range { 8.0, 12.0, true, true };    // 4 s to 6 s at 120 bpm

        const auto r = Punch::regionFor (range, 0.0, 120.0, mat);
        check (r.valid, "the punch behind the widening figure was refused");
        checkClose (r.crossfadeSeconds, 0.010, 1.0e-12, "the punch did not get the ten it asked for");

        const std::vector<CompTakeSpan> takes { { 0.0, 10.0 }, { 2.0, 6.0 } };
        std::vector<CompSegment> comp { { 0.0, 0 } };
        double crossfadeMs = 10.0;
        check (Punch::applyTo (comp, crossfadeMs, r, 1, 2, 10.0), "it was not applied");

        // At the width it was built for, the attack is untouched.
        {
            const auto spans = buildCompSpans (comp, takes, 10.0, 0.010);
            checkClose (gainOf (spans, 1, 4.0), 1.0, 1.0e-9,
                        "the punch is not at full level at its own in point");
        }

        // Widened to 50 ms afterwards, with the boundaries still where 10 put
        // them, the fade reaches 20 ms into the punch and the in point sits
        // three fifths of the way up it.
        {
            const auto spans = buildCompSpans (comp, takes, 10.0, 0.050);
            const double g  = gainOf (spans, 1, 4.0);
            const double dB = 20.0 * std::log10 (g);

            checkClose (dB, -1.84, 0.05,
                        "widening a punched folder to 50 ms does not cost the attack the 1.8 dB "
                        "the header says it does");
            std::printf ("  widening a punched folder from 10 ms to 50 ms costs the attack %.2f dB\n", dB);
        }
    }

    if (failures == 0)
        std::printf ("  ok\n");
    else
        std::printf ("  %d failure(s)\n", failures);

    return failures == 0 ? 0 : 1;
}
