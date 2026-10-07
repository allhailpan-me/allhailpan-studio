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

    //==========================================================================
    /** What one mixer insert is fed from, which may be nothing.

        Most inserts carry no live input: they are where an instrument or a
        send lands. Only the ones being recorded through have a socket behind
        them, so "nothing" is the common case and has to be representable
        rather than faked with channel zero.

        This is modelled on how Image-Line document FL Studio's mixer, because
        the behaviour is established and a musician coming from one studio to
        another should not have to learn a second set of rules for the same
        job. Their manual: "Each Mixer track can receive one external stereo
        audio input", from a menu with "an upper stereo list and lower mono
        list". Same here: one input per insert, pairs listed above singles.
    */
    struct Assignment
    {
        Choice choice;
        bool   assigned = false;
    };

    /** Zero means nothing, so an insert with no input stores as zero and a
        settings file full of zeroes means a mixer nobody has wired up yet. */
    inline int assignmentToStored (const Assignment& assignment)
    {
        return assignment.assigned ? toStored (assignment.choice) + 1 : 0;
    }

    inline Assignment assignmentFromStored (int stored)
    {
        if (stored <= 0)
            return {};

        return { fromStored (stored - 1), true };
    }

    /** Gives every input on the device an insert of its own, starting at
        `firstInsert` and working upward.

        Image-Line's manual calls this Auto-map: "This will automatically map
        each input on your audio device to a unique Mixer Track Input,
        starting on the Mixer track where the Auto-map was initiated and
        working to the right." It is the difference between wiring an eight
        input interface in one action and in eight.

        Returns one assignment per insert, unassigned where nothing reached.
        Runs out gracefully at both ends: more inputs than inserts leaves the
        extra inputs unmapped, and more inserts than inputs leaves the extra
        inserts alone rather than wrapping around and stealing an input that
        already has a home.

        Insert 0 is the master and is never mapped, for the same reason it is
        never the monitored insert: it is where everything else is summed, so
        a live input there would run through the whole master chain.
    */
    inline std::vector<Assignment> autoMap (int firstInsert, int numInserts,
                                            int numInputChannels, bool asStereoPairs)
    {
        std::vector<Assignment> mapped;

        if (numInserts <= 0)
            return mapped;

        mapped.resize ((std::size_t) numInserts);

        if (numInputChannels <= 0)
            return mapped;

        const int step  = asStereoPairs ? 2 : 1;
        const int start = firstInsert < 1 ? 1 : firstInsert;

        int insert = start;

        for (int channel = 0; channel + step - 1 < numInputChannels; channel += step)
        {
            if (insert >= numInserts)
                break;                      // out of inserts before out of inputs

            mapped[(std::size_t) insert] = { { channel, asStereoPairs }, true };
            ++insert;
        }

        return mapped;
    }
}
