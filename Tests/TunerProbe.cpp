// Not run by run.sh: a *Probe.cpp is a measurement that justified a design
// decision, kept so the decision can be rechecked rather than argued about.
//
//   g++ -std=c++20 -O2 -pthread -I Source -o probe Tests/TunerProbe.cpp && ./probe
//
// It needs a release build and about a minute, which is why it is not in the
// suite: one reading at 192 kHz is a couple of hundred million operations and
// the tear measurement needs two threads to actually fight.
//
// THE DECISIONS THIS EXISTS TO RECHECK
//
// Three sets of numbers in Tuner.h are stated as measurements and were, until
// this file, measured nowhere that anybody could rerun:
//
//   1. The four row reading cost table above `Pacing`, and the claim that the
//      cost goes as the square of the sample rate. That table is the whole
//      argument for the duty cycle existing, and `Tests/TunerTest.cpp` hard
//      codes all four of its figures.
//
//   2. The reading rates and mean hops the duty cycle produces from it, which
//      are what `sweepVibrato` in the suite centres its vibrato on. If the
//      cost figures are wrong then the sweep is sweeping the wrong band and
//      the gate was tuned against the wrong fault.
//
//   3. The tear rates above `InputRing`, which are what `studioRingCapacity`
//      cites to justify taking four times the snapshot rather than the
//      correctness requirement of one snapshot plus two blocks.
//
// `Research/BACKLOG.md` named all three as not reproducible from anything
// checked in, and said why it mattered: the cost sets the duty cycle, the duty
// cycle sets the reading rate, and the reading rate decides the whole vibrato
// result.
//
// WHAT IS MACHINE DEPENDENT HERE AND WHAT IS NOT
//
// The milliseconds belong to whatever processor runs this, so a figure off the
// owner's machine and a figure off a build agent are not the same measurement
// and neither is wrong. What does not depend on the machine is the SHAPE: the
// window length at each rate is exact arithmetic, the cost ratio between one
// rate and twice it is a property of the algorithm, and everything the duty
// cycle derives is arithmetic over whatever the cost turns out to be. So the
// shape is what is held to a bound, and the absolute table is printed with the
// machine it came from named beside it.
//
// The tear rates are machine dependent in a second way worth stating: they are
// a race, so how often it is lost depends on how the two threads happen to be
// scheduled. A run that sees no tears at a capacity the requirement says is
// too small has not disproved the requirement, it has lost the race fewer
// times. Only a run that DOES see tears there proves anything, which is why
// the bound below is one sided.

#include "Tuner.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace
{

constexpr double kPi = 3.14159265358979323846;

using Clock = std::chrono::steady_clock;

double secondsSince (Clock::time_point from)
{
    return std::chrono::duration<double> (Clock::now() - from).count();
}

//==============================================================================
/** A note with harmonics on it, which is what a reading actually costs on.

    Not a pure sine, deliberately. The difference function walks every lag
    whatever the signal is, so the cost is nearly independent of the content,
    but the branch in step 4 stops at the first dip below the threshold and a
    pure sine dips sooner than a plucked string does. Six harmonics at 1/h is
    the same signal the suite's vibrato sweep uses, so the two measurements are
    of the same work.
*/
std::vector<float> note (double sampleRate, double f0, int n)
{
    std::vector<float> v ((size_t) n, 0.0f);
    double phase = 0.0;

    for (int i = 0; i < n; ++i)
    {
        double sample = 0.0;

        for (int h = 1; h <= 6; ++h)
            sample += 0.3 / (double) h * std::sin ((double) h * phase);

        v[(size_t) i] = (float) sample;
        phase += 2.0 * kPi * f0 / sampleRate;
    }

    return v;
}

struct CostRow
{
    double sampleRate  = 0.0;
    int    windowSamples = 0;
    double windowSeconds = 0.0;
    double readingSeconds = 0.0;
};

/** What one reading costs at `sampleRate`, with the SHIPPED spec.

    The shipped spec and not the suite's `fastSpec`, which raises the search
    floor to 160 Hz to run in a few seconds. The floor is what sets the longest
    lag, so a measurement taken at 160 Hz would understate the cost by the
    ratio of the two floors and would be measuring a tuner nobody ships.

    The best of several runs rather than the mean. The quantity wanted is what
    the work costs, and anything above the fastest run is this probe being
    descheduled, which is noise from somewhere else on the machine rather than
    part of the reading.
*/
CostRow measureReading (double sampleRate, int repeats)
{
    Tuner::Spec spec;
    spec.sampleRate = sampleRate;

    Tuner::Detector detector;
    detector.configure (spec);

    CostRow row;
    row.sampleRate    = sampleRate;
    row.windowSamples = detector.requiredSampleCount();
    row.windowSeconds = (double) row.windowSamples / sampleRate;

    const auto signal = note (sampleRate, 220.0, row.windowSamples);

    // One reading thrown away before the timing starts. The first touch of the
    // detector's own buffers is a page fault per page of them, which at 192 kHz
    // is a megabyte of first touches and is paid once per device rather than
    // once per reading.
    detector.estimate (signal.data(), (int) signal.size());

    double best = 1.0e9;

    for (int r = 0; r < repeats; ++r)
    {
        const auto started = Clock::now();
        const auto estimate = detector.estimate (signal.data(), (int) signal.size());
        const double took = secondsSince (started);

        // Read the result so no part of the call can be optimised away.
        if (estimate.hz < 0.0)
            std::printf ("  (no reading at %.0f Hz, which this signal should always give)\n", sampleRate);

        best = std::min (best, took);
    }

    row.readingSeconds = best;
    return row;
}

//==============================================================================
/** What the duty cycle does with a reading that costs `readingSeconds`.

    All of this is arithmetic over `Pacing` rather than a measurement, and it is
    here rather than in the suite only because it is reported against measured
    costs. The suite checks the same arithmetic against the table.
*/
struct Paced
{
    double meanWait = 0.0;
    double meanHop  = 0.0;     // the reading and the wait after it
    double readingsPerSecond = 0.0;
    double coreShare = 0.0;
    double base = 0.0;
};

Paced pace (double readingSeconds, const Tuner::Pacing::Settings& s, std::uint64_t seed = 1u)
{
    Tuner::Pacing p;
    p.configure (s, seed);

    const int draws = 200000;
    double sum = 0.0;

    for (int i = 0; i < draws; ++i)
        sum += p.nextWait (readingSeconds);

    Paced out;
    out.base     = std::max (s.nominalSeconds - readingSeconds, readingSeconds * s.dutyMultiple);
    out.meanWait = sum / (double) draws;
    out.meanHop  = readingSeconds + out.meanWait;
    out.readingsPerSecond = 1.0 / out.meanHop;
    out.coreShare = readingSeconds / out.meanHop;

    return out;
}

//==============================================================================
/** How often an ACCEPTED snapshot was actually torn, at a given capacity.

    The suite checks that a tear is detected and that normal arrival is not
    refused, which brackets the bound from either side. Neither can say how
    often a tear gets through at a capacity that is too small, because that
    needs the snapshot's contents checked for continuity rather than its return
    value checked for truth.

    So the writer writes a ramp instead of audio: sample k carries k modulo
    2^20, which is exact in a float and large enough that a tear cannot
    masquerade as the wrap. A contiguous snapshot then steps by exactly one
    everywhere except at the single wrap, and a torn one steps by a whole
    capacity somewhere in the middle, which is the discontinuity an estimator
    would read as a confident wrong pitch.

    Bounded by the writer's own progress rather than by wall clock time, so an
    unevenly scheduled machine makes this slower rather than giving a different
    answer.
*/
struct TearResult
{
    long long accepted = 0;
    long long refused  = 0;
    long long torn     = 0;
};

TearResult measureTears (int capacity, int snapshotSamples, int blockSamples,
                         long long targetAccepted, double secondsCap)
{
    constexpr std::uint64_t modulus = 1ull << 20;

    Tuner::InputRing ring;
    ring.setSize (capacity);

    std::atomic<bool> stop { false };
    std::atomic<std::uint64_t> produced { 0 };

    std::thread writer ([&]
    {
        std::vector<float> block ((size_t) blockSamples, 0.0f);
        std::uint64_t counter = 0;

        while (! stop.load (std::memory_order_relaxed))
        {
            for (int i = 0; i < blockSamples; ++i)
                block[(size_t) i] = (float) (counter++ % modulus);

            ring.write (block.data(), blockSamples);
            produced.store (counter, std::memory_order_relaxed);
        }
    });

    // Wait for the ring to fill, or every early snapshot is refused for having
    // too little audio rather than for being torn.
    while (ring.totalWritten() < (std::uint64_t) capacity)
        std::this_thread::yield();

    TearResult out;
    std::vector<float> dest ((size_t) snapshotSamples);

    // Bounded by how many snapshots were ACCEPTED rather than by how much
    // audio went through, because the quantity being measured is a share of
    // the accepted ones and the figures in Tuner.h are stated as counts of
    // them. Bounding by samples written instead, which the first version of
    // this did, gave 22 accepted snapshots at one capacity and 527 at another,
    // so a claimed rate of 29 in 147572 could not have shown up either way.
    // The wall clock cap is only there so a machine that schedules the reader
    // badly makes this finish rather than hang.
    const auto startedAtTime = Clock::now();

    while (out.accepted < targetAccepted && secondsSince (startedAtTime) < secondsCap)
    {
        if (! ring.readLatest (dest))
        {
            ++out.refused;
            continue;
        }

        ++out.accepted;

        for (size_t i = 1; i < dest.size(); ++i)
        {
            const double step = (double) dest[i] - (double) dest[i - 1];

            // One step of the ramp, or the one wrap it is allowed to contain.
            if (step == 1.0 || step == -(double) (modulus - 1))
                continue;

            ++out.torn;
            break;
        }
    }

    stop.store (true, std::memory_order_relaxed);
    writer.join();

    return out;
}

//==============================================================================
int failures = 0;

void expect (bool ok, const std::string& what)
{
    if (! ok)
    {
        std::printf ("  FAIL: %s\n", what.c_str());
        ++failures;
    }
}

} // namespace

//==============================================================================
int main()
{
    std::printf ("Tuner probe: the figures Tuner.h states as measurements\n");
    std::printf ("Hardware concurrency reported as %u\n\n", std::thread::hardware_concurrency());

    //==========================================================================
    // 1. The window, which is exact arithmetic and so is the one column of the
    //    table that any machine must reproduce to the sample.
    //==========================================================================
    std::printf ("1. The analysis window at each rate\n");
    std::printf ("   The window is a fixed DURATION, so its length in samples is\n"
                 "   proportional to the rate and its duration is not.\n\n");
    std::printf ("   | sample rate | window samples | window ms |\n");
    std::printf ("   |---|---|---|\n");

    const double rates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };

    for (double rate : rates)
    {
        Tuner::Spec spec;
        spec.sampleRate = rate;

        Tuner::Detector d;
        d.configure (spec);

        std::printf ("   | %7.0f | %6d | %6.1f |\n", rate, d.requiredSampleCount(),
                     1000.0 * (double) d.requiredSampleCount() / rate);
    }

    std::printf ("\n");

    //==========================================================================
    // 2. The reading cost table, and the quadratic law that is the claim a
    //    machine cannot change.
    //==========================================================================
    std::printf ("2. What one reading costs, shipped spec, best of five\n\n");
    std::printf ("   | sample rate | window         | one reading |\n");
    std::printf ("   |---|---|---|\n");

    std::vector<CostRow> measured;

    for (double rate : rates)
    {
        const auto row = measureReading (rate, 5);
        measured.push_back (row);

        std::printf ("   | %7.0f | %6d (%3.0f ms) | %6.1f ms |\n",
                     row.sampleRate, row.windowSamples, row.windowSeconds * 1000.0,
                     row.readingSeconds * 1000.0);
    }

    std::printf ("\n   Against the table in Tuner.h, which was taken on the owner's\n"
                 "   machine in a release build: 9, 11, 48 and 197 ms.\n\n");

    // The law. A reading walks every lag for every sample of the window, both
    // of which are proportional to the rate, so doubling the rate is four times
    // the work. This is the claim the comment makes that no processor can
    // change, so it is the one held to a bound.
    std::printf ("   The quadratic law, cost(2 fs) / cost(fs), which should be near 4:\n");

    for (size_t i = 1; i < measured.size(); ++i)
    {
        const double ratio = measured[i].readingSeconds / measured[i - 1].readingSeconds;
        const double rateRatio = measured[i].sampleRate / measured[i - 1].sampleRate;
        const double expected = rateRatio * rateRatio;

        std::printf ("     %6.0f -> %6.0f Hz: %5.2fx, against %4.2fx for the square of the rate\n",
                     measured[i - 1].sampleRate, measured[i].sampleRate, ratio, expected);

        // Loose on purpose. A doubling of the rate is four times the work in
        // the limit, and at these window lengths the cache is not in the limit:
        // the 192 kHz window is megabytes and the 44.1 kHz one is not, so the
        // top of the range pays a memory cost the bottom does not and the ratio
        // is allowed to run above the square as well as below it.
        expect (ratio > 0.55 * expected && ratio < 2.0 * expected,
                "the cost did not grow about as the square of the sample rate between "
                + std::to_string ((int) measured[i - 1].sampleRate) + " and "
                + std::to_string ((int) measured[i].sampleRate) + " Hz");
    }

    std::printf ("\n");

    //==========================================================================
    // 3. What the duty cycle makes of it. Reported against both the measured
    //    cost and the cost the header claims, because the suite's vibrato sweep
    //    is handed the header's figure and the question is whether that figure
    //    still names the right band.
    //==========================================================================
    std::printf ("3. What Pacing does with each cost\n\n");

    const Tuner::Pacing::Settings shipped;
    const double claimed[] = { 0.009, 0.011, 0.048, 0.197 };

    std::printf ("   | sample rate | cost | base | mean hop | readings/s | core |\n");
    std::printf ("   |---|---|---|---|---|---|\n");

    for (size_t i = 0; i < measured.size(); ++i)
    {
        const auto fromClaim = pace (claimed[i], shipped);
        const auto fromHere  = pace (measured[i].readingSeconds, shipped);

        std::printf ("   | %7.0f | %6.1f ms claimed | %6.1f ms | %6.1f ms | %5.2f | %4.1f%% |\n",
                     measured[i].sampleRate, claimed[i] * 1000.0, fromClaim.base * 1000.0,
                     fromClaim.meanHop * 1000.0, fromClaim.readingsPerSecond,
                     fromClaim.coreShare * 100.0);

        std::printf ("   | %7s | %6.1f ms here    | %6.1f ms | %6.1f ms | %5.2f | %4.1f%% |\n",
                     "", measured[i].readingSeconds * 1000.0, fromHere.base * 1000.0,
                     fromHere.meanHop * 1000.0, fromHere.readingsPerSecond,
                     fromHere.coreShare * 100.0);

        // The duty cycle's entire job. It holds whatever the cost turns out to
        // be, because the bound is a multiple of the cost itself, so this is the
        // one figure here that a slower or faster machine cannot move.
        expect (fromHere.coreShare <= 0.25 + 1.0e-9,
                "the analysis thread took more than a quarter of a core at "
                + std::to_string ((int) measured[i].sampleRate) + " Hz");
    }

    std::printf ("\n   Tuner.h claims from the table: 8.5 readings a second at 44.1 and 48 kHz,\n"
                 "   4.5 at 96 and 1.1 at 192; mean hops of 118 ms at 44.1 and 221 ms at 96.\n\n");

    //==========================================================================
    // 4. The tear rates, which are what studioRingCapacity cites.
    //==========================================================================
    std::printf ("4. How often an accepted snapshot was torn\n\n");

    // The snapshot the figures were taken at is not recorded in Tuner.h, so it
    // is stated here: the 44.1 kHz window, which is the rate a tuner is most
    // often opened at, and the 512 sample blocks the comment does name.
    {
        Tuner::Spec spec;
        spec.sampleRate = 44100.0;

        Tuner::Detector d;
        d.configure (spec);

        const int snapshot = d.requiredSampleCount();
        const int block    = 512;

        std::printf ("   Snapshot %d samples (the 44.1 kHz window), %d sample blocks.\n\n",
                     snapshot, block);
        std::printf ("   | capacity | accepted | refused | torn | torn share |\n");
        std::printf ("   |---|---|---|---|---|\n");

        struct Case { const char* name; int capacity; };

        const Case cases[] = {
            { "snapshot + 64",  snapshot + 64 },
            { "snapshot + 512", snapshot + block },
            { "snapshot x 4",   4 * snapshot }
        };

        long long tornAtSixtyFour = -1;

        for (const auto& c : cases)
        {
            // The volume the figures in Tuner.h are quoted at, so that a rate
            // of 29 in 147572 is a thing this run could see. Capped at half a
            // minute a case either way.
            const auto r = measureTears (c.capacity, snapshot, block, 200000ll, 30.0);

            const double share = r.accepted > 0 ? (double) r.torn / (double) r.accepted : 0.0;

            std::printf ("   | %-15s %7d | %9lld | %7lld | %6lld | %6.2f%% |\n",
                         c.name, c.capacity, r.accepted, r.refused, r.torn, share * 100.0);

            if (tornAtSixtyFour < 0)
                tornAtSixtyFour = r.torn;

            // The requirement is capacity >= snapshot + 2 * block. The third
            // case clears it and the first two do not, so only the third gets a
            // bound: a torn snapshot accepted there would mean the requirement
            // is understated and studioRingCapacity is too small.
            if (c.capacity >= snapshot + 2 * block)
                expect (r.torn == 0,
                        std::string ("a capacity that meets the stated requirement still "
                                     "accepted torn snapshots, at ") + c.name);
        }

        // One sided on purpose, as the header note at the top of this file says:
        // a run that sees no tears below the requirement has lost the race
        // fewer times rather than disproved it. Reported either way.
        if (tornAtSixtyFour == 0)
            std::printf ("\n   No tears got through at the snapshot plus 64 on this run. That is\n"
                         "   this machine's scheduling and not a refutation: the requirement is\n"
                         "   an argument about what the counter can hide, not a measurement.\n");
    }

    std::printf ("\n");

    //==========================================================================
    // 5. The capacity the studio actually gives it.
    //==========================================================================
    std::printf ("5. studioRingCapacity, and what it is in seconds\n\n");

    const int given = Tuner::studioRingCapacity();

    std::printf ("   %d samples: %.2f s at 192 kHz, %.2f s at 44.1 kHz\n",
                 given, (double) given / 192000.0, (double) given / 44100.0);
    std::printf ("   Requirement at 192 kHz with the largest block: %d\n",
                 Tuner::requiredRingCapacity (Tuner::highestStudioSampleRate,
                                              Tuner::largestStudioBlockSamples));
    std::printf ("   Tuner.h claims 99756 samples, half a second at the top rate and\n"
                 "   2.26 s at 44.1 kHz.\n\n");

    //==========================================================================
    if (failures > 0)
    {
        std::printf ("%d of the probe's own bounds failed.\n", failures);
        return 1;
    }

    std::printf ("Every bound this probe asserts held.\n");
    return 0;
}
