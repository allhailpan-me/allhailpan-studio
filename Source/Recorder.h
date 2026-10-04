#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <vector>

// Captures input audio without ever blocking the audio thread.
// The audio thread writes into a ring buffer; a background thread
// moves that audio into growing memory.
//
// Recording round a loop produces several takes rather than one. Rather than
// starting and stopping a recording at every wrap, which would mean a gap at
// each one and a race between the audio thread and whoever restarts it, the
// capture runs straight through and the audio thread simply notes where the
// transport came back round. The split happens at the end, on the message
// thread, where splitting a buffer is free and nothing is waiting on it.
class Recorder : private juce::Thread
{
public:
    struct Take
    {
        juce::AudioBuffer<float> audio;
        double startBeat = 0.0;
        int    droppedFrames = 0;
    };

    /** A point at which the transport jumped back to the start of the loop:
        how far into the capture it happened, and the beat it landed on. */
    struct LoopWrap
    {
        int    frame = 0;
        double beat  = 0.0;
    };

    // Far more passes than anyone comps from, and fixed so that the audio
    // thread never allocates to record one.
    static constexpr int maxPasses = 512;

    Recorder() : juce::Thread ("ALLHAILPAN recorder") {}
    ~Recorder() override { stopThread (2000); }

    // Called when the audio device starts.
    void prepare (double newSampleRate)
    {
        const juce::ScopedLock sl (dataLock);
        sampleRate = newSampleRate;
        const int capacity = (int) (newSampleRate * 4.0);   // four seconds of slack
        fifo.setTotalSize (capacity);
        ring.setSize (2, capacity);
    }

    bool isActive() const noexcept { return active.load(); }

    // ---- message thread ----
    void begin()
    {
        {
            const juce::ScopedLock sl (dataLock);
            takeL.clear();
            takeR.clear();
            takeL.reserve ((size_t) (sampleRate * 120.0));
            takeR.reserve ((size_t) (sampleRate * 120.0));
            fifo.reset();
        }
        dropped.store (0);
        startBeat.store (-1.0);
        framesPushed.store (0);
        wrapCount.store (0);
        active.store (true);
        startThread();
    }

    /** How many times round the loop the capture has been so far, which is
        one less than the number of takes it will produce. */
    int passCount() const noexcept { return wrapCount.load(); }

    /** Every pass, in the order they were played. One take for a recording
        that did not loop, which is exactly what this returned before loop
        recording existed.

        A pass with no audio in it is dropped rather than returned empty: that
        happens when the transport wraps twice inside one block, which a loop
        shorter than the buffer size does.
    */
    std::vector<Take> end()
    {
        std::vector<Take> takes;

        if (! active.exchange (false))
            return takes;

        stopThread (2000);
        drain();

        const juce::ScopedLock sl (dataLock);

        // The two channels are appended together and are always the same
        // length, but the shorter of them is what the slicing below is
        // allowed to read: one of these is indexed with a number the audio
        // thread supplied, and a take with one channel truncated is a
        // disappointment where reading past the end of a buffer is a crash.
        const int total = (int) std::min (takeL.size(), takeR.size());
        if (total == 0)
            return takes;

        // The dropped count belongs to the capture as a whole rather than to
        // any one pass, so it is reported on each of them: the producer needs
        // to know the buffer was too small, and which pass it happened on is
        // not something this can say.
        const int droppedFrames = dropped.load();
        const int wraps = std::min (wrapCount.load(), maxPasses);

        int    from     = 0;
        double fromBeat = std::max (0.0, startBeat.load());

        auto emit = [&] (int first, int last, double beat)
        {
            const int frames = last - first;
            if (frames <= 0)
                return;

            Take take;
            take.audio.setSize (2, frames);
            take.audio.copyFrom (0, 0, takeL.data() + first, frames);
            take.audio.copyFrom (1, 0, takeR.data() + first, frames);
            take.startBeat     = beat;
            take.droppedFrames = droppedFrames;
            takes.push_back (std::move (take));
        };

        for (int i = 0; i < wraps; ++i)
        {
            const int at = std::clamp (wrapFrames[(size_t) i].frame, from, total);
            emit (from, at, fromBeat);
            from     = at;
            fromBeat = wrapFrames[(size_t) i].beat;
        }
        emit (from, total, fromBeat);

        takeL.clear(); takeL.shrink_to_fit();
        takeR.clear(); takeR.shrink_to_fit();
        return takes;
    }

    // ---- audio thread ----
    /** Adds a block to the capture. `wraps` lists the points inside this block
        at which the transport jumped back to the start of the loop, as offsets
        into the block, which is how the capture is later split into passes.
        They are offsets into the block rather than absolute positions because
        only this knows how much of the block the ring buffer actually took. */
    void push (const float* left, const float* right, int numFrames, double beatAtBlockStart,
               const LoopWrap* wraps = nullptr, int numWraps = 0) noexcept
    {
        if (! active.load() || left == nullptr)
            return;

        if (startBeat.load() < 0.0)
            startBeat.store (beatAtBlockStart);

        if (right == nullptr)
            right = left;

        const auto scope = fifo.write (numFrames);
        auto copy = [&] (int ringStart, int count, int srcOffset)
        {
            if (count <= 0) return;
            ring.copyFrom (0, ringStart, left  + srcOffset, count);
            ring.copyFrom (1, ringStart, right + srcOffset, count);
        };
        copy (scope.startIndex1, scope.blockSize1, 0);
        copy (scope.startIndex2, scope.blockSize2, scope.blockSize1);

        const int written = scope.blockSize1 + scope.blockSize2;
        const int base    = framesPushed.load();

        // A wrap is placed against the audio that was actually kept. If the
        // ring overran, the split drifts by however much was dropped, which is
        // the least of the problems a take with dropped samples has, and the
        // producer is told about those separately.
        for (int i = 0; i < numWraps && wraps != nullptr; ++i)
        {
            const int n = wrapCount.load();
            if (n >= maxPasses)
                break;
            wrapFrames[(size_t) n] = { base + std::clamp (wraps[i].frame, 0, written), wraps[i].beat };
            wrapCount.store (n + 1);
        }

        framesPushed.store (base + written);

        if (written < numFrames)
            dropped.fetch_add (numFrames - written);
    }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            drain();
            wait (15);
        }
    }

    void drain()
    {
        const juce::ScopedLock sl (dataLock);
        const auto scope = fifo.read (fifo.getNumReady());

        auto append = [this] (int start, int count)
        {
            if (count <= 0) return;
            const auto* l = ring.getReadPointer (0, start);
            const auto* r = ring.getReadPointer (1, start);
            takeL.insert (takeL.end(), l, l + count);
            takeR.insert (takeR.end(), r, r + count);
        };
        append (scope.startIndex1, scope.blockSize1);
        append (scope.startIndex2, scope.blockSize2);
    }

    juce::AbstractFifo       fifo { 1 };
    juce::AudioBuffer<float> ring;
    juce::CriticalSection    dataLock;
    std::vector<float>       takeL, takeR;
    double                   sampleRate = 44100.0;

    std::atomic<bool>   active    { false };
    std::atomic<double> startBeat { -1.0 };
    std::atomic<int>    dropped   { 0 };

    // Written by the audio thread, read once recording has stopped. Fixed
    // size, because growing it would mean allocating on the audio thread.
    std::array<LoopWrap, (size_t) maxPasses> wrapFrames {};
    std::atomic<int>    wrapCount    { 0 };
    std::atomic<int>    framesPushed { 0 };
};

// Writes a 24-bit WAV file. Kept self-contained on purpose.
inline bool writeWavFile (const juce::File& file, const juce::AudioBuffer<float>& audio, double sampleRate)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();

    juce::FileOutputStream out (file);
    if (out.failedToOpen())
        return false;

    const int channels = audio.getNumChannels();
    const int frames   = audio.getNumSamples();
    const int bytes    = 3;
    const int dataSize = frames * channels * bytes;
    const int rate     = (int) sampleRate;

    out.write ("RIFF", 4);  out.writeInt (36 + dataSize);
    out.write ("WAVE", 4);
    out.write ("fmt ", 4);  out.writeInt (16);
    out.writeShort (1);                                  // PCM
    out.writeShort ((short) channels);
    out.writeInt (rate);
    out.writeInt (rate * channels * bytes);
    out.writeShort ((short) (channels * bytes));
    out.writeShort (24);
    out.write ("data", 4);  out.writeInt (dataSize);

    juce::MemoryBlock block ((size_t) dataSize);
    auto* p = static_cast<juce::uint8*> (block.getData());
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
        {
            const int v = juce::roundToInt (juce::jlimit (-1.0f, 1.0f, audio.getSample (c, i)) * 8388607.0f);
            *p++ = (juce::uint8) (v & 0xff);
            *p++ = (juce::uint8) ((v >> 8) & 0xff);
            *p++ = (juce::uint8) ((v >> 16) & 0xff);
        }

    out.write (block.getData(), block.getSize());
    out.flush();
    return out.getStatus().wasOk();
}
