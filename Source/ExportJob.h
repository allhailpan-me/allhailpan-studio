#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include "AudioEngine.h"
#include "Dither.h"
#include <map>

// Renders the song on a background thread. Progress appears in the status bar,
// so the window stays usable while it works.
//
// The depth it writes at and whether the reduction to that depth is dithered
// are both chosen, in Preferences. What those choices mean, why only 16 bit is
// dithered, and why the quantiser is in Dither.h rather than left to JUCE are
// all in that header. The short version of the last one, because it is the
// reason this file writes integers rather than floats: JUCE's
// writeFromFloatArrays narrows with an arithmetic right shift, which is floor
// division rather than rounding, and leaves half an LSB of DC in the file.
class ExportJob : public juce::Thread
{
public:
    ExportJob (AudioEngine& e, double songEnd, double sampleRate,
               const juce::File& master,
               std::vector<std::pair<int, juce::File>> stems,
               bool exportingStems,
               Dither::Depth depth, Dither::Mode dither)
        : juce::Thread ("ALLHAILPAN export"),
          engine (e), end (songEnd), rate (sampleRate),
          masterFile (master), stemFiles (std::move (stems)), withStems (exportingStems),
          bitDepth (depth), ditherMode (Dither::modeFor (depth, dither))
    {
    }

    ~ExportJob() override { stopThread (4000); }

    std::atomic<double> progress { 0.0 };
    std::atomic<bool>   done { false }, succeeded { false };
    juce::String problem;

    const juce::File& getMasterFile() const noexcept { return masterFile; }
    bool exportedStems() const noexcept { return withStems; }
    int  numStems() const noexcept { return (int) stemFiles.size(); }

    Dither::Depth getDepth() const noexcept  { return bitDepth; }

    /** The dither actually used, which is not always the one asked for: the
        policy in Dither.h turns it off above 16 bit. Reported rather than
        recomputed by the caller, so what the status line says is what the
        file got. */
    Dither::Mode  getDither() const noexcept { return ditherMode; }

    void run() override
    {
        // One writer per file, and one quantiser per channel of it. Per
        // channel because two channels sharing a dither stream put identical
        // noise on both, which images it in the centre of the stereo field
        // instead of spreading it, and per file so that a stem is not
        // correlated with the master it is part of.
        struct Target
        {
            std::unique_ptr<juce::AudioFormatWriter> writer;
            std::vector<Dither::Quantiser> quantisers;
        };

        std::map<int, Target> targets;

        // Counted up so every channel of every file gets its own stream, and
        // fixed rather than taken from a clock so that exporting the same mix
        // twice gives the same file. A reproducible export is worth more than
        // an unpredictable one: it is the only way to tell a change in the mix
        // from a change in the noise.
        std::uint64_t nextSeed = 0x51de'ed1cedull;

        auto open = [&] (int index, const juce::File& file)
        {
            file.getParentDirectory().createDirectory();
            file.deleteFile();

            auto fileStream = std::make_unique<juce::FileOutputStream> (file);
            if (! fileStream->openedOk())
            {
                problem = "Couldn't write " + file.getFullPathName();
                return false;
            }

            // Held as the base type, because createWriterFor takes the pointer
            // by reference and so will not convert from a derived one.
            std::unique_ptr<juce::OutputStream> stream = std::move (fileStream);

            juce::WavAudioFormat wav;

            // The overload taking loose arguments is deprecated and will
            // eventually be removed, which would stop exports compiling. This
            // one takes the stream by reference and claims it on success, so
            // there is no raw pointer to hand over.
            //
            // The sample format is passed rather than left automatic. At 32
            // bits JUCE writes floating point unless told integral, so
            // automatic happens to be right today, and saying it out loud
            // means a change to that default cannot silently turn a float
            // export into 32 bit integer, which would be the same numbers
            // quantised with no dither and nothing to say so.
            const auto options = juce::AudioFormatWriterOptions{}
                                     .withSampleRate (rate)
                                     .withNumChannels (2)
                                     .withBitsPerSample (Dither::bitsFor (bitDepth))
                                     .withSampleFormat (Dither::isFixedPoint (bitDepth)
                                                            ? juce::AudioFormatWriterOptions::SampleFormat::integral
                                                            : juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

            if (auto writer = wav.createWriterFor (stream, options))
            {
                Target t;
                t.writer = std::move (writer);

                for (int ch = 0; ch < 2; ++ch)
                    t.quantisers.emplace_back (bitDepth, ditherMode, nextSeed++);

                targets.emplace (index, std::move (t));
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
        for (const auto& [index, target] : targets)
            if (index >= 0)
                stems.push_back (index);

        // Two channels of integer codes, grown to whatever the render hands
        // over rather than guessed at. This is a background thread, so the
        // allocation is only a question of not doing it per block.
        std::vector<int> codes;
        int codeCapacity = 0;

        const bool ok = engine.renderOffline (0.0, end, 4.0, stems,
            [&] (int index, const float* const* data, int numSamples)
            {
                auto it = targets.find (index);
                if (it == targets.end() || it->second.writer == nullptr || numSamples <= 0)
                    return;

                auto& target = it->second;

                if (! Dither::isFixedPoint (bitDepth))
                {
                    // Nothing to quantise, so nothing to dither. JUCE's
                    // writeFromFloatArrays passes the floats straight through
                    // when the writer is a floating point one, with no
                    // conversion at all.
                    target.writer->writeFromFloatArrays (data, 2, numSamples);
                    return;
                }

                if (numSamples > codeCapacity)
                {
                    codeCapacity = numSamples;
                    codes.resize ((size_t) codeCapacity * 2);
                }

                for (int ch = 0; ch < 2; ++ch)
                {
                    int* dest    = codes.data() + (size_t) ch * (size_t) codeCapacity;
                    auto& quant  = target.quantisers[(size_t) ch];
                    const float* src = data[ch];

                    // Left justified on the way out. write (const int**, n)
                    // takes samples spanning the whole 32 bit range and the
                    // WAV writer shifts them back down by exactly this much,
                    // so the code written to the file is the code computed
                    // here, with no floating point in between.
                    for (int i = 0; i < numSamples; ++i)
                        dest[i] = Dither::leftJustified (quant.code (src[i]), bitDepth);
                }

                // Three entries and a null, because write's own documentation
                // says the outer array is zero terminated and JUCE's internal
                // caller does terminate it. The WAV writer happens to take a
                // channel count as well and so would not read past the second,
                // but relying on that would be relying on an implementation
                // detail of a library this project cannot compile to check.
                const int* channels[3] = { codes.data(),
                                           codes.data() + (size_t) codeCapacity,
                                           nullptr };

                target.writer->write (channels, numSamples);
            },
            [this] (double amount)
            {
                progress.store (amount);
                return ! threadShouldExit();
            });

        targets.clear();                   // closes and finishes every file
        succeeded.store (ok && ! threadShouldExit());
        done.store (true);
    }

private:
    AudioEngine& engine;
    double end, rate;
    juce::File masterFile;
    std::vector<std::pair<int, juce::File>> stemFiles;
    bool withStems;
    Dither::Depth bitDepth;
    Dither::Mode  ditherMode;
};
