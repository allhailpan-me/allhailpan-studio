// ---------------------------------------------------------------------------
// Checks the defaults that decide whether the studio is playable on first run.
//
// This exists because of a bug report that was not really about a setting: a
// guitarist turned on monitoring, could not play through the delay, and had
// no reason to know that the answer was a driver dropdown two menus away. The
// fix is for the studio to choose, and a choice made on the user's behalf has
// to be right without them checking it, because they will not check it. They
// will conclude the studio has latency.
//
// Both halves of that choice are worth pinning down here:
//
//   the driver order   a regression test as much as a test. Putting the
//                      heavily buffered shared mode back on this list, or
//                      ahead of the modes that are not, would quietly undo
//                      the whole fix while every other test stayed green.
//   the buffer size    arithmetic over an unsorted list, with a floor, a
//                      target, and a fallback for drivers that will not reach
//                      the target. Off by one here is a few milliseconds,
//                      which is exactly the amount under complaint.
//
// The buffer choice is checked by brute force over thousands of generated
// driver lists against an independently written reference, because the cases
// that bite are the ragged ones: lists that are not sorted, lists with one
// entry, lists where every option is on the wrong side of the target.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include "AudioDefaults.h"

#include <algorithm>
#include <cstdio>
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

static void checkEqual (int actual, int expected, const std::string& what)
{
    if (actual != expected)
        fail (what + ": got " + std::to_string (actual)
                   + ", expected " + std::to_string (expected));
}

//==============================================================================
// The driver order.
//
// Written as properties rather than as the literal list, so that adding a
// driver does not mean editing a test, but putting the wrong one in does.

static void driverOrder()
{
    const auto drivers = AudioDefaults::preferredDrivers();

    check (! drivers.empty(), "the preferred driver list is empty");

    // The complaint this whole file answers. Plain "Windows Audio" is the
    // shared, heavily buffered WASAPI mode, and DirectSound is worse. Neither
    // is a driver to move somebody onto: the studio was already getting them
    // by default, which is what the delay was.
    for (const auto& name : drivers)
    {
        check (name != "Windows Audio",
               "the buffered shared mode is back on the preferred list");
        check (name != "DirectSound",
               "DirectSound is on the preferred list");
    }

    const auto position = [&drivers] (const std::string& name)
    {
        const auto it = std::find (drivers.begin(), drivers.end(), name);
        return it == drivers.end() ? -1 : (int) (it - drivers.begin());
    };

    const int lowLatency = position ("Windows Audio (Low Latency Mode)");
    const int asio       = position ("ASIO");

    check (lowLatency >= 0, "Windows Audio (Low Latency Mode) is not preferred at all");
    check (asio >= 0,       "ASIO is not preferred at all");

    if (asio >= 0 && lowLatency >= 0)
        check (asio < lowLatency, "ASIO is not preferred first");

    // Exclusive mode holds the endpoint, so nothing else on the machine plays
    // through that device while the studio is open. That is a cost, not a
    // veto: on an audio interface it costs close to nothing, and the
    // alternative is somebody who cannot play. It has to be known about so
    // that it comes last and so that the studio can say it took the device.
    const auto taking = AudioDefaults::driversThatTakeTheDevice();

    check (! taking.empty(), "nothing is marked as taking the device over");

    for (const auto& name : taking)
    {
        check (! name.empty(), "a driver that takes the device has no name");
        check (AudioDefaults::takesTheDevice (name),
               "\"" + name + "\" is on the list but is not recognised by name");
        check (std::find (drivers.begin(), drivers.end(), name) == drivers.end(),
               "\"" + name + "\" is on both lists, so it would be tried twice");
    }

    check (std::find (taking.begin(), taking.end(),
                      std::string ("Windows Audio (Exclusive Mode)")) != taking.end(),
           "exclusive mode is not named as taking the device over, so nothing can "
           "reach for it when it is the only thing left");

    for (const auto& name : drivers)
        check (! AudioDefaults::takesTheDevice (name),
               "\"" + name + "\" is preferred but is also said to take the device");
}

//==============================================================================
/** The order everything is tried in, which is the whole policy. */
static void searchOrder()
{
    const auto order = AudioDefaults::searchOrder();
    const auto preferred = AudioDefaults::preferredDrivers();
    const auto taking = AudioDefaults::driversThatTakeTheDevice();

    checkEqual ((int) order.size(), (int) (preferred.size() + taking.size()),
                "the search order is not everything on both lists");

    // Nothing that takes the device may be tried before something that does
    // not. A faster driver is not worth having if a slower one would have
    // done without silencing the rest of the machine.
    bool seenOneThatTakes = false;

    for (const auto& candidate : order)
    {
        if (candidate.takesTheDevice)
            seenOneThatTakes = true;
        else
            check (! seenOneThatTakes,
                   "\"" + candidate.name + "\" is tried after a driver that takes the "
                   "device, so the studio could hold the sound card when it did not have to");

        check (AudioDefaults::takesTheDevice (candidate.name) == candidate.takesTheDevice,
               "\"" + candidate.name + "\" is flagged differently from how it is classified");
    }

    check (seenOneThatTakes,
           "nothing in the search order takes the device, so a machine where only "
           "exclusive mode is fast has nothing left to try");

    // And the order within the preferred part is preserved, since that is
    // where ASIO being first matters. Guarded, because nothing here aborts on
    // a failure: without this a short order would be an out of bounds read
    // and the suite would report a crash rather than the named problem.
    if (order.size() < preferred.size())
        return;

    for (std::size_t i = 0; i < preferred.size(); ++i)
        check (order[i].name == preferred[i],
               "the search order does not follow the preferred order");

    // Across the whole order, not just the preferred part, since the two are
    // now tried from one list.
    std::vector<std::string> names;

    for (const auto& candidate : order)
        names.push_back (candidate.name);

    // No name may be a substring of another. The engine compares these
    // exactly, but a later change to matching by substring would otherwise
    // silently start matching the wrong driver, and "Windows Audio" matching
    // all three WASAPI modes is precisely the failure being fixed. This is
    // the check that would catch somebody adding plain "Windows Audio" back.
    for (const auto& a : names)
        for (const auto& b : names)
            if (&a != &b)
                check (b.find (a) == std::string::npos,
                       "\"" + a + "\" is a substring of \"" + b + "\"");

    // Duplicates would mean trying the same driver twice and skipping a real
    // candidate if the first attempt failed for a reason the second shares.
    auto sorted = names;
    std::sort (sorted.begin(), sorted.end());
    check (std::adjacent_find (sorted.begin(), sorted.end()) == sorted.end(),
           "the search order has a duplicate");
}

//==============================================================================
// The buffer size.

/** An independently written answer to the same question, by sorting and
    scanning rather than by a single pass. Deliberately not the same shape as
    the implementation: two copies of one mistake would agree with each other. */
static int referenceChoice (std::vector<int> available, double sampleRate,
                            double targetSeconds)
{
    if (sampleRate <= 0.0)
        return 0;

    available.erase (std::remove_if (available.begin(), available.end(),
                                     [] (int s) { return s <= 0; }),
                     available.end());

    if (available.empty())
        return 0;

    std::sort (available.begin(), available.end());

    double wanted = sampleRate * targetSeconds;
    int target = (int) std::lround (wanted);

    if (target < 64)
        target = 64;

    for (int size : available)
        if (size >= target)
            return size;

    return available.back();
}

static void nothingToChooseFrom()
{
    checkEqual (AudioDefaults::chooseBufferSize ({}, 48000.0), 0,
                "an empty list should choose nothing");
    checkEqual (AudioDefaults::chooseBufferSize ({ 128, 256 }, 0.0), 0,
                "a sample rate of zero should choose nothing");
    checkEqual (AudioDefaults::chooseBufferSize ({ 128, 256 }, -48000.0), 0,
                "a negative sample rate should choose nothing");
    checkEqual (AudioDefaults::chooseBufferSize ({ 0, -256 }, 48000.0), 0,
                "a list of impossible sizes should choose nothing");

    // One bad entry among good ones must not become the answer, including
    // when the good ones are all below the target so the fallback runs.
    checkEqual (AudioDefaults::chooseBufferSize ({ 0, 64, 128 }, 48000.0), 128,
                "an impossible size leaked into the fallback");
}

static void theUsualDrivers()
{
    // Two and a half milliseconds at 48 kHz is 120 samples, so 128 covers it.
    // That is deliberately the first size Windows offers on a driver that
    // supports small buffers: Microsoft document the inbox HDAudio driver as
    // supporting 128 to 480, and taking 256 instead would throw away most of
    // what Low Latency Mode exists to provide.
    checkEqual (AudioDefaults::chooseBufferSize ({ 64, 128, 256, 512, 1024 }, 48000.0), 128,
                "48 kHz with the usual sizes");

    // 44.1 kHz asks for 110.25, which must not round down into 64.
    checkEqual (AudioDefaults::chooseBufferSize ({ 64, 128, 256, 512, 1024 }, 44100.0), 128,
                "44.1 kHz with the usual sizes");

    // Higher rates need proportionally larger buffers for the same delay,
    // which is the part that gets forgotten when a buffer size is hardcoded.
    checkEqual (AudioDefaults::chooseBufferSize ({ 64, 128, 256, 512, 1024 }, 96000.0), 256,
                "96 kHz with the usual sizes");
    checkEqual (AudioDefaults::chooseBufferSize ({ 64, 128, 256, 512, 1024, 2048 }, 192000.0), 512,
                "192 kHz with the usual sizes");

    // The exact figure a 48 kHz driver would be asked for, which must not be
    // skipped over in favour of the next one up.
    checkEqual (AudioDefaults::chooseBufferSize ({ 120, 240, 480 }, 48000.0), 120,
                "an exact match should be taken");

    // The real shape of a Low Latency Mode list: the minimum period, then
    // multiples of the fundamental period up to the maximum.
    checkEqual (AudioDefaults::chooseBufferSize ({ 128, 160, 192, 224, 256, 480 }, 48000.0), 128,
                "a low latency mode buffer size list");

    // The list order is the driver's business. ASIO devices in particular
    // report whatever order they like.
    checkEqual (AudioDefaults::chooseBufferSize ({ 1024, 64, 512, 128, 256 }, 48000.0), 128,
                "an unsorted list");

    // One size offered, which is what a device in exclusive mode can look
    // like. There is no choice, so take it either way round.
    checkEqual (AudioDefaults::chooseBufferSize ({ 480 }, 48000.0), 480,
                "a single size above the target");
    checkEqual (AudioDefaults::chooseBufferSize ({ 96 }, 48000.0), 96,
                "a single size below the target");
}

static void driversThatWillNotReachTheTarget()
{
    // Everything on offer is below the target, so the target cannot be met and
    // the largest is the safest of the options rather than the worst of them.
    checkEqual (AudioDefaults::chooseBufferSize ({ 32, 64, 96 }, 48000.0), 96,
                "all sizes below the target should take the largest");
    checkEqual (AudioDefaults::chooseBufferSize ({ 96, 32, 64 }, 48000.0), 96,
                "all sizes below the target, unsorted");
}

static void theFloor()
{
    // At low sample rates the target in samples gets small enough that a
    // handful of samples per block would satisfy it, and at that point the
    // per block overhead is most of the work. Sixty four is the floor.
    //
    // 8 kHz wants 40 samples; without the floor this takes 48.
    checkEqual (AudioDefaults::chooseBufferSize ({ 16, 32, 48, 64, 128 }, 8000.0), 64,
                "the sixty four sample floor was not applied");

    // And with the floor in place there is still a sensible answer when the
    // driver cannot reach it.
    checkEqual (AudioDefaults::chooseBufferSize ({ 16, 32, 48 }, 8000.0), 48,
                "below the floor with nothing that reaches it");
}

static void aDifferentTarget()
{
    // The target is a parameter so that it can be tested and so that a
    // different one can be asked for. Ten milliseconds at 48 kHz is 480.
    checkEqual (AudioDefaults::chooseBufferSize ({ 64, 128, 256, 512 }, 48000.0, 0.010), 512,
                "a ten millisecond target");
    checkEqual (AudioDefaults::chooseBufferSize ({ 64, 128, 256, 512 }, 48000.0, 0.0005), 64,
                "a target below the floor should land on the floor");
}

static void defaultTargetIsPlayable()
{
    // The point of the default: one way at the target, doubled for the round
    // trip, has to leave room for the driver's own buffering either side and
    // still come out somewhere a player can work. If this ever stops being
    // true the number was changed without the thresholds being revisited.
    const double bothWays = 2000.0 * AudioDefaults::targetBufferSeconds;

    check (AudioDefaults::playable (bothWays),
           "the default target is not even playable before the driver adds anything");

    // And it has to leave room for the driver's own buffering either side
    // before it stops being playable, since that is added on top and is not
    // ours to choose. Expressed against the thresholds rather than against
    // numbers, so that moving one of them cannot leave this passing by
    // accident.
    double headroom = 0.0;
    while (AudioDefaults::playable (bothWays + headroom))
        headroom += 0.1;

    check (headroom >= bothWays,
           "the default target leaves less headroom for the driver than it uses itself");
}

static void estimatingWhenTheDriverWillNotSay()
{
    // Reported figures win when there are any.
    checkEqual ((int) std::lround (AudioDefaults::roundTripOrEstimate (256, 256, 256, 48000.0) * 100.0),
                1067, "a driver that reports its latency should be believed");

    // And when there are none, two buffers is the floor and ranks correctly.
    // This is the ASIO case: JUCE zeroes both figures when getLatencies fails.
    checkEqual ((int) std::lround (AudioDefaults::roundTripOrEstimate (0, 0, 128, 48000.0) * 100.0),
                533, "a driver that reports nothing should be estimated from its buffer");

    check (AudioDefaults::roundTripOrEstimate (0, 0, 128, 48000.0) > 0.0,
           "an estimate of zero would be read as a perfect device");

    // An interface reporting nothing at a small buffer has to come out
    // playable, or the search throws away the one driver that would have
    // worked and falls back to the slow one.
    check (AudioDefaults::playable (AudioDefaults::roundTripOrEstimate (0, 0, 128, 48000.0)),
           "a fast driver that reports no latency is not being recognised as playable");

    // And one at a large buffer must not be.
    check (! AudioDefaults::playable (AudioDefaults::roundTripOrEstimate (0, 0, 2048, 48000.0)),
           "a slow driver that reports no latency is being treated as playable");

    checkEqual ((int) AudioDefaults::roundTripOrEstimate (0, 0, 0, 48000.0), 0,
                "nothing known at all should stay zero rather than be invented");
    checkEqual ((int) AudioDefaults::roundTripOrEstimate (256, 256, 256, 0.0), 0,
                "an unknown sample rate should stay zero rather than divide by it");
}

static void roundTripInSamples()
{
    checkEqual (AudioDefaults::roundTripSamples (240, 264, 128), 504,
                "a reported round trip is not passed through in samples");

    // A reported zero means the driver did not say, so the floor any round
    // trip can have stands in for it. See roundTripOrEstimate.
    checkEqual (AudioDefaults::roundTripSamples (0, 0, 128), 256,
                "a driver that reported nothing was taken at its word");

    checkEqual (AudioDefaults::roundTripSamples (0, 0, 0), 0,
                "with nothing reported and no buffer there is nothing to guess from");

    // A driver reporting something negative is reporting nonsense, and the
    // answer has to be the floor rather than a round trip below zero, which
    // would move a recorded take later for no reason at all.
    check (AudioDefaults::roundTripSamples (-480, 0, 128) >= 0,
           "a negative report produced a negative round trip");
    check (AudioDefaults::roundTripSamples (-480, -480, 0) >= 0,
           "a negative report with no buffer produced a negative round trip");

    // Nothing a driver can claim may overflow the addition, which is what the
    // bound is for rather than any musical reason.
    const int huge = AudioDefaults::roundTripSamples (2147483647, 2147483647, 2147483647);
    check (huge > 0 && huge <= 2 * AudioDefaults::maxLatencySamples,
           "an absurd report was not bounded");

    // The milliseconds version is the same rule divided by the sample rate,
    // so the two can never disagree about whether a driver said anything.
    for (const int buffer : { 0, 64, 128, 480, 2048 })
        for (const int in : { 0, 1, 240, 4800 })
            for (const int out : { 0, 1, 264, 4800 })
            {
                const double ms = AudioDefaults::roundTripOrEstimate (in, out, buffer, 48000.0);
                const double expected = 1000.0 * AudioDefaults::roundTripSamples (in, out, buffer)
                                          / 48000.0;
                check (std::abs (ms - expected) < 1.0e-9,
                       "the two round trip figures disagree");
            }
}

static void roundTrip()
{
    checkEqual ((int) std::lround (AudioDefaults::roundTripMs (256, 256, 48000.0) * 100.0),
                1067, "round trip of two 256 sample buffers at 48 kHz, in hundredths of a ms");

    check (AudioDefaults::roundTripMs (0, 0, 48000.0) == 0.0,
           "no latency should read as zero");
    check (AudioDefaults::roundTripMs (256, 256, 0.0) == 0.0,
           "an unknown sample rate should read as zero rather than divide by it");

    // Input and output count the same, and both count.
    check (AudioDefaults::roundTripMs (480, 0, 48000.0)
             == AudioDefaults::roundTripMs (0, 480, 48000.0),
           "input and output latency should contribute equally");
    check (AudioDefaults::roundTripMs (480, 480, 48000.0)
             > AudioDefaults::roundTripMs (480, 0, 48000.0),
           "output latency is being ignored");
}

static void thresholds()
{
    // The two judgements have to agree with each other. Anything playable is
    // by definition not too slow to play, whatever the numbers are set to.
    for (double ms = 0.0; ms <= 120.0; ms += 0.25)
        if (AudioDefaults::playable (ms))
            check (! AudioDefaults::tooSlowToPlay (ms),
                   "a round trip is both playable and too slow to play");

    check (! AudioDefaults::playable (0.0),
           "a round trip of zero means nothing was measured, and must not read as "
           "the best possible score: a search that believes it stops on the one "
           "driver that would not answer");
    check (! AudioDefaults::playable (-3.0), "a negative round trip should not be playable");
    check (AudioDefaults::playable (5.0),   "five milliseconds should be playable");
    check (! AudioDefaults::playable (40.0), "forty milliseconds should not be playable");
    check (AudioDefaults::tooSlowToPlay (50.0),
           "fifty milliseconds should be worth warning about");
    check (! AudioDefaults::tooSlowToPlay (12.0),
           "twelve milliseconds should not be worth warning about");
}

//==============================================================================
static void randomised()
{
    std::mt19937 rng (20260119);

    const std::vector<double> rates { 8000.0, 22050.0, 32000.0, 44100.0, 48000.0,
                                      88200.0, 96000.0, 176400.0, 192000.0 };
    const std::vector<double> targets { 0.0005, 0.001, 0.002,
                                        AudioDefaults::targetBufferSeconds,
                                        0.010, 0.020, 0.050 };

    std::uniform_int_distribution<int> howMany (1, 12);
    std::uniform_int_distribution<int> aSize (-64, 4096);
    std::uniform_int_distribution<int> anOffset (-2, 2);
    std::uniform_int_distribution<size_t> aRate (0, rates.size() - 1);
    std::uniform_int_distribution<size_t> aTarget (0, targets.size() - 1);
    std::bernoulli_distribution nearTheEdge (0.5);

    int disagreements = 0;
    int postconditions = 0;

    for (int trial = 0; trial < 20000; ++trial)
    {
        const double rate   = rates[aRate (rng)];
        const double target = targets[aTarget (rng)];

        std::vector<int> available;

        // Sizes drawn uniformly almost never land on a boundary, and the
        // boundaries are where this can be wrong: a size exactly at the
        // target, one either side of it, and the same around the floor. The
        // first version of this test drew uniformly and let a target that
        // truncated instead of rounding pass.
        const int wanted = (int) (rate * target);

        for (int i = howMany (rng); --i >= 0;)
        {
            if (nearTheEdge (rng))
                available.push_back ((nearTheEdge (rng) ? wanted : 64) + anOffset (rng));
            else
                available.push_back (aSize (rng));
        }

        const int chosen = AudioDefaults::chooseBufferSize (available, rate, target);

        if (chosen != referenceChoice (available, rate, target))
            ++disagreements;

        // Postconditions, checked separately from the reference so that a
        // shared misunderstanding between the two still gets caught.
        if (chosen != 0)
        {
            const bool present = std::find (available.begin(), available.end(), chosen)
                                   != available.end();
            if (! present || chosen <= 0)
                ++postconditions;

            // Nothing smaller than the choice may also have met the target:
            // if one did, a smaller delay was available and was passed over.
            const int atLeast = std::max (64, (int) std::lround (rate * target));

            if (chosen > atLeast)
                for (int size : available)
                    if (size > 0 && size >= atLeast && size < chosen)
                        ++postconditions;
        }
    }

    checkEqual (disagreements, 0, "randomised trials disagreed with the reference");
    checkEqual (postconditions, 0, "randomised trials broke a postcondition");
}

//==============================================================================
int main()
{
    driverOrder();
    searchOrder();
    nothingToChooseFrom();
    theUsualDrivers();
    driversThatWillNotReachTheTarget();
    theFloor();
    aDifferentTarget();
    defaultTargetIsPlayable();
    roundTrip();
    roundTripInSamples();
    estimatingWhenTheDriverWillNotSay();
    thresholds();
    randomised();

    if (failures != 0)
    {
        std::printf ("AudioDefaultsTest: %d failure(s)\n", failures);
        return 1;
    }

    std::printf ("AudioDefaultsTest: all checks passed\n");
    return 0;
}
