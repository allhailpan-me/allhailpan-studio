#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

//==============================================================================
/** Choosing a driver and a buffer size so that the studio is playable before
    anyone opens the audio settings.

    This exists because of a real complaint, and the complaint was right: a
    guitarist plugged in, heard a delay he could not play through, and had no
    reason to know that the answer was buried in a dropdown two menus away.
    Asking a musician to understand WASAPI sharing modes before they can play
    a part is not a settings problem, it is a defect.

    On Windows the operating system offers three ways into the same hardware,
    and the difference between them is not small:

      Windows Audio                     shared, heavily buffered, tens of
                                        milliseconds, and the default. Worse
                                        than heavily buffered: JUCE offers a
                                        single buffer size on this mode and
                                        ignores any request to change it,
                                        because the wakeup period belongs to
                                        the driver. The buffer size control
                                        really does nothing here, which is
                                        exactly what it was reported to do
      Windows Audio (Low Latency Mode)  shared, but negotiated through
                                        IAudioClient3, single digit
                                        milliseconds on Windows 10 and later
      Windows Audio (Exclusive Mode)    low latency, but takes the device, so
                                        nothing else on the machine can make
                                        a sound

    Low Latency Mode is the one to want where it works: it is fast and it still
    shares, so a video in a browser keeps playing. It is not always available.
    Windows only offers small shared buffers if the device's driver opted in,
    and the makers of audio interfaces mostly did not, because they ship an
    ASIO driver instead and expect it to be used. On that hardware Low Latency
    Mode reports the same ten milliseconds as the default and there is nothing
    to be gained from it.

    Which leaves exclusive mode, and a rule this file got wrong at first.
    Refusing to choose it automatically sounds careful, and on a laptop with
    one sound card it is: taking the card silences the browser. But somebody
    with an audio interface is in the opposite position. Their system sounds
    are going somewhere else, the interface exists to monitor through, and
    every other studio on their machine takes it exclusively already. Refusing
    on their behalf does not protect them from anything, it just leaves them
    unable to play, which is the complaint this whole file exists to answer.

    So exclusive mode is now the last thing tried rather than the one thing
    refused, and whenever the studio ends up there by itself it says so.

    The arithmetic is here rather than in the engine so it can be tested. The
    driver choice cannot be: it depends on what the machine has.
*/
namespace AudioDefaults
{
    /** Drivers the studio may move somebody onto without asking, best first.

        ASIO leads, and on Windows it is usually the only thing that matters.
        An interface's own driver goes straight at the hardware, and the
        makers of interfaces put their effort there rather than into Windows'
        low latency shared mode, so on that hardware it is not merely the
        fastest option, it is the only fast one.

        It is on this list rather than among the ones that take the device,
        even though an ASIO driver is often exclusive to one application,
        because having an ASIO driver at all means having an interface, and an
        interface is not where the rest of the machine's sound is going. There
        is nothing to silence. Every other studio on that machine claims it
        the same way, which is what the person expects.

        Public builds did not have ASIO at all until the SDK was dual licensed
        under GPL-3.0 on 15 October 2025, which is compatible with this
        project's AGPL-3.0 through section 13 of each. Before that, a machine
        with a perfectly good interface had nothing here that could reach it.

        Then Low Latency Mode, which is the one that makes this worth doing.
        CoreAudio and JACK are already low latency, and ALSA is last because
        it is the fallback rather than a choice.

        Anything not on this list is left alone: being unrecognised is not a
        reason to move someone off a driver that works.
    */
    inline std::vector<std::string> preferredDrivers()
    {
        return { "ASIO",
                 "Windows Audio (Low Latency Mode)",
                 "CoreAudio",
                 "JACK",
                 "ALSA" };
    }

    /** Fast, and worth saying something about afterwards.

        Exclusive mode bypasses the Windows audio engine and holds the
        endpoint. Microsoft's own description of the two modes is that in
        shared mode "the audio engine mixes the streams from these
        applications", while in exclusive mode "the client has exclusive
        access to the audio hardware", which means nothing else on the machine
        plays through that device for as long as the studio is open.

        That is a real cost and it is why these come last, after everything
        that does not have it. It is not a reason to refuse: on an audio
        interface the cost is close to nothing, since the system is not
        playing through the interface anyway, and the alternative is a
        guitarist who cannot play. What it is a reason for is saying so, which
        is why this is a list of its own rather than three more entries on the
        one above.
    */
    inline std::vector<std::string> driversThatTakeTheDevice()
    {
        return { "Windows Audio (Exclusive Mode)" };
    }

    /** One driver to try, in order. */
    struct Candidate
    {
        std::string name;
        bool takesTheDevice = false;
    };

    /** Everything worth trying, best first, with the ones that hold the
        device last.

        The order is the whole policy, so it is built here from the two lists
        above rather than written out a third time, and the test asserts the
        property that matters: nothing which takes the device may be tried
        before something which does not. A faster driver is not worth having
        if a slower one would have done without the cost.
    */
    inline std::vector<Candidate> searchOrder()
    {
        std::vector<Candidate> order;

        for (auto& name : preferredDrivers())
            order.push_back ({ name, false });

        for (auto& name : driversThatTakeTheDevice())
            order.push_back ({ name, true });

        return order;
    }

    /** Whether this driver holds the device, for a caller that has a name and
        needs to know whether to say something about it. */
    inline bool takesTheDevice (const std::string& name)
    {
        for (const auto& taking : driversThatTakeTheDevice())
            if (taking == name)
                return true;

        return false;
    }

    /** How much of the round trip the engine should aim to be responsible for,
        one way, in seconds.

        Two and a half milliseconds, which is 120 samples at 48 kHz. That
        number is chosen against what Windows actually offers rather than
        picked for feel: Microsoft's low latency audio documentation states
        that the inbox HDAudio driver "has been updated to support buffer
        sizes between 128 samples (2.66ms@48kHz) and 480 samples
        (10ms@48kHz)", and the 480 end is what everything gets by default. A
        target of 2.5 ms therefore lands on the first size such a driver
        offers, which is the whole point of asking for Low Latency Mode at
        all. Asking for five milliseconds, as this did at first, would have
        skipped past 128 and 192 to take 256, throwing away most of the gain
        the mode exists to provide.

        Deliberately expressed as a target rather than "the smallest it will
        admit to". On a driver whose minimum is smaller than 128 the floor
        below keeps this off the very bottom, because a buffer at the floor is
        where dropouts live and a studio that crackles until you raise a
        setting is a worse first impression than one a millisecond slower than
        it could be. Someone who wants the floor can still go and take it.
    */
    inline constexpr double targetBufferSeconds = 0.0025;

    /** Picks a buffer size from the ones a driver admits to supporting.

        The smallest that meets the target, or, if the driver will not go that
        low, the largest it offers, since in that case the whole list is below
        the target and the nearest to it is the largest.

        Returns zero only when there is nothing usable to choose from, which
        the caller reads as "leave the buffer size as it is". The list is not
        assumed to be sorted: the order a driver reports its sizes in is its
        own business.

        A zero or negative size can never come back out, even though no size
        is filtered on the way in. Nothing below the target can be selected
        and the target is at least sixty four, and the fallback is a maximum
        taken against zero. That matters because whatever this returns is
        asked of the device as a buffer size.
    */
    inline int chooseBufferSize (const std::vector<int>& available, double sampleRate,
                                 double targetSeconds = targetBufferSeconds)
    {
        if (sampleRate <= 0.0)
            return 0;

        // Never below sixty four whatever the sample rate, since the cost of
        // a block starts to be the block rather than the audio down there.
        const int target = std::max (64, (int) std::lround (sampleRate * targetSeconds));

        int chosen  = 0;
        int largest = 0;

        for (int size : available)
        {
            largest = std::max (largest, size);

            if (size >= target && (chosen == 0 || size < chosen))
                chosen = size;
        }

        return chosen != 0 ? chosen : largest;
    }

    /** Round trip in milliseconds, from what the driver reports. Zero when it
        reports nothing, which the caller has to handle. */
    inline double roundTripMs (int inputLatencySamples, int outputLatencySamples, double sampleRate)
    {
        if (sampleRate <= 0.0)
            return 0.0;

        return 1000.0 * (inputLatencySamples + outputLatencySamples) / sampleRate;
    }

    /** The widest latency figure worth believing from a driver, as a bound
        rather than as a judgement.

        Nothing legitimate comes near it: a minute and a half of latency at
        48 kHz. It is here so that a driver reporting something absurd, or a
        settings file edited by hand, cannot overflow an addition further down
        rather than merely give a wrong answer.
    */
    inline constexpr int maxLatencySamples = 1 << 22;

    /** The round trip in samples: what the driver reported, or two buffers
        when it reported nothing.

        The rule lives here, once, and roundTripOrEstimate below is this
        divided into milliseconds. Record alignment needs the figure in
        samples and exactly, because it is used to count frames of a capture
        rather than to rank one driver against another, and a figure that went
        through milliseconds and back would arrive a sample or two out.

        See roundTripOrEstimate for why a reported zero has to mean "the
        driver did not say" rather than "instant".
    */
    inline int roundTripSamples (int inputLatencySamples, int outputLatencySamples,
                                 int bufferSizeSamples) noexcept
    {
        auto believable = [] (int v) { return std::clamp (v, -maxLatencySamples, maxLatencySamples); };

        const int reported = believable (inputLatencySamples) + believable (outputLatencySamples);

        if (reported > 0)
            return reported;

        return 2 * std::clamp (bufferSizeSamples, 0, maxLatencySamples);
    }

    /** A round trip the search can rank, falling back to the buffer when the
        driver will not say.

        A driver that reports nothing is not a driver with no latency, and the
        difference matters more than it looks. JUCE's ASIO backend zeroes both
        figures when the driver's getLatencies call fails, and plenty of
        drivers never implement it, so a reported zero means "it did not say"
        rather than "instant".

        Taking it at face value is the worst possible reading, because zero is
        also the best possible score: the search would open an interface's own
        driver, be told nothing, decide it had found a perfect device, stop
        looking, and then fail its own sanity check and fall back to the slow
        one it started on. Which is exactly the bug this guards, found by
        auditing the first version of it.

        Two buffers is the floor that any round trip can have, so it ranks
        correctly even where it understates. Callers that show the number to
        somebody say that it is an estimate.
    */
    inline double roundTripOrEstimate (int inputLatencySamples, int outputLatencySamples,
                                       int bufferSizeSamples, double sampleRate)
    {
        if (sampleRate <= 0.0)
            return 0.0;

        return 1000.0 * roundTripSamples (inputLatencySamples, outputLatencySamples,
                                          bufferSizeSamples) / sampleRate;
    }

    /** Whether a round trip is low enough to stop looking for something
        faster.

        A rule of thumb rather than a standard: there is no specification for
        what a musician can play through, and it varies by player and by
        instrument. Fifteen is set where it is from two directions. Below
        roughly ten milliseconds the delay stops being felt at all, so
        anything in that region is unarguably done. The upper end comes from
        what players demonstrably work at: Image-Line's own guidance for FL
        Studio is to run an ASIO buffer of "between 10 and 20 ms (440 to 880
        samples)", which is a round trip of twenty milliseconds and more, and
        people record guitar through that every day.

        Fifteen is therefore comfortably better than the thing this is being
        compared against, while still being a number the search will not
        settle for when something genuinely faster is available. Set it at ten
        and a machine that has reached the best it can do, 128 samples each
        way plus the driver's own, gets reported as a failure.
    */
    inline bool playable (double roundTrip)
    {
        // Greater than zero, not merely at most fifteen. Zero means nothing
        // was measured, and treating "unknown" as the best possible score is
        // how a search stops on the one driver that would not answer it.
        return roundTrip > 0.0 && roundTrip <= 15.0;
    }

    /** Whether it is bad enough to be worth telling somebody about, as
        opposed to merely not the best available. Thirty is past the point
        where a player hears themselves late rather than feeling the note,
        and well past anything the settings above should have produced, so
        reaching it means something is worth saying. */
    inline bool tooSlowToPlay (double roundTrip) { return roundTrip > 30.0; }
}
