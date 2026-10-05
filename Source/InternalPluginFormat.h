#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

#include "PanOne.h"

#include <memory>

//==============================================================================
/** Makes the instruments compiled into the studio look like any other plugin.

    There are two ways to have a built-in instrument. One is to special case
    it everywhere: a separate entry in the channel dropdown, a separate branch
    when a project is saved, another when it is loaded, another in the MIDI
    recorder, another in the modulators. That is five places to forget, and
    the forgetting shows up as an instrument that is nearly a plugin.

    The other is to give it a plugin format of its own, which is what this
    is. From everywhere else in the studio PAN One is then a plugin like a
    scanned VST3: it comes out of the same list, loads through the same
    createPluginInstanceAsync, is written into an .ahp by the same
    PluginDescription, and is modulated and recorded by the same code. The
    format is a dozen small functions and it buys all of that.

    Note on JUCE 9: AudioPluginFormat lives in juce_audio_processors_headless
    rather than juce_audio_processors, which is where it was through JUCE 8.
    Including juce_audio_processors brings it in, since that module depends
    on the headless one, but anything looking for juce_AudioPluginFormat.h
    under the old path will not find it. CLAUDE.md records this.

    isTrivialToScan is true, which is the part that matters for start up:
    plugin scanning here runs in a child process so a crashing plugin cannot
    take the studio down, and launching one to examine an instrument that is
    already compiled in would be pointless. It also means no scan is needed
    before the instrument appears, which is the whole point of shipping one.
*/
class InternalPluginFormat final : public juce::AudioPluginFormat
{
public:
    InternalPluginFormat() = default;

    static juce::String formatName() { return "Built in"; }

    /** Every instrument this format offers, for adding straight to the
        plugin list at startup rather than waiting for a scan. */
    static juce::Array<juce::PluginDescription> descriptions()
    {
        juce::Array<juce::PluginDescription> all;
        all.add (PanOne::describe());
        return all;
    }

    juce::String getName() const override { return formatName(); }

    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results,
                              const juce::String& fileOrIdentifier) override
    {
        for (const auto& d : descriptions())
            if (d.fileOrIdentifier == fileOrIdentifier)
                results.add (new juce::PluginDescription (d));
    }

    bool fileMightContainThisPluginType (const juce::String& fileOrIdentifier) override
    {
        for (const auto& d : descriptions())
            if (d.fileOrIdentifier == fileOrIdentifier)
                return true;

        return false;
    }

    juce::String getNameOfPluginFromIdentifier (const juce::String& fileOrIdentifier) override
    {
        for (const auto& d : descriptions())
            if (d.fileOrIdentifier == fileOrIdentifier)
                return d.name;

        return {};
    }

    // Compiled in, so it cannot change underneath us and cannot go missing.
    bool pluginNeedsRescanning (const juce::PluginDescription&) override { return false; }
    bool doesPluginStillExist (const juce::PluginDescription&) override  { return true; }

    // There is nowhere to scan: the list is the one above.
    bool canScanForPlugins() const override { return false; }
    bool isTrivialToScan() const override   { return true; }

    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool, bool) override
    {
        juce::StringArray paths;

        for (const auto& d : descriptions())
            paths.add (d.fileOrIdentifier);

        return paths;
    }

    juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }

    bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override
    {
        return false;
    }

protected:
    void createPluginInstance (const juce::PluginDescription& description,
                               double initialSampleRate, int initialBufferSize,
                               PluginCreationCallback callback) override
    {
        std::unique_ptr<juce::AudioPluginInstance> instance;

        if (description.fileOrIdentifier == PanOne::identifier())
            instance = std::make_unique<PanOne>();

        if (instance == nullptr)
        {
            callback (nullptr, "No built in instrument called " + description.name);
            return;
        }

        // Prepared here so the first note after loading is not the thing
        // that discovers the sample rate.
        instance->prepareToPlay (initialSampleRate, initialBufferSize);
        callback (std::move (instance), {});
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InternalPluginFormat)
};
