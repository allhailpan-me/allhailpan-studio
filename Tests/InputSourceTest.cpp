// ---------------------------------------------------------------------------
// Checks which input channel the studio listens to, and where it lands.
//
// This covers a bug that made the studio unusable for the commonest thing
// anybody does with it. Input 1 was hardwired to the left of a stereo pair and
// input 2 to the right, so a guitar plugged into input 1 of an interface, with
// nothing in input 2, was heard hard left and recorded as a stereo take with
// silence down one side.
//
// What makes it worth a test file rather than a one line fix is that it
// presents as an output fault. One speaker is an output symptom, so that is
// where somebody goes looking, and nothing they find there can help. The
// property below, that a mono source is centred, is the one that has to hold
// for the studio to be usable at all, and it is asserted directly.
//
// The rest is off by one arithmetic against a real device: never an index the
// interface does not have, never reading past the last channel, and never
// silently moving somebody onto an input they are not plugged into.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include "InputSource.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <set>
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

static void checkEqual (int actual, int expected, const std::string& what)
{
    if (actual != expected)
        fail (what + ": got " + std::to_string (actual)
                   + ", expected " + std::to_string (expected));
}

//==============================================================================
static void aMonoSourceIsCentred()
{
    // The bug, stated as the property it broke. A mono input must reach both
    // speakers, or the studio is unusable for recording one instrument.
    for (int channels = 1; channels <= 32; ++channels)
    {
        for (int pick = 0; pick < channels; ++pick)
        {
            const auto resolved = InputSource::resolve ({ pick, false }, channels);

            check (resolved.valid, "a device with inputs resolved to nothing");
            check (resolved.isCentred(),
                   "input " + std::to_string (pick + 1) + " of " + std::to_string (channels)
                     + " is not centred, so it comes out of one speaker");
            checkEqual (resolved.left, pick, "a mono source read the wrong channel");
        }
    }
}

static void theDefaultIsAMonoInstrument()
{
    const auto fallback = InputSource::defaultChoice();

    check (! fallback.stereo,
           "the default is a stereo pair, so one guitar in input 1 is heard on one side");
    checkEqual (fallback.first, 0, "the default is not the first input");

    const auto resolved = InputSource::resolve (fallback, 2);
    check (resolved.isCentred(), "the default is not centred");
    checkEqual (resolved.left, 0, "the default is not the first input once resolved");
}

static void aStereoPairStaysAPair()
{
    const auto resolved = InputSource::resolve ({ 0, true }, 2);

    check (! resolved.isCentred(), "a stereo pair was folded to the middle");
    checkEqual (resolved.left, 0, "the left of the pair is wrong");
    checkEqual (resolved.right, 1, "the right of the pair is wrong");

    const auto higher = InputSource::resolve ({ 4, true }, 8);
    checkEqual (higher.left, 4, "a higher pair picked the wrong left");
    checkEqual (higher.right, 5, "a higher pair picked the wrong right");
}

//==============================================================================
static void nothingReadsPastTheLastChannel()
{
    // The one that is not a wrong number but a crash: on the audio thread,
    // reading inputChannelData[n] where the device has n channels.
    for (int channels = 1; channels <= 32; ++channels)
    {
        for (int pick = -4; pick < channels + 4; ++pick)
        {
            for (bool stereo : { false, true })
            {
                const auto resolved = InputSource::resolve ({ pick, stereo }, channels);

                check (resolved.valid, "a device with inputs resolved to nothing");
                check (resolved.left >= 0 && resolved.left < channels,
                       "the left index is outside the device's channels");
                check (resolved.right >= 0 && resolved.right < channels,
                       "the right index is outside the device's channels");
            }
        }
    }
}

static void astereoPairOnTheLastChannelFallsBackToMono()
{
    // An interface with an odd number of inputs, or a pair asked for on the
    // very last channel. Centring is the safe answer: the alternative is
    // reading a channel that does not exist.
    const auto odd = InputSource::resolve ({ 2, true }, 3);
    check (odd.isCentred(),
           "a pair starting on the last input did not fall back to mono");
    checkEqual (odd.left, 2, "the fallback moved the person off their input");

    const auto single = InputSource::resolve ({ 0, true }, 1);
    check (single.isCentred(), "a pair on a one input device did not fall back to mono");
    checkEqual (single.left, 0, "a one input device read the wrong channel");
}

static void aDeviceWithNoInputsSaysSo()
{
    for (int bad : { 0, -1, -16 })
    {
        const auto resolved = InputSource::resolve ({ 0, false }, bad);
        check (! resolved.valid,
               "a device with " + std::to_string (bad) + " inputs claimed to have one");
    }

    check (InputSource::options (0).empty(), "a device with no inputs offered one anyway");
    check (InputSource::options (-3).empty(), "a nonsense channel count offered an input");
}

//==============================================================================
static void everyOptionOfferedIsAnOptionThatWorks()
{
    for (int channels = 1; channels <= 32; ++channels)
    {
        const auto all = InputSource::options (channels);

        check (! all.empty(), "a device with inputs offered none");

        int monoCount = 0, stereoCount = 0;

        for (const auto& choice : all)
        {
            const auto resolved = InputSource::resolve (choice, channels);

            check (resolved.valid, "an offered input resolved to nothing");
            check (resolved.left >= 0 && resolved.left < channels,
                   "an offered input is outside the device's channels");
            check (resolved.right >= 0 && resolved.right < channels,
                   "an offered input is outside the device's channels");

            // An offered stereo pair must actually be a pair. Offering one
            // that quietly folds to mono is a menu entry that lies.
            if (choice.stereo)
            {
                check (! resolved.isCentred(),
                       "a stereo pair was offered that is not really a pair");
                ++stereoCount;
            }
            else
            {
                check (resolved.isCentred(), "an offered mono input is not centred");
                ++monoCount;
            }
        }

        checkEqual (monoCount, channels, "there should be one mono input per channel");
        checkEqual (stereoCount, channels / 2, "the wrong number of stereo pairs was offered");
    }
}

static void namesCountFromOne()
{
    // The back of an interface is numbered from one, and nobody counts their
    // inputs from zero.
    check (InputSource::describe ({ 0, false }) == "In 1", "the first input is not called In 1");
    check (InputSource::describe ({ 7, false }) == "In 8", "the eighth input is misnamed");
    check (InputSource::describe ({ 0, true })  == "In 1+2", "the first pair is misnamed");
    check (InputSource::describe ({ 4, true })  == "In 5+6", "a higher pair is misnamed");
    check (InputSource::describe ({ -2, false }) == "In 1", "a nonsense input is not named safely");
}

static void aChoiceSurvivesBeingSaved()
{
    for (int first = 0; first < 32; ++first)
    {
        for (bool stereo : { false, true })
        {
            const InputSource::Choice choice { first, stereo };
            const auto back = InputSource::fromStored (InputSource::toStored (choice));

            checkEqual (back.first, first, "a saved input channel came back different");
            check (back.stereo == stereo, "a saved input lost whether it was a pair");
        }
    }

    // A settings file can hold anything. Whatever it holds, what comes out
    // has to be usable, since the alternative is a studio that will not pass
    // audio until somebody finds the setting.
    for (int stored : { -1, -99 })
    {
        const auto back = InputSource::fromStored (stored);
        check (back.first >= 0, "a corrupt setting produced a negative input");
    }

    const auto resolvedFromJunk = InputSource::resolve (InputSource::fromStored (99999), 2);
    check (resolvedFromJunk.valid && resolvedFromJunk.left < 2,
           "a corrupt setting survived into an index the device does not have");
}

//==============================================================================
// Per insert assignment, and Auto-map.

static void nothingIsTheCommonCase()
{
    const InputSource::Assignment none;

    check (! none.assigned, "an insert starts out wired to an input");
    checkEqual (InputSource::assignmentToStored (none), 0,
                "an unassigned insert should store as zero, so an unwired mixer is all zeroes");
    check (! InputSource::assignmentFromStored (0).assigned,
           "zero should read back as nothing");

    // Anything a settings file can hold has to come back as something, and
    // "nothing" is the safe something: an insert that quietly starts carrying
    // a live input is feedback waiting to happen.
    for (int junk : { -1, -500 })
        check (! InputSource::assignmentFromStored (junk).assigned,
               "a corrupt setting wired an insert to an input");
}

static void anAssignmentSurvivesBeingSaved()
{
    for (int first = 0; first < 32; ++first)
    {
        for (bool stereo : { false, true })
        {
            const InputSource::Assignment a { { first, stereo }, true };
            const auto back = InputSource::assignmentFromStored (
                                  InputSource::assignmentToStored (a));

            check (back.assigned, "a saved assignment came back as nothing");
            checkEqual (back.choice.first, first, "a saved assignment changed channel");
            check (back.choice.stereo == stereo, "a saved assignment lost whether it was a pair");
        }
    }

    // The one value that must never collide with a real assignment.
    for (int first = 0; first < 32; ++first)
        for (bool stereo : { false, true })
            check (InputSource::assignmentToStored ({ { first, stereo }, true }) != 0,
                   "a real assignment stored as zero, which means nothing");
}

static void autoMapGivesEveryInputItsOwnInsert()
{
    // Eight inputs, as on the interface this was built for, mapped one per
    // insert from insert 1 upward.
    const auto mapped = InputSource::autoMap (1, 17, 8, false);

    checkEqual ((int) mapped.size(), 17, "auto-map should answer for every insert");
    check (! mapped[0].assigned, "auto-map wired the master, which is where everything is summed");

    for (int i = 1; i <= 8; ++i)
    {
        check (mapped[(size_t) i].assigned,
               "insert " + std::to_string (i) + " was left unwired");
        checkEqual (mapped[(size_t) i].choice.first, i - 1,
                    "insert " + std::to_string (i) + " got the wrong input");
        check (! mapped[(size_t) i].choice.stereo, "a mono auto-map produced a pair");
    }

    for (int i = 9; i < 17; ++i)
        check (! mapped[(size_t) i].assigned,
               "insert " + std::to_string (i) + " was wired to an input that does not exist");
}

static void autoMapStartsWhereItIsToldTo()
{
    const auto mapped = InputSource::autoMap (5, 17, 4, false);

    for (int i = 1; i < 5; ++i)
        check (! mapped[(size_t) i].assigned,
               "auto-map wired an insert before the one it was started from");

    for (int i = 5; i <= 8; ++i)
    {
        check (mapped[(size_t) i].assigned, "auto-map stopped short");
        checkEqual (mapped[(size_t) i].choice.first, i - 5, "auto-map got the wrong input");
    }
}

static void autoMapInPairsTakesTwoChannelsAtATime()
{
    const auto mapped = InputSource::autoMap (1, 17, 8, true);

    for (int i = 1; i <= 4; ++i)
    {
        check (mapped[(size_t) i].assigned, "a pair auto-map stopped short");
        check (mapped[(size_t) i].choice.stereo, "a pair auto-map produced a mono input");
        checkEqual (mapped[(size_t) i].choice.first, (i - 1) * 2,
                    "a pair auto-map got the wrong first channel");
    }

    check (! mapped[5].assigned, "a pair auto-map ran past the inputs it had");

    // An odd number of inputs has no pair at the top, and inventing one would
    // read a channel the device does not have.
    const auto odd = InputSource::autoMap (1, 17, 5, true);
    checkEqual ((int) odd[1].choice.first, 0, "the first pair is wrong");
    checkEqual ((int) odd[2].choice.first, 2, "the second pair is wrong");
    check (! odd[3].assigned, "a pair was invented from the last odd channel");
}

static void autoMapNeverWrapsOrOverlaps()
{
    std::mt19937 rng (20260307);
    std::uniform_int_distribution<int> aStart (-4, 20);
    std::uniform_int_distribution<int> anInsertCount (1, 20);
    std::uniform_int_distribution<int> aChannelCount (0, 40);

    for (int trial = 0; trial < 20000; ++trial)
    {
        const int inserts  = anInsertCount (rng);
        const int channels = aChannelCount (rng);
        const bool pairs   = (trial & 1) != 0;
        const int requestedStart = aStart (rng);

        const auto mapped = InputSource::autoMap (requestedStart, inserts, channels, pairs);

        checkEqual ((int) mapped.size(), inserts, "auto-map answered for the wrong number of inserts");

        if (! mapped.empty())
            check (! mapped[0].assigned, "auto-map wired the master");

        std::set<int> taken;
        int lastInsert = -1;
        int assignedCount = 0;

        for (size_t i = 0; i < mapped.size(); ++i)
        {
            if (! mapped[i].assigned)
                continue;

            const auto& choice = mapped[i].choice;

            // Inside the device. An insert wired to a channel the interface
            // does not have is silent, and silently so.
            check (choice.first >= 0 && choice.first < channels,
                   "auto-map wired an insert to a channel the device does not have");

            if (pairs)
                check (choice.first + 1 < channels,
                       "a mapped pair runs past the last channel of the device");

            check (choice.stereo == pairs, "auto-map mixed mono and pairs");

            // Unique. Two inserts on one socket is not a mapping, and the
            // whole point of Auto-map is that each input gets its own place.
            check (taken.insert (choice.first).second,
                   "auto-map gave the same input to two inserts");

            // Ascending, which is what "working to the right" means.
            check ((int) i > lastInsert, "auto-map went backwards");
            lastInsert = (int) i;
            ++assignedCount;
        }

        // And it has to map as many as it could: the smaller of the inputs
        // available and the inserts left above the master. Mapping fewer
        // would leave a socket unwired after the one action whose entire
        // purpose is to wire them all, and nothing would say so.
        const int step            = pairs ? 2 : 1;
        const int startedAt       = std::max (1, requestedStart);
        const int insertsLeft     = std::max (0, inserts - startedAt);
        const int unitsAvailable  = channels / step;

        checkEqual (assignedCount, std::min (insertsLeft, unitsAvailable),
                    "auto-map wired fewer inserts than it had inputs and room for");
    }
}

//==============================================================================
int main()
{
    std::printf ("input source\n");

    aMonoSourceIsCentred();
    theDefaultIsAMonoInstrument();
    aStereoPairStaysAPair();
    nothingReadsPastTheLastChannel();
    astereoPairOnTheLastChannelFallsBackToMono();
    aDeviceWithNoInputsSaysSo();
    everyOptionOfferedIsAnOptionThatWorks();
    namesCountFromOne();
    aChoiceSurvivesBeingSaved();
    nothingIsTheCommonCase();
    anAssignmentSurvivesBeingSaved();
    autoMapGivesEveryInputItsOwnInsert();
    autoMapStartsWhereItIsToldTo();
    autoMapInPairsTakesTwoChannelsAtATime();
    autoMapNeverWrapsOrOverlaps();

    if (failures != 0)
    {
        std::printf ("InputSourceTest: %d failure(s)\n", failures);
        return 1;
    }

    std::printf ("all input source checks passed\n");
    return 0;
}
