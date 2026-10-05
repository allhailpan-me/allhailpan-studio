#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

// Knows which plugin formats this platform supports (VST3 everywhere,
// AU on macOS, LV2 on Linux), remembers scanned plugins between sessions,
// and stores app settings.
class PluginManager : private juce::ChangeListener
{
public:
    PluginManager();
    ~PluginManager() override;

    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList          knownPlugins;

    juce::PropertiesFile& settings()    { return *properties.getUserSettings(); }

    // Where a plugin that crashed while being scanned is recorded, so the next
    // scan skips it. The preferences window needs it to build the plugin list.
    juce::File crashedPluginsFile();

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void save();

    juce::ApplicationProperties properties;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginManager)
};
