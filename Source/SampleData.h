#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <map>
#include <memory>
#include <vector>

// Audio loaded into memory, plus a small overview used to draw waveforms.
struct SampleData
{
    juce::String name;
    juce::File   file;
    juce::AudioBuffer<float> audio;
    double sampleRate = 44100.0;

    static constexpr int peakBlock = 256;   // one overview point per 256 frames
    std::vector<float> peaks;
    double missingSeconds = -1.0;           // >= 0 for a placeholder

    bool isMissing() const noexcept { return missingSeconds >= 0.0; }

    double durationSeconds() const noexcept
    {
        if (isMissing())
            return missingSeconds;
        return sampleRate > 0.0 ? audio.getNumSamples() / sampleRate : 0.0;
    }

    void buildPeaks()
    {
        const int n  = audio.getNumSamples();
        const int ch = audio.getNumChannels();
        peaks.assign ((size_t) (n / peakBlock + 1), 0.0f);

        for (int c = 0; c < ch; ++c)
        {
            const auto* d = audio.getReadPointer (c);
            for (int i = 0; i < n; ++i)
            {
                auto& p = peaks[(size_t) (i / peakBlock)];
                p = std::max (p, std::abs (d[i]));
            }
        }
    }
};

// Loads audio files once and keeps them alive for the whole session,
// so the audio thread can safely hold plain pointers to them.
class SampleCache
{
public:
    SampleCache()
    {
        formats.registerBasicFormats();
    }

    bool isAudioFile (const juce::File& f) const
    {
        return f.existsAsFile()
            && f.hasFileExtension ("wav;aif;aiff;flac;ogg;mp3");
    }

    std::shared_ptr<SampleData> load (const juce::File& file, juce::String& error)
    {
        const auto key = file.getFullPathName();
        if (auto it = cache.find (key); it != cache.end())
            return it->second;

        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr)
        {
            error = file.getFileName() + " isn't an audio format this app can read yet.";
            return {};
        }

        const auto length = reader->lengthInSamples;
        if (length <= 0 || length > (juce::int64) (reader->sampleRate * 60.0 * 30.0))
        {
            error = file.getFileName() + " is empty or longer than 30 minutes.";
            return {};
        }

        auto data = std::make_shared<SampleData>();
        data->name       = file.getFileNameWithoutExtension();
        data->file       = file;
        data->sampleRate = reader->sampleRate;
        data->audio.setSize (juce::jlimit (1, 2, (int) reader->numChannels), (int) length);
        reader->read (&data->audio, 0, (int) length, 0, true, true);
        data->buildPeaks();

        cache[key] = data;
        return data;
    }

    // Stands in for a file that couldn't be found, so the clip and its path survive.
    std::shared_ptr<SampleData> placeholder (const juce::File& file, const juce::String& name, double seconds)
    {
        auto data = std::make_shared<SampleData>();
        data->name       = "Missing: " + name;
        data->file       = file;
        data->sampleRate = 44100.0;
        data->audio.setSize (1, 0);
        data->missingSeconds = seconds;
        return data;
    }

    // For audio that already lives in memory, such as a fresh recording.
    std::shared_ptr<SampleData> adopt (juce::AudioBuffer<float>&& audio, double sampleRate,
                                       const juce::File& file, const juce::String& name)
    {
        auto data = std::make_shared<SampleData>();
        data->name       = name;
        data->file       = file;
        data->sampleRate = sampleRate;
        data->audio      = std::move (audio);
        data->buildPeaks();

        cache[file.getFullPathName().isNotEmpty() ? file.getFullPathName() : name] = data;
        return data;
    }

    juce::AudioFormatManager formats;

private:
    std::map<juce::String, std::shared_ptr<SampleData>> cache;
};
