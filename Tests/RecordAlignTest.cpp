// ---------------------------------------------------------------------------
// Checks that a recorded take lands where it was played.
//
// This is arithmetic with no symptom. A take written down at the wrong
// position does not error, does not crackle and does not move a meter: it is
// simply a few milliseconds behind the part it was played against, which
// sounds like a player who cannot keep time. Worse, the amount depends on the
// driver, the buffer size and which plugins happen to be loaded, so two
// sessions disagree and neither looks wrong.
//
// So the property is asserted directly rather than at a few hand picked
// points, and over thousands of generated recordings including the degenerate
// ones: loops shorter than one buffer, wrap lists that arrive out of order,
// captures shorter than the offset itself, tempos and beats that are not
// numbers, and offsets of both signs. The negative sign has a check of its
// own rather than sharing the property below, because it is handled
// differently on purpose: see RecordAlign::passes.
//
// The property, in one sentence: capture frame f ends up at the beat the
// transport was at when f arrived, less the offset. Everything else here is a
// consequence of that one, and the end to end check at the bottom states it
// the way a musician would, which is that a note played on the beat lands on
// the beat.
//
// RecordAlign.h has no JUCE in it so this builds and runs on its own with
// sanitizers in a few seconds. See the header for where the behaviour came
// from.
// ---------------------------------------------------------------------------

#include "RecordAlign.h"

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

static void checkEqual (long long a, long long b, const std::string& what)
{
    if (a != b)
        fail (what + "  (" + std::to_string (a) + " vs " + std::to_string (b) + ")");
}

static void checkClose (double a, double b, double tol, const std::string& what)
{
    if (! (std::abs (a - b) <= tol))
        fail (what + "  (" + std::to_string (a) + " vs " + std::to_string (b)
                    + ", off by " + std::to_string (std::abs (a - b)) + ")");
}

// ---------------------------------------------------------------------------
// The transport position at which a given frame of the capture arrived,
// worked out from the raw wrap list the audio thread noted, with no offset
// applied. This is the "truth" the alignment is measured against, and it is
// written independently of the code under test on purpose: a helper that
// shared RecordAlign's own stepping would agree with it about any mistake
// they both made.

static double rawBeatAtFrame (int totalFrames, double rawStartBeat,
                              const std::vector<RecordAlign::Wrap>& wraps,
                              int frame, double beatsPerSample)
{
    int    boundary = 0;
    double beat     = rawStartBeat;

    for (const auto& w : wraps)
    {
        const int at = std::clamp (w.frame, boundary, totalFrames);

        if (at > frame)
            break;

        boundary = at;
        beat     = w.beat;
    }

    return beat + (double) (frame - boundary) * beatsPerSample;
}

// ---------------------------------------------------------------------------
// Properties every split has to satisfy, whatever it was handed.

static void checkPassInvariants (const std::vector<RecordAlign::Pass>& passes,
                                 int totalFrames, int offsetSamples,
                                 const std::string& label)
{
    const int dropped = std::min (std::max (0, offsetSamples), totalFrames);

    if (totalFrames - dropped <= 0)
    {
        check (passes.empty(), label + ": a capture shorter than the offset should keep nothing");
        return;
    }

    check (! passes.empty(), label + ": a capture with audio left in it produced no passes at all");
    if (passes.empty())
        return;

    checkEqual (passes.back().lastFrame, totalFrames,
                label + ": the last pass does not run to the end of the capture");

    int covered = 0;

    for (size_t i = 0; i < passes.size(); ++i)
    {
        const auto& p = passes[i];

        check (p.audibleFrames() > 0, label + ": a pass with nothing audible in it was returned");
        check (p.firstFrame >= 0 && p.lastFrame <= totalFrames,
               label + ": a pass reads outside the capture");
        check (p.preRollFrames >= 0 && p.preRollFrames < p.frames(),
               label + ": a pass's pre-roll is not inside the pass");
        check (p.startBeat >= 0.0, label + ": a pass was placed before the start of the arrangement");

        if (i == 0)
        {
            // Only the first pass may carry pre-roll, and only from the very
            // start of the capture. For every pass after it, the audio in
            // front of its start is the previous pass's tail and is already
            // in the previous pass's own file: duplicating it would mean one
            // performance in two files.
            if (p.preRollFrames > 0)
                checkEqual (p.firstFrame, 0,
                            label + ": pre-roll was kept somewhere other than the capture's start");
        }
        else
        {
            checkEqual (p.firstFrame, passes[i - 1].lastFrame,
                        label + ": a gap or an overlap between two passes");
            checkEqual (p.preRollFrames, 0,
                        label + ": a pass after the first was given pre-roll of its own");
        }

        covered += p.audibleFrames();
    }

    // Every frame from the first audible one to the end of the capture is
    // heard exactly once. Anything else means audio played once is heard
    // twice, or a hole in the middle of a performance.
    checkEqual (covered, totalFrames - passes.front().audibleFrom(),
                label + ": the passes do not account for every audible frame");

    check (passes.front().audibleFrom() <= dropped,
           label + ": more was put in front of the first pass than the offset asked for");
}

// ---------------------------------------------------------------------------
// Generates a recording: a length, a tempo, a start, and a wrap list with
// some out of order boundaries in it.

struct Recording
{
    int    totalFrames = 0;
    double startBeat   = 0.0;
    double bps         = 0.0;
    std::vector<RecordAlign::Wrap> wraps;
};

static Recording generate (std::mt19937& rng)
{
    std::uniform_int_distribution<int> totalDist   (1, 4000);
    std::uniform_int_distribution<int> wrapDist    (0, 12);
    std::uniform_int_distribution<int> disorder    (0, 4);
    std::uniform_real_distribution<double> beatDist (0.0, 400.0);
    std::uniform_real_distribution<double> bpmDist  (20.0, 400.0);
    std::uniform_int_distribution<int> rateDist    (0, 3);

    const double rates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };

    Recording r;
    r.totalFrames = totalDist (rng);
    r.startBeat   = beatDist (rng);
    r.bps         = bpmDist (rng) / 60.0 / rates[(size_t) rateDist (rng)];

    const int numWraps = wrapDist (rng);
    int at = 0;

    for (int i = 0; i < numWraps; ++i)
    {
        std::uniform_int_distribution<int> step (0, std::max (1, r.totalFrames / std::max (1, numWraps)));
        at += step (rng);

        // Every so often, hand it a boundary that has gone backwards. The
        // audio thread clamps what it writes, but a capture that dropped
        // frames can still produce an ordering nobody designed, and a pass
        // reading backwards is a crash rather than a wrong answer.
        r.wraps.push_back ({ disorder (rng) == 0 ? at - step (rng) : at, beatDist (rng) });
    }

    return r;
}

// ---------------------------------------------------------------------------
// The one property. Over thousands of generated recordings.
//
// A frame arriving `offset` samples after the studio rendered the beat it was
// played against belongs at that beat. So the frame that arrived at capture
// position f belongs wherever the frame at f - offset would have been written
// down with no compensation at all, which is what rawBeatAtFrame computes from
// the wrap list independently of the code under test.

static void alignmentHoldsEverywhere()
{
    std::mt19937 rng (20261008u);
    std::uniform_int_distribution<int> offsetDist (0, 3000);

    for (int trial = 0; trial < 2500; ++trial)
    {
        const auto r      = generate (rng);
        const int  offset = offsetDist (rng);

        const auto passes = RecordAlign::passes (r.totalFrames, r.startBeat, r.wraps.data(),
                                                 (int) r.wraps.size(), offset, r.bps);

        const std::string label = "trial " + std::to_string (trial);
        checkPassInvariants (passes, r.totalFrames, offset, label);

        if (failures > 0)
            return;         // one report is enough; thousands is noise

        for (const auto& p : passes)
        {
            // Over the whole of the pass's file, pre-roll included: a frame in
            // front of the pass's start has a place too, before that start,
            // and that is what the clip's left edge can be dragged back over.
            for (int frame = p.firstFrame; frame < p.lastFrame; ++frame)
            {
                const double placed = p.startBeat + (double) (frame - p.audibleFrom()) * r.bps;
                const double wanted = rawBeatAtFrame (r.totalFrames, r.startBeat, r.wraps,
                                                      frame - offset, r.bps);

                // A tolerance in beats scaled to the answer: the two sides add
                // the same products in a different order, so what is left is
                // floating point ordering and nothing else.
                checkClose (placed, wanted, 1.0e-9 + std::abs (wanted) * 1.0e-12,
                            label + ": frame " + std::to_string (frame)
                              + " is not placed at the beat it was played at");

                if (failures > 0)
                    return;
            }

            // And the helper in the header agrees with the arithmetic above,
            // since that is what the end to end check below leans on.
            checkClose (RecordAlign::placementOf (passes, p.audibleFrom(), r.bps), p.startBeat,
                        1.0e-12, "placementOf disagrees about where a pass starts");
        }

        // A sentinel rather than "negative", because a pre-roll frame has a
        // genuinely negative place when the take starts near the top of the
        // arrangement.
        for (const int outside : { -1, r.totalFrames, r.totalFrames + 1 })
            checkEqual ((long long) RecordAlign::placementOf (passes, outside, r.bps, -1.0e9),
                        (long long) -1.0e9,
                        "placementOf claims to have placed a frame outside the capture");
    }
}

// ---------------------------------------------------------------------------
// The other direction, which only a trim bigger than the whole round trip can
// ask for: the take belongs later than it was captured, and there is no way to
// drop frames backwards, so every stamp moves instead and no audio is lost.
//
// Measured against the same recording compensated by nothing, which the check
// above has already pinned to the wrap list.

static void aNegativeOffsetMovesTheWholeTakeLater()
{
    std::mt19937 rng (991u);
    std::uniform_int_distribution<int> offsetDist (-3000, -1);

    for (int trial = 0; trial < 2000; ++trial)
    {
        const auto r      = generate (rng);
        const int  offset = offsetDist (rng);

        const auto moved = RecordAlign::passes (r.totalFrames, r.startBeat, r.wraps.data(),
                                                (int) r.wraps.size(), offset, r.bps);
        const auto plain = RecordAlign::passes (r.totalFrames, r.startBeat, r.wraps.data(),
                                                (int) r.wraps.size(), 0, r.bps);

        const std::string label = "late trial " + std::to_string (trial);
        checkPassInvariants (moved, r.totalFrames, offset, label);

        checkEqual ((long long) moved.size(), (long long) plain.size(),
                    label + ": moving a take later changed how it was split");

        if (failures > 0)
            return;

        const double shift = (double) -offset * r.bps;

        for (size_t i = 0; i < moved.size(); ++i)
        {
            checkEqual (moved[i].firstFrame, plain[i].firstFrame,
                        label + ": moving a take later dropped audio off the front");
            checkEqual (moved[i].preRollFrames, 0,
                        label + ": moving a take later invented pre-roll for it");
            checkEqual (moved[i].lastFrame, plain[i].lastFrame,
                        label + ": moving a take later shortened it");
            checkClose (moved[i].startBeat, plain[i].startBeat + shift,
                        1.0e-9 + std::abs (plain[i].startBeat) * 1.0e-12,
                        label + ": a pass did not move later by the offset");

            if (failures > 0)
                return;
        }
    }
}

// ---------------------------------------------------------------------------
// The offset itself: what it is made of, and that each part of it counts.

static void offsetIsTheSumOfItsParts()
{
    RecordAlign::Figures f;
    f.sampleRate = 48000.0;

    f.inputLatency  = 240;
    f.outputLatency = 264;
    f.engineLatency = 0;
    checkEqual (RecordAlign::captureOffsetSamples (f), 504,
                "a reported round trip is not passed through");

    f.engineLatency = 1024;
    checkEqual (RecordAlign::captureOffsetSamples (f), 1528,
                "the studio's own compensation is not counted");

    // A driver that reports nothing is not a driver with no latency. Two
    // buffers is the floor any round trip can have, which understates a real
    // one and is exactly why there is a trim.
    f.inputLatency = f.outputLatency = 0;
    f.engineLatency = 0;
    f.bufferSize = 128;
    checkEqual (RecordAlign::captureOffsetSamples (f), 256,
                "a driver that reported nothing was taken at its word");

    f.bufferSize = 0;
    checkEqual (RecordAlign::captureOffsetSamples (f), 0,
                "with nothing reported and no buffer there is nothing to guess from");

    // The trim, in both directions, and at the sample rate it is given.
    f.inputLatency  = 240;
    f.outputLatency = 264;
    f.trimMs = 10.0;
    checkEqual (RecordAlign::captureOffsetSamples (f), 504 + 480,
                "a positive trim does not pull the take earlier");

    f.trimMs = -10.0;
    checkEqual (RecordAlign::captureOffsetSamples (f), 504 - 480,
                "a negative trim does not push the take later");

    f.sampleRate = 96000.0;
    checkEqual (RecordAlign::captureOffsetSamples (f), 504 - 960,
                "the trim is not being read in milliseconds");

    // Out of range, not a number, and a sample rate that is neither: all of
    // these can be in a settings file, none of them may reach the conversion
    // to int as they are.
    f.sampleRate = 48000.0;
    f.trimMs = 1.0e9;
    checkEqual (RecordAlign::captureOffsetSamples (f),
                504 + (int) std::lround (RecordAlign::maxTrimMs * 48.0),
                "a trim beyond the range was not clamped to it");

    // A trim that is not a number. Note for anybody mutation testing this:
    // removing the std::isfinite guard in captureOffsetSamples does not fail
    // here on glibc, because std::lround of a NaN happens to return zero,
    // which is the same answer the guard gives. The standard leaves that
    // return value unspecified and this studio also ships MSVC and Apple
    // libm builds, so the guard stays whatever the mutation score says.
    f.trimMs = std::nan ("");
    checkEqual (RecordAlign::captureOffsetSamples (f), 504,
                "a trim that is not a number was not treated as no trim");

    f.trimMs = 10.0;
    f.sampleRate = 0.0;
    checkEqual (RecordAlign::captureOffsetSamples (f), 504,
                "a trim was converted with no sample rate to convert it by");

    f.sampleRate = -48000.0;
    checkEqual (RecordAlign::captureOffsetSamples (f), 504,
                "a negative sample rate turned the trim around");

    // A driver that answers for one direction and goes quiet about the other
    // is not a driver with no latency on the quiet side, and no round trip can
    // be shorter than one buffer in and one out whatever is claimed.
    f.trimMs        = 0.0;
    f.engineLatency = 0;
    f.inputLatency  = 240;
    f.outputLatency = 0;
    f.bufferSize    = 480;
    checkEqual (RecordAlign::captureOffsetSamples (f), 960,
                "a one sided report was believed on the side it went quiet about");

    // And a report above that floor is used as reported rather than raised.
    f.inputLatency  = 2000;
    f.outputLatency = 2000;
    checkEqual (RecordAlign::captureOffsetSamples (f), 4000,
                "a round trip above the floor was dragged down or up to it");

    f.inputLatency  = 240;
    f.outputLatency = 264;
    f.bufferSize    = 0;

    // Rounded rather than truncated, so a trim smaller than one sample still
    // moves by one rather than by nothing.
    f.sampleRate = 48000.0;
    f.trimMs = 0.02;                    // 0.96 samples at 48 kHz
    checkEqual (RecordAlign::captureOffsetSamples (f), 504 + 1,
                "a sub sample trim was truncated away instead of rounded");

    // A negative figure out of the mixer is not a reason to push a take
    // later. It cannot happen while the latency graph is right, which is
    // exactly why it is worth refusing here rather than trusting.
    f.trimMs = 0.0;
    f.engineLatency = -2000;
    checkEqual (RecordAlign::captureOffsetSamples (f), 504,
                "a negative compensation figure was allowed to push the take later");
    f.engineLatency = 0;

    // Monotonic in every part of it, which is the shape of the sum and the
    // thing a sign error breaks.
    std::mt19937 rng (7u);
    std::uniform_int_distribution<int> small (0, 4096);
    std::uniform_real_distribution<double> trim (RecordAlign::minTrimMs, RecordAlign::maxTrimMs);

    for (int i = 0; i < 2000; ++i)
    {
        RecordAlign::Figures a;
        a.sampleRate    = 48000.0;
        a.inputLatency  = small (rng);
        a.outputLatency = small (rng);
        a.engineLatency = small (rng);
        a.bufferSize    = small (rng);
        a.trimMs        = trim (rng);

        auto b = a;
        b.engineLatency += 1 + small (rng);
        check (RecordAlign::captureOffsetSamples (b) > RecordAlign::captureOffsetSamples (a),
               "more plugin compensation did not move the take earlier");

        auto c = a;
        c.trimMs = std::min (RecordAlign::maxTrimMs, a.trimMs + 20.0);
        check (RecordAlign::captureOffsetSamples (c) >= RecordAlign::captureOffsetSamples (a),
               "a larger trim did not move the take earlier");

        // A negative report from a broken driver must never produce a
        // negative round trip, which would push takes later for no reason.
        auto d = a;
        d.inputLatency = -small (rng) - 1;
        d.outputLatency = 0;
        d.engineLatency = 0;
        d.trimMs = 0.0;
        check (RecordAlign::captureOffsetSamples (d) >= 0,
               "a driver reporting a negative latency pushed the take later");
    }
}

// ---------------------------------------------------------------------------
// The same thing said the way it matters: a note played on a beat lands on
// that beat.

static void aNotePlayedOnTheBeatLandsOnTheBeat()
{
    const double sampleRate = 48000.0;
    const double bpm        = 96.0;
    const double bps        = bpm / 60.0 / sampleRate;

    RecordAlign::Figures f;
    f.sampleRate    = sampleRate;
    f.inputLatency  = 288;
    f.outputLatency = 352;
    f.engineLatency = 1500;             // a lookahead limiter on the master
    const int offset = RecordAlign::captureOffsetSamples (f);

    // Recording starts at bar 5 of a four four arrangement, over a four bar
    // loop, three times round.
    const double recordAt  = 16.0;
    const double loopFrom  = 16.0;
    const double loopBeats = 16.0;
    const int    loopFrames = (int) std::lround (loopBeats / bps);

    std::vector<RecordAlign::Wrap> wraps { { loopFrames, loopFrom }, { 2 * loopFrames, loopFrom } };
    const int totalFrames = 3 * loopFrames;

    const auto passes = RecordAlign::passes (totalFrames, recordAt, wraps.data(),
                                             (int) wraps.size(), offset, bps);

    checkEqual ((long long) passes.size(), 3, "three times round the loop did not make three takes");

    for (const auto& p : passes)
        checkClose (p.startBeat, loopFrom, 1.0e-9,
                    "the passes of one loop recording are not all stamped at the loop start");

    // The player hears beat X and plays there. Their note enters the socket
    // `offset` samples after the studio rendered that beat, so it arrives in
    // the capture at the frame below. It has to come back out at X.
    for (const double target : { 16.0, 17.0, 19.5, 23.0, 31.999 })
    {
        const int frame = (int) std::lround ((target - recordAt) / bps) + offset;

        checkClose (RecordAlign::placementOf (passes, frame, bps), target, 1.0e-6,
                    "a note played on beat " + std::to_string (target) + " did not land there");
    }

    // Without the compensation it is late by exactly the round trip plus the
    // compensation, which is the bug this exists to fix. Stated here so that a
    // future change that quietly stops applying the offset fails loudly.
    const auto uncompensated = RecordAlign::passes (totalFrames, recordAt, wraps.data(),
                                                    (int) wraps.size(), 0, bps);

    const int frame = (int) std::lround ((20.0 - recordAt) / bps) + offset;
    checkClose (RecordAlign::placementOf (uncompensated, frame, bps),
                20.0 + (double) offset * bps, 1.0e-6,
                "the uncompensated case is not late by the offset, so the test is measuring nothing");

    check (RecordAlign::placementOf (uncompensated, frame, bps)
             > RecordAlign::placementOf (passes, frame, bps) + 0.01,
           "compensating made no audible difference at all, which cannot be right");
}

// ---------------------------------------------------------------------------
// The degenerate shapes, hand picked because random generation reaches them
// rarely and each one is a real recording somebody can make.

static void degenerateCaptures()
{
    const double bps = 120.0 / 60.0 / 48000.0;

    check (RecordAlign::passes (0, 4.0, nullptr, 0, 0, bps).empty(),
           "a capture with no audio in it produced a take");

    check (RecordAlign::passes (-5, 4.0, nullptr, 0, 0, bps).empty(),
           "a negative frame count produced a take");

    // A null list with a count, which is what a bug elsewhere looks like.
    check (RecordAlign::passes (1000, 4.0, nullptr, 7, 0, bps).size() == 1,
           "a null wrap list with a count was trusted");

    // The whole capture is shorter than the offset: everything in it belongs
    // before the record point, so there is nothing to keep. Better an empty
    // result than a take of the player's response to the count-in.
    check (RecordAlign::passes (100, 4.0, nullptr, 0, 500, bps).empty(),
           "a capture shorter than the offset still produced a take");

    // A loop shorter than one buffer wraps twice inside one block, so two
    // boundaries land on the same frame and the pass between them is empty.
    std::vector<RecordAlign::Wrap> doubled { { 64, 8.0 }, { 64, 8.0 } };
    const auto passes = RecordAlign::passes (256, 8.0, doubled.data(), 2, 0, bps);
    for (const auto& p : passes)
        check (p.frames() > 0, "an empty pass from a double wrap was returned rather than dropped");
    checkPassInvariants (passes, 256, 0, "double wrap");

    // A tempo that is not a number, which a corrupt project can hold. The
    // split still has to be a valid split.
    const auto nanTempo = RecordAlign::passes (256, 8.0, doubled.data(), 2, 32,
                                               std::nan (""));
    checkPassInvariants (nanTempo, 256, 32, "tempo that is not a number");

    const auto nanStart = RecordAlign::passes (256, std::nan (""), nullptr, 0, 32, bps);
    checkPassInvariants (nanStart, 256, 32, "start beat that is not a number");

    // A start or a wrap beat before the arrangement, which a loop range read
    // out of a project file can be. A take placed at a negative beat is a
    // take nobody can scroll to, so the arrangement's start is the floor.
    std::vector<RecordAlign::Wrap> early { { 64, -12.0 } };
    const auto before = RecordAlign::passes (256, -3.0, early.data(), 1, 0, bps);
    checkPassInvariants (before, 256, 0, "beats before the arrangement");
    for (const auto& p : before)
        check (p.startBeat >= 0.0, "a pass was placed before the start of the arrangement");

    // A negative offset keeps all the audio and moves every stamp later
    // instead, because there is no way to drop frames backwards.
    const auto late = RecordAlign::passes (1000, 4.0, nullptr, 0, -480, bps);
    checkEqual ((long long) late.size(), 1, "a negative offset lost the take");
    if (! late.empty())
    {
        checkEqual (late.front().firstFrame, 0, "a negative offset dropped audio off the front");
        checkEqual (late.front().lastFrame, 1000, "a negative offset shortened the take");
        checkEqual (late.front().preRollFrames, 0, "a negative offset invented pre-roll");
        checkClose (late.front().startBeat, 4.0 + 480.0 * bps, 1.0e-12,
                    "a negative offset did not move the take later");
    }
}

// ---------------------------------------------------------------------------
// The pre-roll is kept rather than dropped, which is what makes a badly set
// correction recoverable instead of permanent.

static void theHeadOfACaptureIsKeptRatherThanDropped()
{
    const double bps = 120.0 / 60.0 / 48000.0;

    const auto one = RecordAlign::passes (10000, 8.0, nullptr, 0, 480, bps);
    checkEqual ((long long) one.size(), 1, "an ordinary capture did not make one pass");
    if (one.empty())
        return;

    checkEqual (one.front().firstFrame, 0,
                "the head of the capture was dropped instead of kept as pre-roll");
    checkEqual (one.front().preRollFrames, 480,
                "the pre-roll is not the offset that was asked for");
    checkEqual (one.front().frames(), 10000,
                "the pass does not hold the whole of the capture");
    checkEqual (one.front().audibleFrames(), 10000 - 480,
                "the audible part of the pass is the wrong length");
    checkClose (one.front().startBeat, 8.0, 1.0e-12,
                "the pass is not written down at the beat recording began at");

    // Three times round a loop. Only the first pass has pre-roll: for the
    // others, the audio in front of their start is the previous pass's tail
    // and is already in the previous pass's file.
    const int loopFrames = 20000;
    std::vector<RecordAlign::Wrap> wraps { { loopFrames, 8.0 }, { 2 * loopFrames, 8.0 } };
    const auto looped = RecordAlign::passes (3 * loopFrames, 8.0, wraps.data(), 2, 480, bps);

    checkEqual ((long long) looped.size(), 3, "three times round the loop did not make three passes");

    for (size_t i = 0; i < looped.size(); ++i)
        checkEqual (looped[i].preRollFrames, i == 0 ? 480 : 0,
                    "the wrong pass of a loop recording carries the pre-roll");

    // And the passes between them hold every frame of the capture once, so
    // nothing captured is in two files and nothing is in none.
    int total = 0;
    for (const auto& p : looped)
        total += p.frames();
    checkEqual (total, 3 * loopFrames, "the passes do not hold the capture exactly once");
}

// ---------------------------------------------------------------------------
// THE MIDI HALF
//
// Same question as the audio half above, for a note played on a keyboard
// rather than a sound played into a socket, and it fails the same silent way:
// a part that is a few milliseconds behind the one it was played against,
// with nothing on screen to say so and an amount that depends on the driver,
// the buffer size and which plugins are loaded.
//
// The property, in one sentence, is the musician's one: a note played on the
// beat lands on the beat. aMidiNotePlayedOnTheBeatLandsOnTheBeat states it by
// walking one note from the player's hands to the beat the engine writes it
// down at, through the collector's own anchoring rather than through the
// formula under test, so the two sides are arrived at independently.
//
// Everything else here is either a part of that sum pinned to a literal, or
// the behaviour at an edge that the sum cannot speak for.
//
// One thing this file got wrong the first time, kept here because the shape of
// it is worth more than the fix. Every check placed a single event, and a rule
// that is right for every event on its own can still be wrong for a note: the
// loop wrapping that used to live in placeMidiEvent sent a note on to the top
// of the cycle and left its note off where it was, and this file blessed that,
// because it asked where one event goes and never where a note goes. An audit
// found it. aNoteSurvivesAsANote is the check that closes it, and it is the
// first thing to add to whenever a rule here grows a new case.

// The trim in samples, written out from the convention rather than read from
// the header. A test that takes its expected value from the constant it is
// testing asserts only that the code agrees with itself, so the bounds, the
// sample rate ceiling and the milliseconds are all literals here. `share` is
// the fraction of the residual the path in question carries: all of it for the
// audio capture, half for a MIDI one.
static long long trimSamplesByConvention (double trimMs, double sampleRate, double share)
{
    if (! std::isfinite (trimMs) || ! (sampleRate > 0.0))
        return 0;

    const double bounded = std::min (50.0, std::max (-50.0, trimMs));

    return std::lround (share * bounded * std::min (sampleRate, 768000.0) / 1000.0);
}

// ---------------------------------------------------------------------------
// What the MIDI offset is made of, each part of it pinned to a number written
// out here rather than computed from the thing being checked.

static void theMidiOffsetIsTheSumOfItsParts()
{
    // Every expected figure below is arithmetic on these two numbers done by
    // hand, so if the bounds move the figures are no longer the right ones and
    // this says so rather than quietly following them.
    check (RecordAlign::minTrimMs == -50.0 && RecordAlign::maxTrimMs == 50.0,
           "the trim's bounds moved, so the figures pinned in this test are no longer the right ones");

    RecordAlign::Figures f;
    f.sampleRate = 48000.0;

    // ---- the output side of the claim, on its own ----

    // The input side is set to something that would change the answer if it
    // leaked in, because a MIDI note never passes through the input converter.
    f.inputLatency  = 240;
    f.outputLatency = 264;
    f.bufferSize    = 128;
    checkEqual (RecordAlign::claimedOutputSamples (f), 264,
                "a reported output latency is not passed through");

    // A driver that said nothing is not a driver with no latency, and one
    // direction floors at one buffer where the round trip floors at two.
    f.outputLatency = 0;
    checkEqual (RecordAlign::claimedOutputSamples (f), 128,
                "a silent driver's output side did not floor at exactly one buffer");

    f.outputLatency = 100;
    checkEqual (RecordAlign::claimedOutputSamples (f), 128,
                "a claim below one buffer was believed");

    f.outputLatency = 264;
    f.bufferSize    = 0;
    checkEqual (RecordAlign::claimedOutputSamples (f), 264,
                "with no buffer size there is nothing to floor against");

    f.outputLatency = 0;
    checkEqual (RecordAlign::claimedOutputSamples (f), 0,
                "nothing claimed and no buffer to guess from should be nothing");

    // A negative claim is a broken driver, and it must never turn into a
    // negative offset: that would push takes later for no reason at all.
    f.outputLatency = -2000;
    f.bufferSize    = 128;
    checkEqual (RecordAlign::claimedOutputSamples (f), 128,
                "a negative claim survived the floor");
    f.bufferSize    = 0;
    checkEqual (RecordAlign::claimedOutputSamples (f), 0,
                "a negative claim with no buffer produced a negative output latency");

    // And an absurd one is bounded rather than carried into an addition.
    f.outputLatency = 1 << 30;
    checkEqual (RecordAlign::claimedOutputSamples (f), 4194304,
                "an absurd claim was not bounded");

    // ---- the whole MIDI offset ----

    f = RecordAlign::Figures{};
    f.sampleRate      = 48000.0;
    f.inputLatency    = 240;        // in the audio offset, never in this one
    f.outputLatency   = 264;
    f.bufferSize      = 128;
    f.engineLatency   = 1024;
    f.midiInputLatency = 0;

    // 264 out + 1024 engine + 128 for the collector's block + 0 for the MIDI
    // path itself. The input latency is deliberately not in it.
    checkEqual (RecordAlign::midiCaptureOffsetSamples (f), 1416,
                "the MIDI offset is not the output plus the engine plus one block");

    f.midiInputLatency = 77;
    checkEqual (RecordAlign::midiCaptureOffsetSamples (f), 1493,
                "the MIDI path's own delay is not counted");
    f.midiInputLatency = 0;

    f.engineLatency = 0;
    checkEqual (RecordAlign::midiCaptureOffsetSamples (f), 392,
                "the studio's own compensation is not counted");
    f.engineLatency = 1024;

    // The collector's one block, pinned on its own: a device claiming nothing
    // at a buffer of 512 owes one buffer for the output floor and one more for
    // the block the collector is anchored to, which is 1024 and not 512.
    RecordAlign::Figures g;
    g.sampleRate    = 48000.0;
    g.outputLatency = 0;
    g.bufferSize    = 512;
    checkEqual (RecordAlign::midiCaptureOffsetSamples (g), 1024,
                "the collector's one block term is not one block");

    g.bufferSize    = 0;
    g.outputLatency = 264;
    checkEqual (RecordAlign::midiCaptureOffsetSamples (g), 264,
                "with no buffer size there is no block to add");

    // ---- the trim, which is halved, and that is a convention ----

    f.trimMs = 10.0;                // 480 samples at 48 kHz, of which half
    checkEqual (RecordAlign::midiCaptureOffsetSamples (f), 1416 + 240,
                "the trim reaching a MIDI capture is not half of it");
    checkEqual (RecordAlign::captureOffsetSamples (f), 1528 + 480,
                "the trim reaching an audio capture is not all of it");

    f.trimMs = -10.0;
    checkEqual (RecordAlign::midiCaptureOffsetSamples (f), 1416 - 240,
                "a negative trim does not push a MIDI take later by half of it");

    f.sampleRate = 96000.0;
    checkEqual (RecordAlign::midiCaptureOffsetSamples (f), 1416 - 480,
                "the trim is not being read in milliseconds");
    f.sampleRate = 48000.0;

    // Halved first and rounded after, so a trim worth under two samples still
    // moves a MIDI take by one rather than by nothing.
    f.trimMs = 0.04;                // 1.92 samples, half of which is 0.96
    checkEqual (RecordAlign::midiCaptureOffsetSamples (f), 1417,
                "a sub sample half trim was truncated away instead of rounded");
    checkEqual (RecordAlign::captureOffsetSamples (f), 1528 + 2,
                "the audio trim rounds differently than it did");

    // Out of range, not a number, and a sample rate that is neither. All of
    // these reach here from a settings file and none may reach the conversion
    // to int as they are.
    f.trimMs = 1.0e9;
    checkEqual (RecordAlign::midiCaptureOffsetSamples (f), 1416 + 1200,
                "a trim beyond the range was not clamped to it");

    f.trimMs = std::nan ("");
    checkEqual (RecordAlign::midiCaptureOffsetSamples (f), 1416,
                "a trim that is not a number was not treated as no trim");

    f.trimMs = std::numeric_limits<double>::infinity();
    checkEqual (RecordAlign::midiCaptureOffsetSamples (f), 1416,
                "an infinite trim was not treated as no trim");

    f.trimMs     = 10.0;
    f.sampleRate = 0.0;
    checkEqual (RecordAlign::midiCaptureOffsetSamples (f), 1416,
                "a trim was converted with no sample rate to convert it by");

    f.sampleRate = -48000.0;
    checkEqual (RecordAlign::midiCaptureOffsetSamples (f), 1416,
                "a negative sample rate turned the trim around");

    f.sampleRate = std::nan ("");
    checkEqual (RecordAlign::midiCaptureOffsetSamples (f), 1416,
                "a sample rate that is not a number was used as one");

    f.sampleRate = 48000.0;
    f.trimMs     = 0.0;

    // A negative figure out of the mixer, or out of a settings file, is not a
    // reason to push a take later.
    f.engineLatency = -2000;
    checkEqual (RecordAlign::midiCaptureOffsetSamples (f), 392,
                "a negative compensation figure was allowed to push a MIDI take later");
    f.engineLatency = 1024;

    f.midiInputLatency = -500;
    checkEqual (RecordAlign::midiCaptureOffsetSamples (f), 1416,
                "a negative MIDI input latency was allowed to pull a take earlier");
    f.midiInputLatency = 0;

    // Everything absurd at once. Each part is bounded on the way in and the
    // sum is bounded on the way out, so nothing overflows an int and nothing
    // absurd comes back.
    RecordAlign::Figures wild;
    wild.sampleRate       = 48000.0;
    wild.outputLatency    = 2000000000;
    wild.inputLatency     = 2000000000;
    wild.bufferSize       = 2000000000;
    wild.engineLatency    = 2000000000;
    wild.midiInputLatency = 2000000000;
    checkEqual (RecordAlign::midiCaptureOffsetSamples (wild), 4194304,
                "an absurd set of figures was not bounded");

    wild.outputLatency    = -2000000000;
    wild.inputLatency     = -2000000000;
    wild.engineLatency    = -2000000000;
    wild.midiInputLatency = -2000000000;
    wild.bufferSize       = 256;
    wild.trimMs           = -50.0;
    checkEqual (RecordAlign::midiCaptureOffsetSamples (wild), 256 + 256 - 1200,
                "an absurd set of negative figures was not bounded");
}

// ---------------------------------------------------------------------------
// The two offsets have to move together. The usual way a pair like this goes
// wrong is somebody editing one of them, and nothing about the result looks
// broken: the audio takes and the MIDI takes simply stop agreeing with each
// other by a few milliseconds.
//
// The header states the relationship as
//
//     midi - audio == bufferSize + midiInputLatency - inputLatency - trim/2
//
// where neither figure's floor binds, and says that where one does the
// relation is false rather than approximate. Both halves of that are asserted
// here: the stated form over generated configurations that keep clear of the
// floors, and, where a floor binds, the amount it is false by pinned to its
// own literal rather than the check being loosened until it passes. The
// universal form the header quotes is the last thing below.

static void theTwoOffsetsStayInStep()
{
    std::mt19937 rng (31337u);

    const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
    const int    blocks[] = { 32, 64, 128, 256, 480, 512, 1024, 2048 };

    std::uniform_int_distribution<int>     rateDist  (0, 4);
    std::uniform_int_distribution<int>     blockDist (0, 7);
    std::uniform_int_distribution<int>     overDist  (0, 8192);
    std::uniform_int_distribution<int>     engDist   (0, 100000);
    std::uniform_int_distribution<int>     midiDist  (0, 8192);
    std::uniform_real_distribution<double> trimDist  (-50.0, 50.0);

    for (int trial = 0; trial < 20000; ++trial)
    {
        RecordAlign::Figures f;
        f.sampleRate = rates[(size_t) rateDist (rng)];
        f.bufferSize = blocks[blockDist (rng)];

        // Both claims at or above their own floor, which is the ordinary case
        // the relation is stated for. A driver that goes quiet is the other
        // case and is pinned by hand below.
        f.inputLatency     = f.bufferSize + 1 + overDist (rng);
        f.outputLatency    = f.bufferSize + overDist (rng);
        f.engineLatency    = engDist (rng);
        f.midiInputLatency = midiDist (rng);
        f.trimMs           = trimDist (rng);

        const long long midi  = RecordAlign::midiCaptureOffsetSamples (f);
        const long long audio = RecordAlign::captureOffsetSamples (f);

        // The share of the trim the audio path carries and the MIDI path does
        // not, from the convention written out above rather than from either
        // function.
        const long long whole  = trimSamplesByConvention (f.trimMs, f.sampleRate, 1.0);
        const long long half   = trimSamplesByConvention (f.trimMs, f.sampleRate, 0.5);
        const long long shared = whole - half;

        checkEqual (midi - audio,
                    (long long) f.bufferSize + f.midiInputLatency - f.inputLatency - shared,
                    "the MIDI offset and the audio offset no longer differ by what the header says");

        // And the same statement the lazy way the header writes it, where the
        // trim is simply halved. The two roundings are independent, so this
        // one is only ever good to a sample: said here rather than left to be
        // discovered by somebody who trusted the comment.
        const double stated = (double) f.bufferSize + f.midiInputLatency - f.inputLatency
                              - 0.5 * (double) whole;
        check (std::abs ((double) (midi - audio) - stated) <= 1.0,
               "the header's half-a-trim form is out by more than the rounding can explain");

        if (failures > 0)
            return;
    }

    // ---- where the floors make that relation false ----

    // A driver that reported nothing at all. The audio side floors its round
    // trip at two buffers and the MIDI side floors its output at one, so the
    // audio figure already contains an input buffer that the relation above
    // reads as an input latency of zero. The difference is therefore short by
    // exactly one buffer, and that is a property of the floors rather than a
    // disagreement between the two.
    RecordAlign::Figures quiet;
    quiet.sampleRate = 48000.0;
    quiet.bufferSize = 256;
    checkEqual (RecordAlign::captureOffsetSamples (quiet), 512,
                "a silent driver's audio round trip did not floor at two buffers");
    checkEqual (RecordAlign::midiCaptureOffsetSamples (quiet), 512,
                "a silent driver's MIDI offset is not one buffer of output and one of block");
    checkEqual (RecordAlign::midiCaptureOffsetSamples (quiet) - RecordAlign::captureOffsetSamples (quiet),
                0,
                "the two offsets do not agree where both floors bind");
    checkEqual ((long long) quiet.bufferSize + quiet.midiInputLatency - quiet.inputLatency, 256,
                "the simple relation no longer predicts what this case is here to contradict");

    // A driver that answered for the input and went quiet about the output.
    // Only the MIDI side floors, so the difference is short by what the output
    // floor had to put in.
    RecordAlign::Figures lopsided;
    lopsided.sampleRate    = 48000.0;
    lopsided.bufferSize    = 256;
    lopsided.inputLatency  = 4000;
    lopsided.outputLatency = 10;
    checkEqual (RecordAlign::captureOffsetSamples (lopsided), 4010,
                "a round trip above the floor was dragged to it");
    checkEqual (RecordAlign::midiCaptureOffsetSamples (lopsided), 512,
                "an output claim below one buffer was believed");
    checkEqual (RecordAlign::midiCaptureOffsetSamples (lopsided)
                  - RecordAlign::captureOffsetSamples (lopsided),
                512 - 4010,
                "the two offsets do not agree where only the output floor binds");
    checkEqual ((long long) 512 - 4010 - (lopsided.bufferSize - lopsided.inputLatency),
                lopsided.bufferSize - lopsided.outputLatency,
                "the output floor's contribution is not the amount the relation is out by");

    // ---- and the universal form, over anything a settings file can hold ----
    //
    // Whatever the floors do, the MIDI offset is the audio one with the input
    // side of the claim taken out and a block, the MIDI path and half the trim
    // put in. "The input side of the claim, as the floors leave it" is the
    // round trip less the output, which is the only part of this the two
    // functions genuinely share.

    std::uniform_int_distribution<int> anyDist (-20000, 20000);

    for (int trial = 0; trial < 20000; ++trial)
    {
        RecordAlign::Figures f;
        f.sampleRate       = rates[(size_t) rateDist (rng)];
        f.bufferSize       = anyDist (rng);
        f.inputLatency     = anyDist (rng);
        f.outputLatency    = anyDist (rng);
        f.engineLatency    = anyDist (rng);
        f.midiInputLatency = anyDist (rng);
        f.trimMs           = trimDist (rng) * 3.0;      // often beyond its range

        const long long block  = std::clamp (f.bufferSize, 0, 1 << 22);
        const long long midiIn = std::clamp (f.midiInputLatency, 0, 1 << 22);
        const long long input  = (long long) RecordAlign::claimedRoundTripSamples (f)
                                 - RecordAlign::claimedOutputSamples (f);
        const long long shared = trimSamplesByConvention (f.trimMs, f.sampleRate, 1.0)
                                 - trimSamplesByConvention (f.trimMs, f.sampleRate, 0.5);

        checkEqual (RecordAlign::midiCaptureOffsetSamples (f)
                      - RecordAlign::captureOffsetSamples (f),
                    block + midiIn - input - shared,
                    "the two offsets differ by something other than the terms that differ");

        if (failures > 0)
            return;
    }
}

// ---------------------------------------------------------------------------
// One note, from the player's hands to the beat the engine writes it down at.
//
// This is the part that has to be arrived at independently of the code under
// test, so nothing below reads any of RecordAlign's arithmetic: it walks the
// wall clock, the player's ears, the collector's anchoring and the block the
// engine stamps from, and only then asks placeMidiEvent where the note goes.

struct MidiRig
{
    double sampleRate   = 48000.0;
    double bpm          = 120.0;
    int    block        = 256;
    int    engine       = 0;    /**< the studio's own compensation */
    int    reportedOut  = 0;    /**< what the driver claims on the way out */
    int    reportedIn   = 0;    /**< and on the way in, which no MIDI note meets */

    /** What the two directions really are, less what was claimed. A loopback
        measurement finds the sum of these and cannot say which converter it
        was in, which is the whole reason the trim is split in half. */
    int    trueExtraOut = 0;
    int    trueExtraIn  = 0;

    /** The MIDI path's own delay, claimed and true: here the player measured
        it correctly, which is what the figure is for. */
    int    midiIn       = 0;

    double recordStart  = 0.0;

    /** The trim, where the misreport above is not what a loopback would find.
        A trim is measured against the claim rather than against the report
        (LatencyProbe::trimMsFor subtracts claimedRoundTripSamples), so where
        the floor has already supplied the figure the driver would not give,
        there is no residual left to correct and the trim is nothing. */
    double trimMsInstead = std::nan ("");

    double bps() const { return bpm / 60.0 / sampleRate; }

    RecordAlign::Figures figures() const
    {
        RecordAlign::Figures f;
        f.sampleRate       = sampleRate;
        f.bufferSize       = block;
        f.inputLatency     = reportedIn;
        f.outputLatency    = reportedOut;
        f.engineLatency    = engine;
        f.midiInputLatency = midiIn;
        f.trimMs           = std::isfinite (trimMsInstead)
                               ? trimMsInstead
                               : 1000.0 * (trueExtraOut + trueExtraIn) / sampleRate;
        return f;
    }
};

/** The beat the engine writes down for a note the player heard `playedSample`
    samples of content after recording began.

    Wall clock in samples, with zero at the drain of the block whose transport
    position is the record point, so the block drained at wall time w is
    rendering transport position recordStart + w beats-per-sample.
*/
static double rawBeatForNotePlayedAt (const MidiRig& rig, int playedSample, int& position)
{
    const double bps = rig.bps();

    // The player hears the block rendered engine + output samples before it
    // reaches them, so the content they hear at wall time w was rendered at
    // transport w - (engine + output). They play along with what they hear.
    const long long heard   = (long long) playedSample + rig.engine
                              + rig.reportedOut + rig.trueExtraOut;

    // Through the MIDI socket, the polling and the operating system's stack.
    const long long arrived = heard + rig.midiIn;

    // MidiMessageCollector anchors the END of the buffer it fills to the
    // instant the callback drained it, so an event is handed over in the first
    // block to drain after it arrived, at the position its arrival sits at
    // counting back from that drain.
    const long long drain = (arrived / rig.block + 1) * rig.block;
    position = (int) (rig.block - (drain - arrived));

    check (position >= 0 && position < rig.block,
           "the simulated collector handed back a position outside the block");

    // blockBeats[i] is the transport position at sample i of the block that
    // drained, and the engine stamps the event with that.
    return rig.recordStart + (double) drain * bps + (double) position * bps;
}

static void aMidiNotePlayedOnTheBeatLandsOnTheBeat()
{
    std::mt19937 rng (20261119u);

    const double rates[]  = { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
    const int    blocks[] = { 32, 64, 128, 256, 480, 512, 1024 };

    std::uniform_int_distribution<int>     rateDist  (0, 4);
    std::uniform_int_distribution<int>     blockDist (0, 6);
    std::uniform_int_distribution<int>     overDist  (0, 4096);
    std::uniform_int_distribution<int>     engDist   (0, 8192);
    std::uniform_int_distribution<int>     midiDist  (0, 1500);
    std::uniform_int_distribution<int>     missDist  (0, 500);
    std::uniform_int_distribution<int>     playDist  (1, 400000);
    std::uniform_real_distribution<double> bpmDist   (20.0, 400.0);
    std::uniform_real_distribution<double> beatDist  (0.0, 400.0);

    int exercised = 0;

    for (int trial = 0; trial < 4000; ++trial)
    {
        MidiRig rig;
        rig.sampleRate  = rates[(size_t) rateDist (rng)];
        rig.bpm         = bpmDist (rng);
        rig.block       = blocks[blockDist (rng)];
        rig.engine      = engDist (rng);
        rig.reportedOut = rig.block + overDist (rng);   // at or above its floor
        rig.reportedIn  = rig.block + overDist (rng);
        rig.midiIn      = midiDist (rng);
        rig.recordStart = beatDist (rng);

        // The driver misreports, and by the same amount in each direction, so
        // that splitting the residual in half is exactly right. The asymmetric
        // case is the next check and is deliberately kept apart from this one:
        // the split is a convention and not a derivation.
        const int miss = missDist (rng);
        rig.trueExtraOut = miss;
        rig.trueExtraIn  = miss;

        const double bps    = rig.bps();
        const int    offset = RecordAlign::midiCaptureOffsetSamples (rig.figures());
        const double offsetBeats = (double) offset * bps;

        for (int note = 0; note < 4; ++note)
        {
            const int    played = playDist (rng);
            const double onTheBeat = rig.recordStart + (double) played * bps;

            int position = 0;
            const double raw = rawBeatForNotePlayedAt (rig, played, position);

            const auto placed = RecordAlign::placeMidiEvent (raw, offsetBeats, rig.recordStart);

            check (placed.keep, "a note played after the record point was thrown away");
            checkClose (placed.beat, onTheBeat, 1.0e-9 + std::abs (onTheBeat) * 1.0e-12,
                        "a note played on the beat did not land on the beat");

            // And the measurement is measuring something: uncompensated, the
            // same note is late by the whole offset, which is the fault this
            // exists to fix. Without this the check above would pass just as
            // well against an offset of nothing.
            check (raw > onTheBeat + 0.5 * offsetBeats,
                   "the uncompensated beat is not late, so this check is measuring nothing");

            ++exercised;

            if (failures > 0)
                return;
        }
    }

    check (exercised > 15000, "the end to end check did not run");

    // ---- the asymmetric case, which is the convention's own limit ----
    //
    // A loopback cable measures a round trip and cannot say which converter
    // the error was in, so the residual is split evenly. Where it was not even
    // the note lands out by half the asymmetry, which is a bound on this
    // studio's accuracy and is written down rather than hidden.
    for (int trial = 0; trial < 2000; ++trial)
    {
        MidiRig rig;
        rig.sampleRate  = rates[(size_t) rateDist (rng)];
        rig.bpm         = bpmDist (rng);
        rig.block       = blocks[blockDist (rng)];
        rig.engine      = engDist (rng);
        rig.reportedOut = rig.block + overDist (rng);
        rig.reportedIn  = rig.block + overDist (rng);
        rig.midiIn      = midiDist (rng);
        rig.recordStart = beatDist (rng);
        rig.trueExtraOut = missDist (rng);
        rig.trueExtraIn  = missDist (rng);

        const double bps    = rig.bps();
        const int    offset = RecordAlign::midiCaptureOffsetSamples (rig.figures());
        const int    played = playDist (rng);

        int position = 0;
        const double raw   = rawBeatForNotePlayedAt (rig, played, position);
        const auto placed  = RecordAlign::placeMidiEvent (raw, (double) offset * bps,
                                                          rig.recordStart);

        const double onTheBeat = rig.recordStart + (double) played * bps;
        const double outBy     = std::abs (placed.beat - onTheBeat) / bps;
        const double asymmetry = 0.5 * std::abs (rig.trueExtraOut - rig.trueExtraIn);

        check (placed.keep, "a note played after the record point was thrown away");
        check (outBy <= asymmetry + 1.0,
               "an evenly split trim is out by more than half the asymmetry it could not see");

        if (failures > 0)
            return;
    }

    // ---- a driver that reported nothing, where the floor is the whole answer ----
    //
    // Its real output latency is one buffer, which is the floor's own
    // assumption, so the note still lands exactly on the beat. This is the
    // case the floor exists for and the one where believing a reported zero
    // would put every note a buffer late.
    {
        MidiRig rig;
        rig.sampleRate  = 48000.0;
        rig.bpm         = 128.0;
        rig.block       = 512;
        rig.engine      = 1500;
        rig.reportedOut = 0;            // the driver did not say
        rig.reportedIn  = 0;
        rig.trueExtraOut = 512;         // but it is one buffer, which the floor guesses
        rig.trueExtraIn  = 512;
        rig.recordStart = 32.0;

        // A loopback would find nothing to correct here: the floor's two
        // buffers are exactly what the round trip really is.
        rig.trimMsInstead = 0.0;

        const double bps    = rig.bps();
        const int    offset = RecordAlign::midiCaptureOffsetSamples (rig.figures());

        for (const int played : { 1, 999, 48000, 123457 })
        {
            int position = 0;
            const double raw = rawBeatForNotePlayedAt (rig, played, position);
            const auto placed = RecordAlign::placeMidiEvent (raw, (double) offset * bps,
                                                             rig.recordStart);
            const double onTheBeat = rig.recordStart + (double) played * bps;

            check (placed.keep, "a note from a silent driver was thrown away");
            checkClose (placed.beat, onTheBeat, 1.0e-9,
                        "a silent driver's floor did not land the note on the beat");
        }
    }

}

// ---------------------------------------------------------------------------
// What belongs before the record point is held there rather than dropped.
//
// The distinction matters more than it looks, and the header has the argument:
// a player pushing the downbeat is most players most of the time and the
// correction is of the same order as the push, so under a drop the first note
// of a take vanishes more often than not, and MainComponent::finishRecording
// would drop it a second time anyway. A MIDI clip has no pre-roll to keep it
// in, the way an audio take does.

static void anEventBeforeTheRecordPointIsHeldThere()
{
    const double recordStart = 8.0;
    const double offset      = 2.0;

    const auto onIt = RecordAlign::placeMidiEvent (10.0, offset, recordStart);
    check (onIt.keep, "a note landing exactly on the record point was thrown away");
    checkClose (onIt.beat, 8.0, 0.0, "a note landing on the record point did not land exactly there");

    // One unit in the last place earlier, which is the smallest "before" a
    // double can express. Both subtractions here are exact, so this really is
    // one ulp below the record point rather than a rounding that went nowhere,
    // and the check below proves it rather than assuming it.
    const double justBefore = std::nextafter (10.0, 0.0);
    const double corrected  = justBefore - offset;
    check (corrected < recordStart && recordStart - corrected < 1.0e-14,
           "the boundary case is not actually one ulp before the record point");

    const auto before = RecordAlign::placeMidiEvent (justBefore, offset, recordStart);
    check (before.keep, "a note from a hair before the record point was thrown away");
    checkClose (before.beat, recordStart, 0.0,
                "a note from a hair before the record point was not held at it");

    // Well before it, which is the pushed downbeat the clamp exists for: ten
    // to sixty milliseconds early is a whole beat at these distances, and it
    // still belongs in the take rather than nowhere.
    const auto wellBefore = RecordAlign::placeMidiEvent (5.0, offset, recordStart);
    check (wellBefore.keep, "a note from well before the record point was thrown away");
    checkClose (wellBefore.beat, recordStart, 0.0,
                "a note from well before the record point was not held at it");

    // Nothing may ever come back earlier than the record point, whatever it
    // was handed. That is the one thing finishRecording relies on: it rebases
    // by the record start and keeps only what is not negative.
    const auto wayBefore = RecordAlign::placeMidiEvent (-1.0e6, offset, recordStart);
    check (wayBefore.keep && wayBefore.beat >= recordStart,
           "a note from a long way before the record point came back before it");

    const auto after = RecordAlign::placeMidiEvent (std::nextafter (10.0, 20.0), offset,
                                                    recordStart);
    check (after.keep, "a note one ulp after the record point was moved");
    check (after.beat > recordStart, "a note after the record point was clamped to it");

    // A record point that is not a number is a studio that has lost track of
    // when it started. There is nothing to clamp against, so the correction
    // stands on its own rather than everything being flattened onto a number
    // that is not one.
    for (const double lost : { std::nan (""), std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity() })
    {
        const auto kept = RecordAlign::placeMidiEvent (10.0, offset, lost);
        check (kept.keep, "a record point that is not a number threw the take away");
        checkClose (kept.beat, 8.0, 0.0,
                    "a note was moved by a record point that is not a number");
    }

    // A negative offset, which only a trim larger than the whole round trip
    // can ask for, moves the note later and cannot be clamped.
    const auto later = RecordAlign::placeMidiEvent (10.0, -2.0, recordStart);
    check (later.keep && later.beat == 12.0, "a negative offset did not push the note later");
}

// ---------------------------------------------------------------------------
// A NOTE SURVIVES AS A NOTE
//
// Single event checks cannot see this and the file's first version did not
// have it, which is how it came to bless a wrap that corrupted note pairs: a
// note on landing a hair before the loop start was sent to the top of the
// cycle while its note off, a beat later, was not. Each event was placed
// perfectly well on its own. What came out was a blip at the far end of the
// pattern, because MainComponent::collectRecordedMidi pairs the two and can
// only clamp a reversed pair to a sixty-fourth.
//
// So a pair is taken through together here, and what is asserted is the thing
// a musician would say: the note that comes back is the part of the note that
// is in the take. Never reversed, never longer than it was played, and shorter
// only by whatever of it was played before the take began.

/** What MainComponent::collectRecordedMidi makes of a pair, which is where the
    symptom of a broken pair actually shows up (MainComponent.cpp:792-801). */
static double pairedLength (double onBeat, double offBeat)
{
    return std::max (1.0 / 64.0, offBeat - onBeat);
}

static void aNoteSurvivesAsANote()
{
    std::mt19937 rng (808u);

    std::uniform_real_distribution<double> startDist  (0.0, 400.0);
    std::uniform_real_distribution<double> offsetDist (0.0, 2.0);
    std::uniform_real_distribution<double> unitDist   (0.0, 1.0);
    std::uniform_real_distribution<double> lenDist    (1.0 / 32.0, 4.0);

    int clamped = 0, whollyBefore = 0, untouched = 0;

    for (int trial = 0; trial < 40000; ++trial)
    {
        const double recordStart = startDist (rng);
        const double offsetBeats = offsetDist (rng);
        const double length      = lenDist (rng);

        // The transport is at or after the record point when anything is
        // written down, so a raw beat never precedes it. Most of the range is
        // inside the first few offsets of the take, which is where the clamp
        // bites and where the broken pairs lived.
        const double reach  = (trial % 4 == 0) ? 40.0 : 3.0 * offsetBeats + 0.05;
        const double onRaw  = recordStart + unitDist (rng) * reach;
        const double offRaw = onRaw + length;

        const auto on  = RecordAlign::placeMidiEvent (onRaw,  offsetBeats, recordStart);
        const auto off = RecordAlign::placeMidiEvent (offRaw, offsetBeats, recordStart);

        const std::string label = "pair trial " + std::to_string (trial);

        check (on.keep && off.keep, label + ": half of a note was thrown away");
        if (! (on.keep && off.keep))
            return;

        // In order. This is the one the wrap broke, and it is first because
        // everything else is meaningless once a note runs backwards.
        check (off.beat >= on.beat, label + ": a note came back ending before it started");

        // Never longer than it was played, and never reaching before the take.
        check (on.beat >= recordStart && off.beat >= recordStart,
               label + ": a note reached back before the record point");

        const double heldFor = off.beat - on.beat;
        const double tol     = 1.0e-9 + std::abs (recordStart) * 1.0e-12;

        check (heldFor <= length + tol, label + ": a note came back longer than it was played");

        // And shorter only by the part of it that was played before the take
        // began, which is the part that is not in the take. Anything else is
        // either lost performance or invented length.
        const double playedBefore = std::clamp (recordStart - (onRaw - offsetBeats), 0.0, length);
        checkClose (heldFor, length - playedBefore, tol,
                    label + ": a note lost something other than the part played before the take");

        // The symptom, modelled where it actually appears. A sixty-fourth is
        // what collectRecordedMidi falls back to when a pair makes no sense,
        // so reaching it for a note that still has something left of it is
        // the fault this check exists for.
        const double paired = pairedLength (on.beat, off.beat);

        if (heldFor >= 1.0 / 64.0)
            checkClose (paired, heldFor, tol,
                        label + ": a note that survived still came back as the sixty-fourth clamp");

        if (playedBefore >= length)
        {
            ++whollyBefore;
            checkClose (heldFor, 0.0, tol,
                        label + ": a note played entirely before the take kept a length");
        }
        else if (playedBefore > 0.0)
            ++clamped;
        else
            ++untouched;

        if (failures > 0)
            return;
    }

    // The counts are asserted so this cannot quietly stop exercising the case
    // it exists for. A run that never clamps anything is a run that would have
    // passed against the wrap as well.
    std::printf ("  pairs  %d untouched, %d clamped at the record point, %d played entirely before it\n",
                 untouched, clamped, whollyBefore);
    check (clamped > 2000 && untouched > 2000 && whollyBefore > 200,
           "the pair check stopped reaching the cases it is for");

    // ---- the regression itself, with the numbers it was measured at ----
    //
    // A player pushing the downbeat of a four bar pattern by two and a half
    // milliseconds at 120 bpm. Under the wrap this came back as `on 31.995
    // length 0.01562`, a sixty-fourth at the far end of the pattern. It is
    // a whole note at the top of it.
    {
        const double bps         = 120.0 / 60.0 / 48000.0;
        const double recordStart = 16.0;
        const double offsetBeats = 1456.0 * bps;          // 300 out, 900 engine, 256 block
        const double push        = 0.0025 * 2.0;          // 2.5 ms at two beats a second

        const double onRaw  = recordStart - push + offsetBeats;
        const double offRaw = onRaw + 0.5;

        const auto on  = RecordAlign::placeMidiEvent (onRaw,  offsetBeats, recordStart);
        const auto off = RecordAlign::placeMidiEvent (offRaw, offsetBeats, recordStart);

        check (on.keep && off.keep, "the pushed downbeat was thrown away");
        checkClose (on.beat, 16.0, 1.0e-12, "the pushed downbeat did not land on the downbeat");
        checkClose (pairedLength (on.beat, off.beat), 0.5 - push, 1.0e-9,
                    "the pushed downbeat did not come back as the note that was played");
        check (pairedLength (on.beat, off.beat) > 1.0 / 64.0,
               "the pushed downbeat came back as a blip");
        check (on.beat < 17.0, "the pushed downbeat was sent somewhere else in the pattern");
    }

    // A note shorter than the offset, played at the very top of a take. Both
    // ends belong before the record point, so nothing of it is in the take and
    // a zero length is the honest answer rather than a symptom: the sixty
    // fourth that collectRecordedMidi then applies is the only thing it can do
    // with a note that was over before recording began.
    {
        const auto on  = RecordAlign::placeMidiEvent (8.01, 1.0, 8.0);
        const auto off = RecordAlign::placeMidiEvent (8.02, 1.0, 8.0);
        check (on.keep && off.keep, "a note from before the take was thrown away");
        checkClose (on.beat, 8.0, 0.0, "a note from before the take did not land at its start");
        checkClose (off.beat, 8.0, 0.0, "a note from before the take did not land at its start");
    }
}

// ---------------------------------------------------------------------------
// Everything that is not a number, in every argument. A settings file, a
// corrupt project or a tempo of nothing can produce any of these, and the
// suite runs with the sanitizers on and halts on the first undefined
// behaviour, so what this is really asserting is that none of it is.
//
// The one thing that must never happen is a beat that is kept and is not a
// number: that is a note written into the arrangement at a position nothing
// can draw, sort or play.

static void hostileFiguresAndPlacements()
{
    const double nan  = std::nan ("");
    const double inf  = std::numeric_limits<double>::infinity();

    const double pool[] = { nan, inf, -inf, 0.0, -0.0, 1.0, -1.0, 4.0, 16.0,
                            1.0e308, -1.0e308, 5.0e-324, 9007199254740992.0,
                            -9007199254740992.0, 1.0e-300, 0.1, -1.0e6 };
    const int    n = (int) (sizeof (pool) / sizeof (pool[0]));

    int kept = 0, dropped = 0;

    for (int a = 0; a < n; ++a)
      for (int b = 0; b < n; ++b)
        for (int c = 0; c < n; ++c)
        {
            const auto placed = RecordAlign::placeMidiEvent (pool[a], pool[b], pool[c]);

            if (! placed.keep)
            {
                // The only reason to refuse is an input that is not a number.
                // Anything a transport and a tempo can really produce has to
                // come back placed somewhere.
                ++dropped;

                if (std::isfinite (pool[a] - pool[b]))
                {
                    fail ("a note with a perfectly good position was thrown away");
                    return;
                }

                continue;
            }

            ++kept;

            if (! std::isfinite (placed.beat))
            {
                fail ("a note was kept at a beat that is not a number");
                return;
            }

            // Nothing may ever come back before the record point, because
            // MainComponent::finishRecording rebases by it and keeps only what
            // is not negative: anything earlier is a note silently deleted.
            if (std::isfinite (pool[c]) && ! (placed.beat >= pool[c]))
            {
                fail ("a note came back before the record point");
                return;
            }
        }

    check (kept > 0 && dropped > 0,
           "the hostile sweep did not reach both answers, so it asserted less than it looks");

    // The same for the figures. Nothing here may overflow an int on the way to
    // an answer, and nothing absurd may come back out of one.
    std::mt19937 rng (6502u);
    std::uniform_int_distribution<int>     anyInt (-2000000000, 2000000000);
    std::uniform_int_distribution<int>     pick   (0, 7);
    const double rates[] = { 0.0, -48000.0, 48000.0, 1.0e-9, 1.0e12, nan, inf, -inf };
    const double trims[] = { 0.0, 50.0, -50.0, 1.0e300, -1.0e300, nan, inf, -inf };

    for (int trial = 0; trial < 20000; ++trial)
    {
        RecordAlign::Figures f;
        f.inputLatency     = anyInt (rng);
        f.outputLatency    = anyInt (rng);
        f.bufferSize       = anyInt (rng);
        f.engineLatency    = anyInt (rng);
        f.midiInputLatency = anyInt (rng);
        f.sampleRate       = rates[pick (rng)];
        f.trimMs           = trims[pick (rng)];

        const int midi = RecordAlign::midiCaptureOffsetSamples (f);
        const int out  = RecordAlign::claimedOutputSamples (f);

        check (midi >= -(1 << 22) && midi <= (1 << 22),
               "a MIDI offset escaped the bound that keeps it out of an overflow");
        check (out >= 0 && out <= (1 << 22),
               "an output latency came back negative or unbounded");

        if (failures > 0)
            return;
    }
}

int main()
{
    std::printf ("RecordAlign\n");

    alignmentHoldsEverywhere();
    aNegativeOffsetMovesTheWholeTakeLater();
    offsetIsTheSumOfItsParts();
    aNotePlayedOnTheBeatLandsOnTheBeat();
    theHeadOfACaptureIsKeptRatherThanDropped();
    degenerateCaptures();

    theMidiOffsetIsTheSumOfItsParts();
    theTwoOffsetsStayInStep();
    aMidiNotePlayedOnTheBeatLandsOnTheBeat();
    anEventBeforeTheRecordPointIsHeldThere();
    aNoteSurvivesAsANote();
    hostileFiguresAndPlacements();

    if (failures == 0)
        std::printf ("  ok\n");
    else
        std::printf ("  %d failure(s)\n", failures);

    return failures == 0 ? 0 : 1;
}
