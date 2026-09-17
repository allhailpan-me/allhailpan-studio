#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include "AudioEngine.h"
#include <map>

// Renders the song on a background thread. Progress appears in the status bar,
// so the window stays usable while it works.
class ExportJob : public juce::Thread
{
public:
    ExportJob (AudioEngine& e, double songEnd, double sampleRate,
               const juce::File& master,
               std::vector<std::pair<int, juce::File>> stems,
               bool exportingStems)
        : juce::Thread ("ALLHAILPAN export"),
          engine (e), end (songEnd), rate (sampleRate),
          masterFile (master), stemFiles (std::move (stems)), withStems (exportingStems)
    {
    }

    ~ExportJob() override { stopThread (4000); }

    std::atomic<double> progress { 0.0 };
    std::atomic<bool>   done { false }, succeeded { false };
    juce::String problem;

    const juce::File& getMasterFile() const noexcept { return masterFile; }
    bool exportedStems() const noexcept { return withStems; }
    int  numStems() const noexcept { return (int) stemFiles.size(); }

    void run() override
    {
        std::map<int, std::unique_ptr<juce::AudioFormatWriter>> writers;

        auto open = [&] (int index, const juce::File& file)
        {
            file.getParentDirectory().createDirectory();
            file.deleteFile();

            auto stream = std::make_unique<juce::FileOutputStream> (file);
            if (! stream->openedOk())
            {
                problem = "Couldn't write " + file.getFullPathName();
                return false;
            }

            juce::WavAudioFormat wav;
            if (auto* writer = wav.createWriterFor (stream.get(), rate, 2, 24, {}, 0))
            {
                stream.release();          // the writer owns the stream now
                writers[index].reset (writer);
                return true;
            }
            problem = "Couldn't write " + file.getFullPathName();
            return false;
        };

        if (! open (-1, masterFile))
        {
            done.store (true);
            return;
        }
        for (const auto& [index, file] : stemFiles)
            if (! open (index, file))
            {
                done.store (true);
                return;
            }

        std::vector<int> stems;
        for (const auto& [index, writer] : writers)
            if (index >= 0)
                stems.push_back (index);

        const bool ok = engine.renderOffline (0.0, end, 4.0, stems,
            [&writers] (int index, const float* const* data, int numSamples)
            {
                if (auto it = writers.find (index); it != writers.end() && it->second != nullptr)
                    it->second->writeFromFloatArrays (data, 2, numSamples);
            },
            [this] (double amount)
            {
                progress.store (amount);
                return ! threadShouldExit();
            });

        writers.clear();                   // closes and finishes every file
        succeeded.store (ok && ! threadShouldExit());
        done.store (true);
    }

private:
    AudioEngine& engine;
    double end, rate;
    juce::File masterFile;
    std::vector<std::pair<int, juce::File>> stemFiles;
    bool withStems;
};
