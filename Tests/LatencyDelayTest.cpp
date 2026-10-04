// ---------------------------------------------------------------------------
// Checks the fixed delay that delay compensation is built out of.
//
// This is the piece every compensated path runs through, so an error here is
// not one broken track, it is every track sitting a few samples away from
// where it should and a mix that sounds slightly smeared for no visible
// reason. It has already had one real bug: a read one sample past the end of
// the ring, which only happened when the write position exactly equalled a
// tap length that had rounded up, and which would have presented as a studio
// that crashed rarely and unreproducibly.
//
// So the delay is checked sample for sample rather than approximately, over
// randomly generated block sizes, because a delay line that is right for
// 512 sample blocks and wrong when the device hands it 480 is the normal way
// this breaks. Sanitizers are what cover the indexing.
//
// LatencyDelay.h wants a few JUCE types, so they are stubbed below. The stub
// is deliberately stricter than JUCE: its accessors bounds check, so an index
// that JUCE would quietly accept is caught here.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

// --- the JUCE surface LatencyDelay.h uses ----------------------------------

namespace juce
{
    template <typename T> T jmax (T a, T b)       { return a > b ? a : b; }
    template <typename T> T jmin (T a, T b)       { return a < b ? a : b; }
    template <typename T> T jmin (T a, T b, T c)  { return jmin (jmin (a, b), c); }

    template <typename T> T jlimit (T lo, T hi, T v)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    /** Enough of juce::AudioBuffer for this header, with bounds checks that
        the real one does not have in a release build. */
    template <typename Sample>
    class AudioBuffer
    {
    public:
        void setSize (int newNumChannels, int newNumSamples, bool keepExisting,
                      bool clearExtraSpace, bool /*avoidReallocating*/)
        {
            std::vector<std::vector<Sample>> grown (
                (size_t) newNumChannels,
                std::vector<Sample> ((size_t) newNumSamples, Sample (0)));

            if (keepExisting)
                for (size_t ch = 0; ch < channels.size() && ch < grown.size(); ++ch)
                    for (size_t i = 0; i < channels[ch].size() && i < grown[ch].size(); ++i)
                        grown[ch][i] = channels[ch][i];

            (void) clearExtraSpace;  // the vectors above are already zeroed
            channels = std::move (grown);
            numSamples = newNumSamples;
        }

        void clear()
        {
            for (auto& ch : channels)
                std::fill (ch.begin(), ch.end(), Sample (0));
        }

        int getNumChannels() const { return (int) channels.size(); }
        int getNumSamples()  const { return numSamples; }

        Sample* getWritePointer (int channel)
        {
            assert (channel >= 0 && channel < (int) channels.size());
            return channels[(size_t) channel].data();
        }

        const Sample* getReadPointer (int channel) const
        {
            assert (channel >= 0 && channel < (int) channels.size());
            return channels[(size_t) channel].data();
        }

    private:
        std::vector<std::vector<Sample>> channels;
        int numSamples = 0;
    };
}

#include "LatencyDelay.h"

// --- harness ---------------------------------------------------------------

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

/** Runs a signal through the delay in the given block sizes and returns what
    came out, so a test can compare the whole stream rather than a few points.

    The input is distinctive per sample rather than an impulse, because an
    impulse only proves where one sample landed, and the bugs this is looking
    for move a run of samples or repeat a stale block. */
static std::vector<float> runThrough (LatencyDelay& delay,
                                      const std::vector<float>& input,
                                      const std::vector<int>& blockSizes,
                                      int channel = 0,
                                      int numChannels = 1)
{
    juce::AudioBuffer<float> buffer;
    const int widest = *std::max_element (blockSizes.begin(), blockSizes.end());
    buffer.setSize (numChannels, widest, false, true, true);

    std::vector<float> output;
    output.reserve (input.size());

    size_t pos = 0;
    size_t which = 0;

    while (pos < input.size())
    {
        const int n = (int) std::min ((size_t) blockSizes[which % blockSizes.size()],
                                      input.size() - pos);
        ++which;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* d = buffer.getWritePointer (ch);

            // Every channel carries a different signal, so a delay line that
            // crossed channels over would show up rather than cancel out.
            for (int i = 0; i < n; ++i)
                d[i] = input[pos + (size_t) i] * (float) (ch + 1);
        }

        delay.process (buffer, numChannels, n);

        const auto* out = buffer.getReadPointer (channel);
        for (int i = 0; i < n; ++i)
            output.push_back (out[i]);

        pos += (size_t) n;
    }

    return output;
}

/** What a delay of exactly d samples should produce: d zeros, then the input,
    truncated to the length actually processed. */
static std::vector<float> expectedDelay (const std::vector<float>& input, int d, float scale)
{
    std::vector<float> want (input.size(), 0.0f);

    for (size_t i = (size_t) d; i < input.size(); ++i)
        want[i] = input[i - (size_t) d] * scale;

    return want;
}

static bool sameStream (const std::vector<float>& got, const std::vector<float>& want,
                        size_t& firstBadIndex)
{
    if (got.size() != want.size())
    {
        firstBadIndex = 0;
        return false;
    }

    for (size_t i = 0; i < got.size(); ++i)
    {
        // Whole sample delay moves values, it does not alter them, so this is
        // an exact comparison on purpose. A tolerance here would hide the
        // interpolation bug it exists to catch.
        if (got[i] != want[i])
        {
            firstBadIndex = i;
            return false;
        }
    }

    return true;
}

static std::vector<float> ramp (size_t n)
{
    std::vector<float> v (n);

    // Values a float holds exactly, and all different, so a sample that came
    // from the wrong place names its own index.
    for (size_t i = 0; i < n; ++i)
        v[i] = (float) (i + 1);

    return v;
}

// --- the checks ------------------------------------------------------------

static void zeroDelayPassesThrough()
{
    std::printf ("a delay of zero changes nothing\n");

    LatencyDelay delay;
    delay.prepare (1, 64, 128);
    delay.setDelay (0);

    const auto in  = ramp (500);
    const auto got = runThrough (delay, in, { 128 });

    size_t bad = 0;
    check (sameStream (got, in, bad), "zero delay altered the signal");
    check (delay.getDelay() == 0, "zero delay did not report itself");
}

static void delayIsExact()
{
    std::printf ("the signal comes out exactly as many samples late as asked\n");

    for (int d : { 1, 2, 7, 63, 64, 65, 127, 128, 129, 511, 512, 1024 })
    {
        LatencyDelay delay;
        delay.prepare (1, 2048, 512);
        delay.setDelay (d);

        if (delay.getDelay() != d)
        {
            fail ("delay of " + std::to_string (d) + " was not accepted");
            continue;
        }

        const auto in  = ramp (4000);
        const auto got = runThrough (delay, in, { 512 });

        size_t bad = 0;
        if (! sameStream (got, expectedDelay (in, d, 1.0f), bad))
            fail ("delay of " + std::to_string (d) + " was wrong from sample "
                  + std::to_string (bad));
    }
}

static void blockSizeDoesNotMatter()
{
    std::printf ("the same delay however the device cuts the blocks\n");

    // A device does not hand over a tidy power of two, and it does not hand
    // over the same size every time. These are the sizes real interfaces use,
    // plus the awkward ones around the ring's own boundaries.
    const std::vector<std::vector<int>> patterns = {
        { 1 }, { 2 }, { 3 }, { 32 }, { 64 }, { 96 }, { 128 }, { 160 }, { 441 },
        { 480 }, { 512 }, { 1 , 512 }, { 512, 1 }, { 7, 13, 1, 480, 3 },
        { 128, 128, 1 }, { 1, 1, 1, 1024 }
    };

    for (const auto& blocks : patterns)
    {
        LatencyDelay delay;
        delay.prepare (1, 1024, 1024);
        delay.setDelay (300);

        const auto in  = ramp (5000);
        const auto got = runThrough (delay, in, blocks);

        size_t bad = 0;
        if (! sameStream (got, expectedDelay (in, 300, 1.0f), bad))
            fail ("block pattern starting " + std::to_string (blocks[0])
                  + " was wrong from sample " + std::to_string (bad));
    }
}

static void channelsStayApart()
{
    std::printf ("each channel is delayed on its own\n");

    LatencyDelay delay;
    delay.prepare (4, 1024, 512);
    delay.setDelay (97);

    const auto in = ramp (3000);

    for (int ch = 0; ch < 4; ++ch)
    {
        LatencyDelay fresh;
        fresh.prepare (4, 1024, 512);
        fresh.setDelay (97);

        const auto got = runThrough (fresh, in, { 512 }, ch, 4);

        size_t bad = 0;
        if (! sameStream (got, expectedDelay (in, 97, (float) (ch + 1)), bad))
            fail ("channel " + std::to_string (ch) + " was wrong from sample "
                  + std::to_string (bad));
    }
}

static void changingTheDelayDoesNotReplayStaleAudio()
{
    std::printf ("changing the delay clears what was in flight\n");

    LatencyDelay delay;
    delay.prepare (1, 1024, 512);
    delay.setDelay (100);

    // Fill the line with something loud and recognisable.
    const auto loud = std::vector<float> (512, 9.0f);
    runThrough (delay, loud, { 512 });

    // A plugin was bypassed, so compensation changes. Whatever is still in the
    // line belongs to the old alignment and must not be heard at the new one.
    delay.setDelay (50);

    const auto silence = std::vector<float> (512, 0.0f);
    const auto got     = runThrough (delay, silence, { 512 });

    bool anyLeak = false;
    for (float s : got)
        if (s != 0.0f)
            anyLeak = true;

    check (! anyLeak, "old audio leaked out after the delay changed");
    check (delay.getDelay() == 50, "the new delay was not reported");
}

static void setDelayReportsWhatItActuallyDid()
{
    std::printf ("a delay larger than the prepared room is reported, not hidden\n");

    // This is the trap in delay compensation. setDelay clamps to the room it
    // has, so asking for more than was prepared yields a line that is quietly
    // short, and every path through it ends up early by the difference. That
    // is precisely the drift that depends on which plugins happen to be
    // loaded. The clamp itself is correct, because the alternative is reading
    // out of bounds. What matters is that getDelay tells the truth afterwards,
    // so updateLatency can notice and prepare more room rather than ship a
    // mix that is subtly out of time.
    LatencyDelay delay;
    delay.prepare (1, 256, 128);
    delay.setDelay (100000);

    check (delay.getDelay() != 100000,
           "a delay beyond the prepared room was accepted, which would read out of bounds");
    check (delay.getDelay() > 0, "the clamp collapsed the delay to nothing");

    // Whatever it settled on, that is what it must actually deliver.
    const int actual = delay.getDelay();
    const auto in    = ramp (2000);
    const auto got   = runThrough (delay, in, { 128 });

    size_t bad = 0;
    check (sameStream (got, expectedDelay (in, actual, 1.0f), bad),
           "the clamped delay did not match what getDelay reported");
}

static void preparingAgainNeverShrinksTheRoom()
{
    std::printf ("preparing again never shrinks the room already granted\n");

    // updateLatency re-prepares every line whenever anything in the signal
    // path changes, sizing it to the worst case latency at that moment. Remove
    // a lookahead limiter and that figure drops, so prepare is routinely
    // called with less room than last time.
    //
    // It must not shrink. prepare does not re-clamp the delay that is already
    // set, so a smaller ring would leave a delay pointing a long way outside
    // it, and the next block would read whatever is there. Holding onto the
    // larger allocation costs a few kilobytes and avoids the whole problem.
    LatencyDelay delay;
    delay.prepare (2, 4096, 512);
    delay.setDelay (4000);
    check (delay.getDelay() == 4000, "a prepared delay of 4000 was not accepted");

    // Plugins removed and the device moved to a small buffer: the smallest
    // room this line will ever be asked for.
    delay.prepare (2, 64, 64);

    check (delay.getDelay() == 4000,
           "re-preparing smaller lost the delay that was already set");

    // Processing now is the dangerous part, and it is what the sanitizers
    // watch. If the ring shrank, this reads far outside it.
    const auto in  = ramp (9000);
    const auto got = runThrough (delay, in, { 64 }, 0, 2);

    size_t bad = 0;
    check (sameStream (got, expectedDelay (in, 4000, 1.0f), bad),
           "the delay was wrong after re-preparing smaller");
}

static void randomConfigurations()
{
    std::printf ("two thousand random configurations\n");

    std::mt19937 rng (20260103);
    int reported = 0;

    for (int trial = 0; trial < 2000; ++trial)
    {
        const int channels  = (int) (rng() % 4) + 1;
        const int maxBlock  = (int) (rng() % 1024) + 1;
        const int maxDelay  = (int) (rng() % 4096);
        const int askDelay  = (int) (rng() % (maxDelay + 1));

        LatencyDelay delay;
        delay.prepare (channels, maxDelay, maxBlock);
        delay.setDelay (askDelay);

        if (delay.getDelay() != askDelay)
        {
            if (++reported < 5)
                fail ("asked for " + std::to_string (askDelay) + " with room for "
                      + std::to_string (maxDelay) + ", got "
                      + std::to_string (delay.getDelay()));
            continue;
        }

        // Block sizes vary within the run, up to the prepared maximum, which
        // is what a device does when it changes buffer size mid session.
        std::vector<int> blocks;
        for (int i = 0; i < 5; ++i)
            blocks.push_back ((int) (rng() % (unsigned) maxBlock) + 1);

        const size_t length = (size_t) askDelay + (size_t) maxBlock * 4 + 101;
        const auto   in     = ramp (length);
        const int    ch     = (int) (rng() % (unsigned) channels);
        const auto   got    = runThrough (delay, in, blocks, ch, channels);

        size_t bad = 0;
        if (! sameStream (got, expectedDelay (in, askDelay, (float) (ch + 1)), bad))
        {
            if (++reported < 5)
                fail ("trial " + std::to_string (trial) + ": delay " + std::to_string (askDelay)
                      + ", channels " + std::to_string (channels)
                      + ", wrong from sample " + std::to_string (bad));
        }
    }

    if (reported >= 5)
        std::printf ("  (%d failures in total, first few shown)\n", reported);
}

int main()
{
    std::printf ("latency delay\n");

    zeroDelayPassesThrough();
    delayIsExact();
    blockSizeDoesNotMatter();
    channelsStayApart();
    changingTheDelayDoesNotReplayStaleAudio();
    setDelayReportsWhatItActuallyDid();
    preparingAgainNeverShrinksTheRoom();
    randomConfigurations();

    if (failures > 0)
    {
        std::printf ("\n%d latency delay check(s) failed\n", failures);
        return 1;
    }

    std::printf ("all latency delay checks passed\n");
    return 0;
}
