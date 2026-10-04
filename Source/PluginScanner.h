#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

//==============================================================================
/** Scans plugins in a separate process.

    Loading a plugin runs somebody else's code inside this application, and a
    plugin that crashes while being examined takes the whole studio with it.
    Scanning each one in a child process means a bad plugin only kills the
    child: the studio notices the failure, skips that plugin, and carries on.

    The child is this same executable, launched with --ahp-scan. It writes what
    it found to a temporary file and exits. See runScanIfRequested() below.
*/
class OutOfProcessScanner  : public juce::KnownPluginList::CustomScanner
{
public:
    static constexpr const char* scanFlag = "--ahp-scan";

    bool findPluginTypesFor (juce::AudioPluginFormat& format,
                             juce::OwnedArray<juce::PluginDescription>& results,
                             const juce::String& fileOrIdentifier) override
    {
        // Formats that cannot crash us are cheaper to do here.
        if (format.isTrivialToScan())
        {
            format.findAllTypesForFile (results, fileOrIdentifier);
            return true;
        }

        auto output = juce::File::createTempFile (".ahpscan");

        juce::StringArray command;
        command.add (juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName());
        command.add (scanFlag);
        command.add (format.getName());
        command.add (fileOrIdentifier);
        command.add (output.getFullPathName());

        juce::ChildProcess child;

        if (! child.start (command, 0))
        {
            output.deleteFile();
            return false;
        }

        // A plugin that hangs is as bad as one that crashes, so give it a
        // generous but finite amount of time.
        if (! child.waitForProcessToFinish (60000))
        {
            child.kill();
            output.deleteFile();
            return false;
        }

        const bool crashed = child.getExitCode() != 0;

        if (! crashed && output.existsAsFile())
            if (auto xml = juce::parseXML (output))
                for (auto* element : xml->getChildIterator())
                {
                    juce::PluginDescription description;

                    if (description.loadFromXml (*element))
                        results.add (new juce::PluginDescription (description));
                }

        output.deleteFile();
        return ! crashed;
    }
};

//==============================================================================
/** If this process was launched to scan one plugin, does it and returns true.

    Called at the very top of the application's startup, before any window is
    made: a scanning child should never show an interface.
*/
inline bool runScanIfRequested (const juce::String& commandLine)
{
    auto arguments = juce::StringArray::fromTokens (commandLine, true);
    arguments.removeEmptyStrings();

    for (auto& a : arguments)
        a = a.unquoted();

    const int flagIndex = arguments.indexOf (OutOfProcessScanner::scanFlag);

    if (flagIndex < 0 || arguments.size() < flagIndex + 4)
        return false;

    const auto formatName = arguments[flagIndex + 1];
    const auto identifier = arguments[flagIndex + 2];
    const juce::File output (arguments[flagIndex + 3]);

    juce::AudioPluginFormatManager formats;
    juce::addDefaultFormatsToManager (formats);

    juce::XmlElement results ("PLUGINS");

    for (auto* format : formats.getFormats())
    {
        if (format->getName() != formatName)
            continue;

        juce::OwnedArray<juce::PluginDescription> found;
        format->findAllTypesForFile (found, identifier);

        for (auto* description : found)
            results.addChildElement (description->createXml().release());

        break;
    }

    output.replaceWithText (results.toString());
    return true;
}
