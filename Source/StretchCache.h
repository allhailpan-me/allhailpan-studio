#pragma once
#include <juce_events/juce_events.h>
#include "SampleData.h"
#include <atomic>
#include <functional>
#include <map>
#include <set>

// Renders time-stretched and pitch-shifted copies of samples in the background
// with the Rubber Band Library (R3 "finer" engine). While a render is running,
// the playlist plays a quick resampled preview instead.
class StretchCache
{
public:
    explicit StretchCache (std::function<void()> onRenderFinished);
    ~StretchCache();

    // True when the clip settings need a rendered copy at all.
    static bool needsRender (double stretch, double semitones)
    {
        return std::abs (stretch - 1.0) > 1e-4 || std::abs (semitones) > 1e-4;
    }

    static juce::String keyFor (const SampleData& source, double stretch, double semitones);

    // Returns the rendered audio if it's ready; otherwise starts rendering and returns nullptr.
    // With requestIfMissing false it only looks, without starting a render.
    std::shared_ptr<SampleData> get (const std::shared_ptr<SampleData>& source, double stretch, double semitones,
                                     bool requestIfMissing = true);
    bool isPending (const SampleData& source, double stretch, double semitones) const;

    bool isBusy() const;

    // Frees renders no longer used by any clip. Only call right after the
    // engine has been given a new arrangement.
    void keepOnly (const std::set<juce::String>& keysInUse);

private:
    static std::shared_ptr<SampleData> render (const SampleData& source, double stretch, double semitones,
                                               const std::atomic<bool>& keepGoing);

    std::function<void()> onFinished;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

    juce::ThreadPool pool { 2 };
    mutable juce::CriticalSection lock;
    std::map<juce::String, std::shared_ptr<SampleData>> ready;
    std::set<juce::String> pending, failed;
};
