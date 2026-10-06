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
                                        milliseconds, and the default
      Windows Audio (Low Latency Mode)  shared, but negotiated through
                                        IAudioClient3, single digit
                                        milliseconds on Windows 10 and later
      Windows Audio (Exclusive Mode)    low latency, but takes the device, so
                                        nothing else on the machine can make
                                        a sound

    Low Latency Mode is the one to want: it is fast and it still shares, so a
    video in a browser keeps playing. It has been sitting there unused because
    the studio accepted whatever the system handed it. Exclusive mode is fast
    too and is deliberately never chosen automatically, for the reason set out
    above driversThatTakeTheDevice below.

    The arithmetic is here rather than in the engine so it can be tested. The
    driver choice cannot be: it depends on what the machine has.
*/
namespace AudioDefaults
{
    /** Drivers the studio may move somebody onto without asking, best first.

        ASIO leads where it exists, since a dedicated driver beats anything
        the operating system offers. It is on this list despite usually being
        exclusive to one application, because the public builds are not
        compiled with ASIO at all: having it means somebody installed an
        interface's driver and built the studio against the SDK on purpose,
        which is as clear a statement of intent as a dropdown.

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

    /** Fast, and deliberately not on the list above.

        Exclusive mode bypasses the Windows audio engine and holds the
        endpoint. Microsoft's own description of the two modes is that in
        shared mode "the audio engine mixes the streams from these
        applications", while in exclusive mode "the client has exclusive
        access to the audio hardware", which means nothing else on the machine
        plays through that device for as long as the studio is open.

        That is a reasonable thing to choose and an unreasonable thing to have
        chosen for you. Somebody who asked to hear their guitar and found that
        their browser had gone silent would have no way to connect the two,
        and would report it as a second fault rather than as the price of the
        first one being fixed. So the studio names this where the latency is
        shown and leaves the choice where it belongs.
    */
    inline std::vector<std::string> driversThatTakeTheDevice()
    {
        return { "Windows Audio (Exclusive Mode)" };
    }

    /** How much of the round trip the engine should aim to be responsible for,
        one way, in seconds.

        Five milliseconds each way lands near ten round trip once the driver's
        own buffering either side is counted, which is about where a player
        stops feeling the delay and starts just hearing themselves.

        Deliberately not the smallest the driver will admit to. A buffer at
        the floor is where dropouts live, and a studio that crackles until you
        raise a setting is a worse first impression than one that is four
        milliseconds slower than it could be. Someone who wants the floor can
        still go and take it.
    */
    inline constexpr double targetBufferSeconds = 0.005;

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

    /** Round trip in milliseconds, for telling someone what they have. */
    inline double roundTripMs (int inputLatencySamples, int outputLatencySamples, double sampleRate)
    {
        if (sampleRate <= 0.0)
            return 0.0;

        return 1000.0 * (inputLatencySamples + outputLatencySamples) / sampleRate;
    }

    /** Whether a round trip is low enough to play an instrument through.
        A rule of thumb rather than a standard: there is no specification for
        what a musician can play through and it varies by player. Ten is about
        where it stops being felt. */
    inline bool playable (double roundTrip) { return roundTrip <= 10.0; }

    /** Whether it is bad enough to be worth saying something about. */
    inline bool tooSlowToPlay (double roundTrip) { return roundTrip > 25.0; }
}
