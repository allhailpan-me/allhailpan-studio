#include "StretchCache.h"
#include <rubberband/RubberBandStretcher.h>
#include <cmath>

StretchCache::StretchCache (std::function<void()> onRenderFinished)
    : onFinished (std::move (onRenderFinished))
{
}

StretchCache::~StretchCache()
{
    alive->store (false);                  // running renders stop at their next block
    pool.removeAllJobs (true, 20000);
}

juce::String StretchCache::keyFor (const SampleData& source, double stretch, double semitones)
{
    return juce::String::toHexString ((juce::pointer_sized_int) &source)
         + "|" + juce::String (stretch, 5) + "|" + juce::String (semitones, 3);
}

bool StretchCache::isBusy() const
{
    const juce::ScopedLock sl (lock);
    return ! pending.empty();
}

bool StretchCache::isPending (const SampleData& source, double stretch, double semitones) const
{
    const juce::ScopedLock sl (lock);
    return pending.count (keyFor (source, stretch, semitones)) > 0;
}

std::shared_ptr<SampleData> StretchCache::get (const std::shared_ptr<SampleData>& source, double stretch, double semitones,
                                               bool requestIfMissing)
{
    if (source == nullptr || source->audio.getNumSamples() == 0)
        return {};

    const auto key = keyFor (*source, stretch, semitones);
    {
        const juce::ScopedLock sl (lock);
        if (auto it = ready.find (key); it != ready.end())
            return it->second;
        if (pending.count (key) || failed.count (key) || ! requestIfMissing)
            return {};
        pending.insert (key);
    }

    auto flag = alive;
    auto done = onFinished;
    pool.addJob ([this, source, stretch, semitones, key, flag, done]
    {
        auto result = render (*source, stretch, semitones, *flag);
        if (! flag->load())
            return;

        {
            const juce::ScopedLock sl (lock);
            pending.erase (key);
            if (result != nullptr)
                ready[key] = result;
            else
                failed.insert (key);     // don't retry forever
        }

        juce::MessageManager::callAsync ([flag, done]
        {
            if (flag->load() && done)
                done();
        });
    });

    return {};
}

void StretchCache::keepOnly (const std::set<juce::String>& keysInUse)
{
    const juce::ScopedLock sl (lock);
    for (auto it = ready.begin(); it != ready.end();)
    {
        if (keysInUse.count (it->first) == 0)
            it = ready.erase (it);
        else
            ++it;
    }
}

std::shared_ptr<SampleData> StretchCache::render (const SampleData& source, double stretch, double semitones,
                                                  const std::atomic<bool>& keepGoing)
{
    using RB = RubberBand::RubberBandStretcher;

    const int    channels = source.audio.getNumChannels();
    const size_t frames   = (size_t) source.audio.getNumSamples();
    if (channels <= 0 || frames == 0)
        return {};

    RB stretcher ((size_t) std::lround (source.sampleRate), (size_t) channels,
                  RB::OptionProcessOffline | RB::OptionEngineFiner | RB::OptionChannelsTogether,
                  stretch, std::pow (2.0, semitones / 12.0));

    const size_t block = 4096;
    stretcher.setExpectedInputDuration (frames);
    stretcher.setMaxProcessSize (block);

    std::vector<const float*> in ((size_t) channels);
    auto pointInput = [&] (size_t pos)
    {
        for (int c = 0; c < channels; ++c)
            in[(size_t) c] = source.audio.getReadPointer (c, (int) pos);
    };

    // Pass 1: study the whole file
    for (size_t pos = 0; pos < frames; pos += block)
    {
        if (! keepGoing.load()) return {};
        const size_t count = std::min (block, frames - pos);
        pointInput (pos);
        stretcher.study (in.data(), count, pos + count >= frames);
    }

    // Pass 2: process and collect
    std::vector<std::vector<float>> out ((size_t) channels);
    for (auto& o : out)
        o.reserve ((size_t) ((double) frames * stretch) + block);

    std::vector<float>  scratch ((size_t) channels * block);
    std::vector<float*> outPtrs ((size_t) channels);

    auto drain = [&]
    {
        int available = 0;
        while ((available = stretcher.available()) > 0)
        {
            const size_t want = std::min ((size_t) available, block);
            for (int c = 0; c < channels; ++c)
                outPtrs[(size_t) c] = scratch.data() + (size_t) c * block;
            const size_t got = stretcher.retrieve (outPtrs.data(), want);
            for (int c = 0; c < channels; ++c)
                out[(size_t) c].insert (out[(size_t) c].end(), outPtrs[(size_t) c], outPtrs[(size_t) c] + got);
            if (got == 0)
                break;
        }
    };

    for (size_t pos = 0; pos < frames; pos += block)
    {
        if (! keepGoing.load()) return {};
        const size_t count = std::min (block, frames - pos);
        pointInput (pos);
        stretcher.process (in.data(), count, pos + count >= frames);
        drain();
    }

    for (int guard = 0; guard < 100000 && stretcher.available() >= 0; ++guard)
    {
        if (stretcher.available() == 0)
            juce::Thread::sleep (1);
        drain();
    }

    const size_t outFrames = out[0].size();
    if (outFrames == 0)
        return {};

    auto result = std::make_shared<SampleData>();
    result->name       = source.name;
    result->file       = source.file;
    result->sampleRate = source.sampleRate;
    result->audio.setSize (channels, (int) outFrames);
    for (int c = 0; c < channels; ++c)
        result->audio.copyFrom (c, 0, out[(size_t) c].data(), (int) outFrames);
    result->buildPeaks();
    return result;
}
