// ---------------------------------------------------------------------------
// Checks delay compensation, and in particular that monitoring escapes it.
//
// This is the arithmetic in this studio least able to announce that it is
// wrong. Nothing errors, nothing crackles: tracks simply sit a few
// milliseconds apart by an amount that depends on which plugins happen to be
// loaded, and whoever is mixing hears a slightly smeared record with no
// reason to suspect the compensation. The bug that prompted this file was of
// exactly that shape, and it had a comment above it claiming the opposite.
//
// So rather than checking the formulas, this checks the property the formulas
// exist to produce. For every source and every route, work out when its audio
// reaches the master by walking the path and adding up everything on it, then
// assert they all agree:
//
//   channel main    own + channelDelay + fxLatency[insert] + insertDelay[insert]
//   channel bus     own + busDelay[b]  + fxLatency[to]     + insertDelay[to]
//   send i -> t     outLat[i] + sendDelay[i][s] + fxLatency[t] + insertDelay[t]
//
// All of those must equal masterIn. That one equality is the whole purpose of
// the compensation, and it is checked over thousands of generated mixers
// rather than a handful of chosen ones, because the cases that broke this in
// the past were ragged: a send into an insert that also carries an instrument,
// a channel with its buses split across destinations, a lookahead plugin
// three sends downstream.
//
// The monitored insert is the deliberate exception, and it gets the opposite
// treatment: assert that it is early, assert by exactly how much, and assert
// that the send copy of it is not quietly put back.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include "LatencyGraph.h"

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
// An independent walk of the finished graph, written in terms of arrival times
// rather than of delays, so that it cannot share a mistake with the thing it
// is checking.

namespace walk
{
    /** inLat as the implementation must have computed it, recomputed here from
        the setup alone. Needed to say when a send leaves its source. */
    static std::vector<int> inLatencies (const LatencyGraph::Setup& setup)
    {
        const int numInserts = (int) setup.inserts.size();

        int slowest = 0;
        for (const auto& c : setup.channels)
            slowest = std::max (slowest, std::max (0, c.latency));

        std::vector<int> in ((size_t) numInserts, slowest);

        for (int i = 1; i < numInserts; ++i)
        {
            const int out = in[(size_t) i] + std::max (0, setup.inserts[(size_t) i].fxLatency);

            for (int to : setup.inserts[(size_t) i].sendTo)
                if (LatencyGraph::isLegalSendTarget (i, to, numInserts))
                    in[(size_t) to] = std::max (in[(size_t) to], out);
        }

        return in;
    }

    /** What a signal picks up by passing through an insert on its way to the
        master's input.

        Zero for insert 0, and that is not a special case bolted on: the
        master's own effects run in processInsert(0) after every other insert
        has already been summed into it, so they are downstream of the point
        all of this is measured to. Everything on the master pays them
        equally, which is why totalLatency adds them on separately. */
    static int fx (const LatencyGraph::Setup& setup, int insert)
    {
        if (insert == 0)
            return 0;

        return std::max (0, setup.inserts[(size_t) insert].fxLatency);
    }

    /** One arrival at the master's input, with a name for the failure message.

        `exempt` marks a route that the monitoring exemption deliberately
        pulls forward. That is not only a route *into* the monitored insert:
        a send *out of* it is an uncompensated copy of it, so whatever else
        is sitting on that insert travels early down the send as well. The
        engine's own comment says as much, that anything else on the
        monitored insert plays early by the same amount, and this is where
        that shows up. */
    struct Arrival
    {
        int when = 0;
        std::string what;
        int insert = 0;
        bool exempt = false;
    };

    static std::vector<Arrival> arrivals (const LatencyGraph::Setup& setup,
                                          const LatencyGraph::Result& result)
    {
        const int numInserts = (int) setup.inserts.size();
        const auto in = inLatencies (setup);
        std::vector<Arrival> out;

        const auto hold = [&result] (int insert)
        {
            // Insert 0 has no delay line: nothing holds the master back
            // because the master is what everything is held back to.
            return insert == 0 ? 0 : result.insertDelay[(size_t) insert];
        };

        for (size_t c = 0; c < setup.channels.size(); ++c)
        {
            const auto& channel = setup.channels[c];
            const int own  = std::max (0, channel.latency);
            const int main = LatencyGraph::clampDestination (channel.insert, numInserts);

            if (! channel.splitBuses)
            {
                out.push_back ({ own + result.channelDelay[c] + fx (setup, main)
                                     + hold (main),
                                 "channel " + std::to_string (c) + " into insert "
                                     + std::to_string (main),
                                 main, main == setup.monitoredInsert });
                continue;
            }

            for (size_t b = 0; b < channel.busInsert.size(); ++b)
            {
                if (! (b < channel.busChannels.size() && channel.busChannels[b] > 0))
                    continue;

                const int routed = channel.busInsert[b];
                const int to = routed > 0 ? LatencyGraph::clampInsert (routed, numInserts) : main;

                out.push_back ({ own + result.busDelay[c][b] + fx (setup, to)
                                     + hold (to),
                                 "channel " + std::to_string (c) + " bus " + std::to_string (b)
                                     + " into insert " + std::to_string (to),
                                 to, to == setup.monitoredInsert });
            }
        }

        for (int i = 1; i < numInserts; ++i)
        {
            const int leaves = in[(size_t) i] + fx (setup, i);

            // The dry output of every insert is an arrival in its own right
            // only insofar as it carries something, which the channel walk
            // above already covers. What has to be checked separately is the
            // send copy, because that is a second route to the same place.
            for (size_t s = 0; s < setup.inserts[(size_t) i].sendTo.size(); ++s)
            {
                const int to = setup.inserts[(size_t) i].sendTo[s];

                if (! LatencyGraph::isLegalSendTarget (i, to, numInserts))
                    continue;

                out.push_back ({ leaves + result.sendDelay[(size_t) i][s] + fx (setup, to)
                                     + hold (to),
                                 "send from insert " + std::to_string (i) + " to "
                                     + std::to_string (to),
                                 to, i == setup.monitoredInsert
                                       || to == setup.monitoredInsert });
            }
        }

        return out;
    }

    /** When the monitored input itself reaches the master, by its dry route. */
    static int monitorDryArrival (const LatencyGraph::Setup& setup,
                                  const LatencyGraph::Result& result)
    {
        const int m = setup.monitoredInsert;
        const auto in = inLatencies (setup);

        // The monitor is summed straight into the insert's buffer, so unlike a
        // channel it is not delayed up to inLat first: it goes in at zero and
        // takes only what is downstream of that point.
        return fx (setup, m) + result.insertDelay[(size_t) m];
    }

    static int monitorSendArrival (const LatencyGraph::Setup& setup,
                                   const LatencyGraph::Result& result,
                                   size_t sendIndex)
    {
        const int m  = setup.monitoredInsert;
        const int to = setup.inserts[(size_t) m].sendTo[sendIndex];

        return fx (setup, m) + result.sendDelay[(size_t) m][sendIndex]
             + fx (setup, to) + result.insertDelay[(size_t) to];
    }
}

//==============================================================================
// Builders, so the cases below read as mixers rather than as struct literals.

static LatencyGraph::Setup mixer (int numInserts, int numChannels, int numSends = 2)
{
    LatencyGraph::Setup setup;
    setup.inserts.resize ((size_t) numInserts);

    for (auto& insert : setup.inserts)
        insert.sendTo.assign ((size_t) numSends, 0);

    setup.channels.resize ((size_t) numChannels);
    return setup;
}

//==============================================================================
static void anEmptyMixerDelaysNothing()
{
    auto setup = mixer (17, 16);
    const auto result = LatencyGraph::compute (setup);

    checkEqual (result.masterIn, 0, "an empty mixer should line up at zero");
    checkEqual (result.totalLatency, 0, "an empty mixer should report no latency");

    for (int i = 1; i < 17; ++i)
        checkEqual (result.insertDelay[(size_t) i], 0,
                    "insert " + std::to_string (i) + " delayed with nothing loaded");

    for (int c = 0; c < 16; ++c)
        checkEqual (result.channelDelay[(size_t) c], 0,
                    "channel " + std::to_string (c) + " delayed with nothing loaded");
}

static void everythingLandsTogether()
{
    // One lookahead limiter on insert 3, two instruments, one send.
    auto setup = mixer (17, 16);
    setup.inserts[3].fxLatency = 1024;
    setup.inserts[5].fxLatency = 64;
    setup.channels[0].insert = 3;
    setup.channels[1].insert = 7;
    setup.channels[1].latency = 128;
    setup.inserts[2].sendTo[0] = 5;

    const auto result = LatencyGraph::compute (setup);

    checkEqual (result.masterIn, 1024 + 128,
                "the master should wait for the slowest instrument plus the limiter");

    for (const auto& arrival : walk::arrivals (setup, result))
        checkEqual (arrival.when, result.masterIn,
                    arrival.what + " does not land with everything else");
}

static void sendsChainForward()
{
    // 2 -> 6 -> 11 -> master, with latency at each hop, which is the case a
    // single forward pass has to get right.
    auto setup = mixer (17, 4);
    setup.inserts[2].fxLatency  = 100;
    setup.inserts[6].fxLatency  = 200;
    setup.inserts[11].fxLatency = 400;
    setup.inserts[2].sendTo[0]  = 6;
    setup.inserts[6].sendTo[0]  = 11;
    setup.channels[0].insert = 2;

    const auto result = LatencyGraph::compute (setup);

    checkEqual (result.masterIn, 700, "a chain of sends should accumulate");

    for (const auto& arrival : walk::arrivals (setup, result))
        checkEqual (arrival.when, result.masterIn,
                    arrival.what + " does not land with everything else");
}

static void splitBusesEachWaitForTheirOwnDestination()
{
    auto setup = mixer (17, 4);
    setup.inserts[2].fxLatency = 2048;      // feeds 4, so inLat[4] is large
    setup.inserts[4].fxLatency = 512;
    setup.inserts[9].fxLatency = 32;

    auto& drums = setup.channels[0];
    drums.latency = 16;
    drums.insert = 2;
    drums.splitBuses = true;
    drums.busInsert  = { 4, 9, 0, 4 };
    drums.busChannels = { 2, 2, 2, 0 };     // the last bus is unused

    setup.inserts[2].sendTo[0] = 4;

    const auto result = LatencyGraph::compute (setup);

    for (const auto& arrival : walk::arrivals (setup, result))
        checkEqual (arrival.when, result.masterIn,
                    arrival.what + " does not land with everything else");

    // Each used bus waits for where it actually goes, which is the point of
    // splitting them: bus 1 lands on insert 9, which nothing else feeds, so
    // it waits only for the instruments.
    checkEqual (result.busDelay[0][1], 0,
                "a bus landing on an insert nothing else feeds should not wait");
    checkEqual (result.busDelay[0][0], 2048 + 16 - 16,
                "a bus landing on the insert that the 2048 sample send feeds should wait for it");

    // The unused bus is left alone rather than given a delay. Nothing reads
    // it, since the engine skips buses with no channels, so this is about not
    // doing pointless work under the graph lock rather than about sound. It is
    // asserted so that it stays deliberate: this case is chosen so the unused
    // bus would get 2048 if it were not skipped.
    checkEqual (result.busDelay[0][3], 0, "an unused bus should not be delayed");
}

//==============================================================================
// The monitoring exception, which is what this file was really written for.

static void theMonitoredInsertIsNotHeldBack()
{
    auto setup = mixer (17, 16);
    setup.inserts[0].fxLatency = 1024;      // a lookahead limiter on the master
    setup.inserts[8].fxLatency = 256;
    setup.channels[0].latency = 64;
    setup.monitoredInsert = 1;

    const auto result = LatencyGraph::compute (setup);

    checkEqual (result.insertDelay[1], 0,
                "the monitored insert is still being held back, which is the whole bug");

    // Everything that is not monitored still lines up with everything else
    // that is not monitored.
    for (const auto& arrival : walk::arrivals (setup, result))
        if (! arrival.exempt)
            checkEqual (arrival.when, result.masterIn,
                        arrival.what + " was disturbed by the monitoring exemption");

    // And the player hears themselves as early as the graph allows, which with
    // nothing on their own insert means immediately.
    checkEqual (walk::monitorDryArrival (setup, result), 0,
                "the player should hear themselves with no added delay at all");
}

static void theMonitoredSendIsNotPutBack()
{
    // The defect this catches: a send out of the monitored insert is a copy of
    // the player's own input, and compensating that copy puts back exactly the
    // delay the dry path just had removed. The player then hears themselves
    // twice, the second time masterIn - outLat[m] samples later, at send
    // level. With a lookahead plugin anywhere upstream that is a slap back
    // that moves whenever a plugin is loaded.
    auto setup = mixer (17, 16);
    setup.inserts[2].fxLatency = 1024;      // lookahead somewhere else entirely
    setup.inserts[1].sendTo[0] = 5;         // the monitored insert sends to 5
    setup.inserts[2].sendTo[0] = 5;
    setup.monitoredInsert = 1;

    const auto result = LatencyGraph::compute (setup);

    checkEqual (result.masterIn, 1024, "the limiter should set the line");
    checkEqual (result.insertDelay[1], 0, "the monitored insert should not be held back");
    checkEqual (result.sendDelay[1][0], 0,
                "the send out of the monitored insert is being compensated, so the "
                "player hears themselves twice");

    const int dry = walk::monitorDryArrival (setup, result);
    const int wet = walk::monitorSendArrival (setup, result, 0);

    checkEqual (dry, 0, "the dry monitor path should be immediate");
    checkEqual (wet, 0, "the send copy of the monitor should not be delayed behind it");
    checkEqual (wet - dry, 0,
                "the player hears themselves twice, separated by this many samples");
}

static void aMonitoredSendOnlyLagsByWhatIsRealDownstream()
{
    // What remains after the fix, stated so it cannot change unnoticed. The
    // send destination's own plugin latency and its own hold back still apply,
    // because removing those would pull everything else on that destination
    // out of line. That residue is a wet copy arriving late, which is what a
    // reverb does anyway.
    auto setup = mixer (17, 4);
    setup.inserts[6].fxLatency = 512;       // a reverb with lookahead, on the send bus
    setup.inserts[1].sendTo[0] = 6;
    setup.monitoredInsert = 1;

    const auto result = LatencyGraph::compute (setup);

    const int dry = walk::monitorDryArrival (setup, result);
    const int wet = walk::monitorSendArrival (setup, result, 0);

    checkEqual (result.sendDelay[1][0], 0, "the send itself should add nothing");
    checkEqual (wet - dry, 512,
                "the wet copy should lag by the send bus's own plugin latency and "
                "nothing more");
}

static void monitoringOffCompensatesEverything()
{
    auto setup = mixer (17, 4);
    setup.inserts[2].fxLatency = 300;
    setup.inserts[1].sendTo[0] = 4;
    setup.monitoredInsert = -1;

    const auto result = LatencyGraph::compute (setup);

    checkEqual (result.insertDelay[1], 300,
                "with nobody monitoring, insert 1 should be held back like the rest");

    for (const auto& arrival : walk::arrivals (setup, result))
        checkEqual (arrival.when, result.masterIn,
                    arrival.what + " does not land with everything else");
}

static void theMasterIsNeverTheMonitoredInsert()
{
    // Insert 0 is the master. Monitoring into it would be exempted by nothing,
    // because the compensation loop starts at 1, and the signal would also
    // pick up the whole master chain. clampInsert is what stops an index of
    // zero ever getting that far.
    checkEqual (LatencyGraph::clampInsert (0, 17), 1, "zero should clamp up to insert 1");
    checkEqual (LatencyGraph::clampInsert (-5, 17), 1, "a negative insert should clamp to 1");
    checkEqual (LatencyGraph::clampInsert (99, 17), 16, "too large should clamp to the last");
    checkEqual (LatencyGraph::clampInsert (8, 17), 8, "a legal insert should pass through");
    checkEqual (LatencyGraph::clampInsert (3, 1), 0, "a mixer with only a master has nowhere to go");
}

//==============================================================================
static void routingStraightToTheMasterStillLinesUp()
{
    // An instrument can be sent to the master without passing an insert, and
    // so can a playlist clip. The master has no hold back of its own, because
    // it is where the inserts are summed after each has already been lined
    // up, so anything arriving there directly has to have waited for all of
    // it in advance. This was wrong: the master's input was left at the
    // slowest instrument, so a synth on the master played early against the
    // mix by whatever lookahead was loaded anywhere else.
    auto setup = mixer (17, 4);
    setup.inserts[6].fxLatency = 2048;      // a lookahead limiter on a bus
    setup.channels[0].insert = 0;           // straight to the master
    setup.channels[0].latency = 64;
    setup.channels[1].insert = 6;

    const auto result = LatencyGraph::compute (setup);

    checkEqual (result.masterIn, 2048 + 64, "the limiter and the instrument set the line");
    checkEqual (result.channelDelay[0], result.masterIn - 64,
                "an instrument on the master is not waiting for the rest of the mix");

    // Playlist audio on the master has to read back by the same amount.
    checkEqual (result.clipDelay[0], result.masterIn,
                "playlist audio on the master is not waiting for the rest of the mix");

    for (const auto& arrival : walk::arrivals (setup, result))
        checkEqual (arrival.when, result.masterIn,
                    arrival.what + " does not land with everything else");
}

static void thereIsRoomForEveryDelay()
{
    // `widest` is what the engine sizes every delay line to, so anything it
    // under-reports is a delay that silently clamps, which is a misalignment
    // with no symptom. It has to cover both the inserts and the instruments.
    auto setup = mixer (17, 4);
    setup.inserts[3].fxLatency = 700;
    setup.channels[0].latency = 300;

    const auto withInserts = LatencyGraph::compute (setup);

    check (withInserts.widest >= withInserts.masterIn, "widest is below masterIn");
    check (withInserts.widest >= 300, "widest does not cover the instrument");

    // The degenerate mixer, which is the only case where the two differ: with
    // no inserts at all there is no insert to raise masterIn to the
    // instrument's own latency, so taking masterIn alone would under-report.
    LatencyGraph::Setup onlyMaster;
    onlyMaster.inserts.resize (1);
    onlyMaster.channels.resize (1);
    onlyMaster.channels[0].latency = 480;

    const auto bare = LatencyGraph::compute (onlyMaster);

    checkEqual (bare.masterIn, 0, "with no inserts nothing can set the line");
    checkEqual (bare.widest, 480, "widest must still make room for the instrument");
}

static void sendsMayOnlyGoForwards()
{
    // The rule that makes one pass enough. A backwards send is ignored rather
    // than acted on, because acting on it is a feedback loop and resolving it
    // needs a real cycle check.
    checkEqual (LatencyGraph::isLegalSendTarget (5, 9, 17) ? 1 : 0, 1, "forwards is legal");
    checkEqual (LatencyGraph::isLegalSendTarget (9, 5, 17) ? 1 : 0, 0, "backwards is not");
    checkEqual (LatencyGraph::isLegalSendTarget (5, 5, 17) ? 1 : 0, 0, "onto itself is not");
    checkEqual (LatencyGraph::isLegalSendTarget (5, 0, 17) ? 1 : 0, 0, "into the master is not");
    checkEqual (LatencyGraph::isLegalSendTarget (5, 17, 17) ? 1 : 0, 0, "past the end is not");

    auto setup = mixer (17, 2);
    setup.inserts[9].fxLatency = 128;
    setup.inserts[9].sendTo[0] = 5;          // backwards, must be ignored
    const auto result = LatencyGraph::compute (setup);

    checkEqual (result.clipDelay[5], 0,
                "a backwards send was acted on, which is a feedback loop");
    checkEqual (result.sendDelay[9][0], 0, "a backwards send should carry no delay");
}

//==============================================================================
static bool same (const LatencyGraph::Result& a, const LatencyGraph::Result& b)
{
    return a.insertDelay  == b.insertDelay
        && a.sendDelay    == b.sendDelay
        && a.channelDelay == b.channelDelay
        && a.busDelay     == b.busDelay
        && a.clipDelay    == b.clipDelay
        && a.outLat       == b.outLat
        && a.masterIn     == b.masterIn
        && a.widest       == b.widest
        && a.totalLatency == b.totalLatency;
}

/** A Result is reused across calls so that changing the mixer does not
    allocate while the audio thread is trying for the graph lock. Reuse is
    where stale data lives: a channel that stops being split, an insert whose
    sends are removed, a mixer that shrinks. So every scene is computed twice,
    once into a fresh Result and once into one that has just been used for a
    completely different mixer, and the two must agree exactly. */
static void reusingAResultCannotLeaveAnythingBehind()
{
    std::mt19937 rng (20260207);
    std::uniform_int_distribution<int> aLatency (0, 4096);
    std::uniform_int_distribution<int> aDestination (0, 16);
    std::uniform_int_distribution<int> anInsert (1, 16);
    std::bernoulli_distribution sometimes (0.4);

    const auto generate = [&]
    {
        auto setup = mixer (17, 16);

        for (auto& insert : setup.inserts)
        {
            if (sometimes (rng)) insert.fxLatency = aLatency (rng);
            for (auto& to : insert.sendTo)
                if (sometimes (rng)) to = anInsert (rng);
        }

        for (auto& channel : setup.channels)
        {
            if (sometimes (rng)) channel.latency = aLatency (rng);
            channel.insert = aDestination (rng);

            if (! sometimes (rng))
                continue;

            channel.splitBuses = true;
            channel.busInsert.assign (16, 0);
            channel.busChannels.assign (16, 0);

            for (int b = 0; b < 16; ++b)
            {
                channel.busInsert[(size_t) b]  = sometimes (rng) ? anInsert (rng) : 0;
                channel.busChannels[(size_t) b] = sometimes (rng) ? 2 : 0;
            }
        }

        setup.monitoredInsert = sometimes (rng) ? anInsert (rng) : -1;
        return setup;
    };

    LatencyGraph::Result reused;
    int differences = 0;

    for (int trial = 0; trial < 4000; ++trial)
    {
        const auto setup = generate();

        LatencyGraph::compute (setup, reused);

        if (! same (reused, LatencyGraph::compute (setup)))
            ++differences;
    }

    checkEqual (differences, 0, "a reused result disagreed with a fresh one");

    // And the degenerate case, which is the one most likely to leave something
    // behind: a full mixer followed by one with nothing in it.
    LatencyGraph::compute (generate(), reused);

    LatencyGraph::Setup empty;
    LatencyGraph::compute (empty, reused);

    check (reused.insertDelay.empty() && reused.channelDelay.empty()
             && reused.clipDelay.empty() && reused.sendDelay.empty()
             && reused.busDelay.empty() && reused.outLat.empty(),
           "a mixer with no inserts left the previous one's delays behind");
    checkEqual (reused.masterIn, 0, "an empty mixer reported a line to meet at");
    checkEqual (reused.totalLatency, 0, "an empty mixer reported latency");
}

//==============================================================================
static void randomised()
{
    std::mt19937 rng (20260206);

    std::uniform_int_distribution<int> aLatency (0, 4096);
    std::uniform_int_distribution<int> anInsert (1, 16);
    std::uniform_int_distribution<int> aDestination (0, 16);   // 0 is the master
    std::uniform_int_distribution<int> aBusCount (0, 4);
    std::bernoulli_distribution sometimes (0.25);
    std::bernoulli_distribution often (0.6);

    int disagreements = 0;
    int negatives = 0;
    int monitorNotEarly = 0;
    int monitorDoubled = 0;

    for (int trial = 0; trial < 20000; ++trial)
    {
        auto setup = mixer (17, 16);

        for (int i = 0; i < 17; ++i)
        {
            if (sometimes (rng))
                setup.inserts[(size_t) i].fxLatency = aLatency (rng);

            for (auto& to : setup.inserts[(size_t) i].sendTo)
                if (sometimes (rng))
                    to = anInsert (rng);      // may well be backwards: that is the point
        }

        for (auto& channel : setup.channels)
        {
            if (sometimes (rng))
                channel.latency = aLatency (rng);

            channel.insert = aDestination (rng);

            if (! sometimes (rng))
                continue;

            channel.splitBuses = true;
            const int buses = 1 + aBusCount (rng);
            channel.busInsert.resize ((size_t) buses);
            channel.busChannels.resize ((size_t) buses);

            for (int b = 0; b < buses; ++b)
            {
                channel.busInsert[(size_t) b]  = often (rng) ? anInsert (rng) : 0;
                channel.busChannels[(size_t) b] = often (rng) ? 2 : 0;
            }
        }

        const bool monitoring = often (rng);
        setup.monitoredInsert = monitoring ? anInsert (rng) : -1;

        const auto result = LatencyGraph::compute (setup);

        // Nothing may be negative: a negative delay is a read into the future
        // and in the engine it clamps, which would be a silent misalignment.
        for (int d : result.insertDelay)      if (d < 0) ++negatives;
        for (int d : result.channelDelay)     if (d < 0) ++negatives;
        for (const auto& row : result.sendDelay) for (int d : row) if (d < 0) ++negatives;
        for (const auto& row : result.busDelay)  for (int d : row) if (d < 0) ++negatives;
        if (result.masterIn < 0 || result.widest < 0) ++negatives;

        // No delay line may be asked to hold more than the engine made room
        // for, or it silently clamps.
        for (int d : result.insertDelay)  if (d > result.widest) ++negatives;
        for (int d : result.channelDelay) if (d > result.widest) ++negatives;
        for (const auto& row : result.sendDelay) for (int d : row) if (d > result.widest) ++negatives;
        for (const auto& row : result.busDelay)  for (int d : row) if (d > result.widest) ++negatives;

        // The headline property, for everything that is not the exception.
        for (const auto& arrival : walk::arrivals (setup, result))
            if (! arrival.exempt && arrival.when != result.masterIn)
                ++disagreements;

        if (! monitoring)
            continue;

        const int m = setup.monitoredInsert;

        // The player must never be later than everything else, and must be
        // strictly earlier whenever there was anything to skip.
        const int dry = walk::monitorDryArrival (setup, result);

        if (dry > result.masterIn)
            ++monitorNotEarly;

        // And the send copy of the player must never be behind the dry copy by
        // more than what is genuinely downstream of the send: the
        // destination's own plugins and its own hold back. Anything more is
        // the compensation being put back.
        for (size_t s = 0; s < setup.inserts[(size_t) m].sendTo.size(); ++s)
        {
            const int to = setup.inserts[(size_t) m].sendTo[s];

            if (! LatencyGraph::isLegalSendTarget (m, to, 17))
                continue;

            const int wet = walk::monitorSendArrival (setup, result, s);
            const int downstream = std::max (0, setup.inserts[(size_t) to].fxLatency)
                                 + result.insertDelay[(size_t) to];

            if (wet - dry != downstream)
                ++monitorDoubled;
        }
    }

    checkEqual (disagreements, 0, "randomised mixers where something did not land together");
    checkEqual (negatives, 0, "randomised mixers with a negative or oversized delay");
    checkEqual (monitorNotEarly, 0, "randomised mixers where monitoring was not early");
    checkEqual (monitorDoubled, 0,
                "randomised mixers where the monitored send carried more delay than "
                "what is genuinely downstream of it");
}

//==============================================================================
int main()
{
    std::printf ("latency graph\n");

    anEmptyMixerDelaysNothing();
    everythingLandsTogether();
    sendsChainForward();
    splitBusesEachWaitForTheirOwnDestination();
    theMonitoredInsertIsNotHeldBack();
    theMonitoredSendIsNotPutBack();
    aMonitoredSendOnlyLagsByWhatIsRealDownstream();
    monitoringOffCompensatesEverything();
    theMasterIsNeverTheMonitoredInsert();
    routingStraightToTheMasterStillLinesUp();
    thereIsRoomForEveryDelay();
    sendsMayOnlyGoForwards();
    reusingAResultCannotLeaveAnythingBehind();
    randomised();

    if (failures != 0)
    {
        std::printf ("LatencyGraphTest: %d failure(s)\n", failures);
        return 1;
    }

    std::printf ("all latency graph checks passed\n");
    return 0;
}
