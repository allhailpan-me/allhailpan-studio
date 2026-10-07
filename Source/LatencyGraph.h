#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>

//==============================================================================
/** Delay compensation, as arithmetic.

    This used to live inside AudioEngine::updateLatency, which meant it could
    not be tested: it needs JUCE to reach a plugin's reported latency and
    JUCE cannot be compiled in the development sandbox. That was the wrong way
    round, because of everything in this studio, this is the arithmetic least
    able to announce when it is wrong. Nothing errors. Nothing crackles.
    Tracks simply sit a few milliseconds apart by an amount that depends on
    which plugins happen to be loaded, and the person mixing hears a record
    that is slightly smeared and has no reason to suspect the compensation.

    So the whole calculation is here, over plain integers, and the engine's
    job is reduced to reading the plugins, calling this, and pushing the
    answers into the delay lines.

    WHAT IT GUARANTEES

    One sentence: everything arrives at the master the same distance behind
    the playhead. That distance is `masterIn`, and the test asserts it for
    every source and every route, over thousands of generated mixers.

    Each insert gets two numbers:

      inLat   the latest that anything feeding it arrives
      outLat  inLat plus its own effects

    Everything entering an insert is delayed up to that insert's inLat, and
    every insert's output is delayed up to the master's. One pass from insert
    1 upward resolves the whole graph, which works only because a send may
    only ever feed a higher numbered insert. That rule is load bearing: relax
    it and this needs a real cycle check and a topological order instead.

    THE DELIBERATE EXCEPTION

    An insert a player is monitoring through is not held back, and neither are
    its sends. There can be several at once, which is what recording a band
    is: a microphone on each of four inserts means four people who each have
    to be able to play. Compensation exists to line internal paths up with each
    other; on the signal a player is listening to while playing it, it is
    latency they feel in their hands. Put a lookahead limiter on the master
    and the compensation that keeps the tracks together would push the guitar
    back by the limiter's whole lookahead, which makes the instrument
    unplayable.

    Both halves of that matter. Exempting the dry path alone is not enough,
    because a send is a copy of the insert taken after its fader, so it
    carries the player's input too, and compensating the copy puts back
    exactly what was taken off the original: the player then hears themselves
    once immediately and again at send level, which is a slap back that
    changes whenever a plugin is loaded elsewhere. Putting a reverb on a vocal
    while tracking it is the ordinary way to use a send, so this is not an
    exotic case.

    What cannot be removed from here is the send destination's own plugin
    latency and its own hold back, since dropping those would pull everything
    else on that destination out of line. The residue is therefore a wet copy
    that can arrive late, which is what a reverb sounds like in any case,
    rather than a dry copy of the player beating against itself.

    The exemption has a price, and it is worth stating because it is not only
    the player's own signal that is pulled forward. Anything else parked on
    the monitored insert travels early with it, down the dry path and down its
    sends alike. While tracking that is the right trade: the player has to be
    able to play, and an insert being used for monitoring is not usually
    carrying a finished part as well. It does mean a mix should not be judged
    with monitoring left on.
*/
namespace LatencyGraph
{
    /** One instrument channel. */
    struct Channel
    {
        int  latency    = 0;      /**< what the instrument reports, never negative */
        int  insert     = 1;      /**< where its output folds to */
        bool splitBuses = false;  /**< each output bus to its own insert instead */

        /** Per bus: which insert it goes to, or 0 meaning "the channel's own".
            A bus with no channels is skipped, which is what busChannels says. */
        std::vector<int> busInsert;
        std::vector<int> busChannels;
    };

    /** One mixer insert. Index 0 is the master. */
    struct Insert
    {
        int fxLatency = 0;           /**< the sum of its unbypassed effects */
        std::vector<int> sendTo;     /**< per send: destination insert, or 0 for none */

        /** True when a live input is being listened to through this insert,
            which exempts it and its sends from compensation. Several can be
            true at once: recording a band is several microphones into several
            inserts, and every one of those players has to be able to play. */
        bool monitored = false;
    };

    struct Setup
    {
        std::vector<Insert>  inserts;         /**< at least one: the master */
        std::vector<Channel> channels;
    };

    struct Result
    {
        std::vector<int>              insertDelay;   /**< per insert, index 0 unused */
        std::vector<std::vector<int>> sendDelay;     /**< per insert, per send */
        std::vector<int>              channelDelay;  /**< per channel, main output */
        std::vector<std::vector<int>> busDelay;      /**< per channel, per bus */

        /** Per insert: the latest that anything feeding it arrives, which is
            also how far back a playlist clip on that insert has to read. */
        std::vector<int> clipDelay;

        /** Per insert: clipDelay plus its own effects. Part of the result
            rather than a local so that reusing a Result reuses this too, and
            because the test needs it to say when a send leaves its source. */
        std::vector<int> outLat;

        int masterIn      = 0;   /**< where everything lines up */
        int widest        = 0;   /**< the most any delay line has to hold */
        int totalLatency  = 0;   /**< masterIn plus the master's own effects */
    };

    /** Insert indices arrive from saved projects and from atomics, so they are
        checked rather than trusted.

        An instrument may legitimately be routed straight to the master, so 0
        is a real destination here and is kept. */
    inline int clampDestination (int index, int numInserts) noexcept
    {
        if (numInserts <= 0)
            return 0;

        return index < 0 ? 0 : (index > numInserts - 1 ? numInserts - 1 : index);
    }

    /** The same, for the places where 0 is not a destination: an output bus
        uses 0 to mean "wherever the channel goes" rather than as an insert
        number of its own. */
    inline int clampInsert (int index, int numInserts) noexcept
    {
        if (numInserts <= 1)
            return 0;

        return index < 1 ? 1 : (index > numInserts - 1 ? numInserts - 1 : index);
    }

    /** True when this insert may legally be a send destination from `from`.
        Forward only, and never the master, which has no send of its own to
        resolve and is where everything ends up anyway. */
    inline bool isLegalSendTarget (int from, int to, int numInserts) noexcept
    {
        return to > from && to < numInserts;
    }

    /** Fills in `result`, reusing whatever storage it already has.

        That reuse is the point of this overload rather than a tidiness. This
        is called with the engine's graph lock held, and the audio thread only
        ever *tries* that lock: it drops the block when it cannot get it. A
        heap allocation inside the lock is therefore a chance of an audible
        gap every time somebody clicks a bypass button, so the engine keeps
        one Setup and one Result and this writes through them. Everything
        below is assign or resize on vectors that are already the right size,
        which does not allocate.
    */
    inline void compute (const Setup& setup, Result& result)
    {
        const int numInserts  = (int) setup.inserts.size();
        const int numChannels = (int) setup.channels.size();

        result.masterIn = result.widest = result.totalLatency = 0;

        if (numInserts <= 0)
        {
            result.insertDelay.clear();
            result.channelDelay.clear();
            result.clipDelay.clear();
            result.outLat.clear();
            result.sendDelay.clear();
            result.busDelay.clear();
            return;
        }

        // Playlist audio and every instrument share one figure, because any of
        // them can land on any insert and the slowest of them sets the pace.
        int slowestInstrument = 0;

        for (const auto& c : setup.channels)
            slowestInstrument = std::max (slowestInstrument, std::max (0, c.latency));

        // clipDelay *is* inLat: the latest anything feeding an insert
        // arrives is exactly how far back a clip on it must read. Worked on
        // in place rather than copied out at the end.
        auto& inLat  = result.clipDelay;
        auto& outLat = result.outLat;

        inLat.assign ((std::size_t) numInserts, slowestInstrument);
        outLat.assign ((std::size_t) numInserts, 0);

        // Forward pass. A send cannot reach its destination before it has been
        // produced, so each destination waits for the latest of its feeds, and
        // one pass suffices because sends only ever go forwards.
        for (int i = 1; i < numInserts; ++i)
        {
            const auto& insert = setup.inserts[(std::size_t) i];

            outLat[(std::size_t) i] = inLat[(std::size_t) i] + std::max (0, insert.fxLatency);

            for (int to : insert.sendTo)
                if (isLegalSendTarget (i, to, numInserts))
                    inLat[(std::size_t) to] = std::max (inLat[(std::size_t) to],
                                                        outLat[(std::size_t) i]);
        }

        int masterIn = 0;

        for (int i = 1; i < numInserts; ++i)
            masterIn = std::max (masterIn, outLat[(std::size_t) i]);

        // The master's own input is where everything meets, so anything routed
        // straight to it waits for all of that rather than only for the
        // instruments.
        //
        // This is not a detail. An instrument or a playlist clip can be sent
        // to the master directly, and the master has no hold back of its own
        // to catch it later: insert 0 is where the inserts are summed, after
        // each of them has already been lined up. Leaving its input at the
        // slowest instrument, which is what it was, meant a synth routed
        // straight to the master played early against the whole mix by
        // whatever lookahead happened to be loaded on any insert. Nothing
        // about that announces itself, which is the entire reason this file
        // exists.
        //
        // Safe to do after the forward pass: sends can only go forwards from
        // insert 1 upward, so nothing ever feeds insert 0 by a send and
        // nothing above reads inLat[0].
        inLat[0] = std::max (inLat[0], masterIn);

        result.masterIn = masterIn;
        result.widest   = std::max (masterIn, slowestInstrument);


        // Instruments wait for whatever else lands on the same insert. A
        // channel whose buses are split can land on several at once, so each
        // bus waits for its own destination rather than for the channel's.
        result.channelDelay.assign ((std::size_t) numChannels, 0);

        // This one needs assign rather than resize, and it is the only one
        // that does. A Result is reused between calls, and the loop below
        // skips a channel whose buses are not split, so without emptying
        // every row first a channel that has just stopped being split would
        // go on handing back the delays it had when it was. Every other
        // vector here has all of its entries written on every call. Emptying
        // a row keeps its capacity, so this does not allocate after the first
        // call.
        result.busDelay.assign ((std::size_t) numChannels, {});

        for (int c = 0; c < numChannels; ++c)
        {
            const auto& channel = setup.channels[(std::size_t) c];
            const int   own     = std::max (0, channel.latency);
            const int   main    = clampDestination (channel.insert, numInserts);

            result.channelDelay[(std::size_t) c] = std::max (0, inLat[(std::size_t) main] - own);

            if (! channel.splitBuses)
                continue;

            auto& buses = result.busDelay[(std::size_t) c];
            buses.assign (channel.busInsert.size(), 0);

            for (std::size_t b = 0; b < channel.busInsert.size(); ++b)
            {
                const bool used = b < channel.busChannels.size()
                               && channel.busChannels[b] > 0;

                if (! used)
                    continue;

                const int routed = channel.busInsert[b];
                const int to = routed > 0 ? clampInsert (routed, numInserts) : main;

                buses[b] = std::max (0, inLat[(std::size_t) to] - own);
            }
        }

        result.insertDelay.assign ((std::size_t) numInserts, 0);

        // Insert 0 is the master: it has no send of its own to resolve and
        // nothing may send into it, so the loop below starts at 1 and row 0
        // is left empty by this assign.
        result.sendDelay.assign ((std::size_t) numInserts, {});

        for (int i = 1; i < numInserts; ++i)
        {
            const auto& insert = setup.inserts[(std::size_t) i];

            // Insert 0 is never monitored: it is the master, which is what
            // everything else is lined up to, so there is nothing to exempt
            // it from. The loop starting at 1 is what enforces that.
            const bool isMonitored = insert.monitored;

            result.insertDelay[(std::size_t) i]
                = isMonitored ? 0
                              : std::max (0, masterIn - outLat[(std::size_t) i]);

            auto& sends = result.sendDelay[(std::size_t) i];
            sends.assign (insert.sendTo.size(), 0);

            for (std::size_t s = 0; s < insert.sendTo.size(); ++s)
            {
                const int to = insert.sendTo[s];
                const int target = isLegalSendTarget (i, to, numInserts)
                                     ? inLat[(std::size_t) to]
                                     : outLat[(std::size_t) i];

                // Zero for the monitored insert, for the reason set out at
                // the top of this file: a compensated send copy puts back
                // exactly the delay the dry path had removed.
                sends[s] = isMonitored
                             ? 0
                             : std::max (0, target - outLat[(std::size_t) i]);
            }
        }

        result.totalLatency = masterIn + std::max (0, setup.inserts[0].fxLatency);
    }

    /** The same, for a caller that does not have a Result to reuse. Used by
        the test, where allocating is free. */
    inline Result compute (const Setup& setup)
    {
        Result result;
        compute (setup, result);
        return result;
    }
}
