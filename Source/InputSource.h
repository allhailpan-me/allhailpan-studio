#pragma once
#include <cstddef>
#include <string>
#include <vector>

//==============================================================================
/** Which input the studio listens to, and where it lands in the stereo field.

    This exists because of a bug that made the studio unusable for the single
    most common thing anybody does with it, and which looked like an output
    fault from the outside.

    The engine took input channel 1 as the left of a stereo pair and input
    channel 2 as the right, always. That is right for a stereo keyboard and
    wrong for everything else. Plug a guitar into input 1 of an interface and
    leave input 2 empty, which is what recording a guitar is, and you hear
    yourself hard left, out of one speaker, with silence in the other. Takes
    were recorded the same way: a stereo file, performance on the left, nothing
    on the right.

    The person reporting it went looking in the output channel settings,
    because one speaker is an output symptom, and nothing there helped. It
    could not: the fault was on the way in.

    So an input is now chosen rather than assumed. A mono source is centred,
    which is what a mono source means, and a stereo pair stays a stereo pair.

    Kept free of JUCE so the channel arithmetic can be tested. The things that
    go wrong here are all off by one: reading past the last channel of an
    interface, resolving a mono source to two different channels, or clamping
    in a way that silently moves somebody onto an input they are not plugged
    into.
*/
namespace InputSource
{
    /** What the person picked. `first` is zero based. */
    struct Choice
    {
        int  first  = 0;
        bool stereo = false;
    };

    /** Which two channels to read, after the choice has been checked against
        what the device actually has. */
    struct Resolved
    {
        int  left  = 0;
        int  right = 0;
        bool valid = false;   /**< false when the device has no inputs at all */

        /** True when both sides read the same channel, which is what centres
            a mono source instead of pinning it to one speaker. */
        bool isCentred() const noexcept { return left == right; }
    };

    /** Reads a choice against a real device.

        Never returns an index the device does not have, and never returns two
        different channels for a mono choice. A stereo choice whose second
        channel would be past the end falls back to mono rather than reading
        off the end of the array, which on the audio thread is not a wrong
        number but a crash.
    */
    inline Resolved resolve (const Choice& choice, int numInputChannels) noexcept
    {
        Resolved out;

        if (numInputChannels <= 0)
            return out;                       // valid stays false

        const int last = numInputChannels - 1;

        out.left  = choice.first < 0 ? 0 : (choice.first > last ? last : choice.first);
        out.right = out.left;
        out.valid = true;

        if (choice.stereo && out.left + 1 <= last)
            out.right = out.left + 1;

        return out;
    }

    /** Every input a device offers, mono first and then the stereo pairs.

        Mono comes first because it is what one instrument is, and the one the
        default has to be. The pairs are only the ones that genuinely exist:
        an interface with an odd number of inputs has no pair at the top.
    */
    inline std::vector<Choice> options (int numInputChannels)
    {
        std::vector<Choice> all;

        if (numInputChannels <= 0)
            return all;

        for (int i = 0; i < numInputChannels; ++i)
            all.push_back ({ i, false });

        for (int i = 0; i + 1 < numInputChannels; i += 2)
            all.push_back ({ i, true });

        return all;
    }

    /** What to put in the menu. One based, because the back of an interface
        is numbered from one and nobody counts their inputs from zero. */
    inline std::string describe (const Choice& choice)
    {
        const int first = choice.first < 0 ? 0 : choice.first;

        if (! choice.stereo)
            return "In " + std::to_string (first + 1);

        return "In " + std::to_string (first + 1) + "+" + std::to_string (first + 2);
    }

    /** The default: the first input, mono, centred.

        A single mono source is what almost everybody plugs in first, and
        getting it wrong for them is the difference between the studio working
        and not. Getting it wrong for somebody with a stereo source is one
        dropdown and is obvious the moment they hear it.
    */
    inline Choice defaultChoice() { return { 0, false }; }

    /** Packs a choice into one integer, for a settings file. The low bit is
        the stereo flag so that a stored value stays readable as a number. */
    inline int toStored (const Choice& choice)
    {
        const int first = choice.first < 0 ? 0 : choice.first;
        return (first << 1) | (choice.stereo ? 1 : 0);
    }

    inline Choice fromStored (int stored)
    {
        if (stored < 0)
            return defaultChoice();

        return { stored >> 1, (stored & 1) != 0 };
    }
}
