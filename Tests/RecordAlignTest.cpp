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

#include <cstdio>
#include <cstdlib>
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

int main()
{
    std::printf ("RecordAlign\n");

    alignmentHoldsEverywhere();
    aNegativeOffsetMovesTheWholeTakeLater();
    offsetIsTheSumOfItsParts();
    aNotePlayedOnTheBeatLandsOnTheBeat();
    theHeadOfACaptureIsKeptRatherThanDropped();
    degenerateCaptures();

    if (failures == 0)
        std::printf ("  ok\n");
    else
        std::printf ("  %d failure(s)\n", failures);

    return failures == 0 ? 0 : 1;
}
