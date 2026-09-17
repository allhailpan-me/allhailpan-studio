#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <optional>
#include <vector>

// Captures input audio without ever blocking the audio thread.
// The audio thread writes into a ring buffer; a background thread
// moves that audio into growing memory.
class Recorder : private juce::Thread
{
public:
    struct Take
    {
        juce::AudioBuffer<float> audio;
        double startBeat = 0.0;
        int    droppedFrames = 0;
    };

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
        active.store (true);
        startThread();
    }

    std::optional<Take> end()
    {
        if (! active.exchange (false))
            return std::nullopt;

        stopThread (2000);
        drain();

        const juce::ScopedLock sl (dataLock);
        if (takeL.empty())
            return std::nullopt;

        Take take;
        take.audio.setSize (2, (int) takeL.size());
        take.audio.copyFrom (0, 0, takeL.data(), (int) takeL.size());
        take.audio.copyFrom (1, 0, takeR.data(), (int) takeR.size());
        take.startBeat     = std::max (0.0, startBeat.load());
        take.droppedFrames = dropped.load();

        takeL.clear(); takeL.shrink_to_fit();
        takeR.clear(); takeR.shrink_to_fit();
        return take;
    }

    // ---- audio thread ----
    void push (const float* left, const float* right, int numFrames, double beatAtBlockStart) noexcept
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
