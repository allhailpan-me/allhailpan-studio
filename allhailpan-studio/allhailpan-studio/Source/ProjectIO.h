#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Project.h"
#include "AudioEngine.h"

// Saving and loading .ahp project files.
//
// A project file is compressed XML holding the arrangement, tempo, mixer,
// every instrument and effect (with its full settings), and where each
// sample lives, both as an absolute path and relative to the project file.
namespace ProjectIO
{
    inline const juce::String extension { ".ahp" };
    inline const juce::String wildcard  { "*.ahp" };

    // Creates a plugin from a saved <HOSTED> element (description + state).
    // Returns nullptr and fills error if the plugin isn't available here.
    using PluginFactory = std::function<std::unique_ptr<juce::AudioPluginInstance> (const juce::PluginDescription&, juce::String& error)>;

    struct SaveOptions
    {
        bool collectSamples = false;   // copy samples into "<name> Samples" beside the project
        bool isAutosave     = false;   // don't touch sample file paths
    };

    struct LoadReport
    {
        double bpm = 128.0;
        double songStart = 0.0;
        juce::StringArray missingPlugins;
        juce::StringArray missingSamples;
    };

    juce::Result save (const juce::File& file, Project&, AudioEngine&, const SaveOptions&);
    juce::Result load (const juce::File& file, Project&, AudioEngine&, SampleCache&, const PluginFactory&, LoadReport&);

    // Removes every plugin and resets the mixer to defaults.
    void resetEngine (AudioEngine&);

    // Looks for missing samples (by file name) inside a folder and its subfolders.
    // Returns how many were found.
    int relinkMissing (Project&, SampleCache&, const juce::File& folder);

    juce::StringArray missingSampleNames (const Project&);

    juce::File autosaveFile();
}
