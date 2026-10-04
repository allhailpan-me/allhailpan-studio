#include "PluginManager.h"
#include "AhpLookAndFeel.h"
#include "DarkTitleBar.h"
#include "PluginScanner.h"
#include "InternalPluginFormat.h"

PluginManager::PluginManager()
{
    juce::PropertiesFile::Options options;
    options.applicationName     = "ALLHAILPAN Studio";
    options.folderName          = "ALLHAILPAN Studio";
    options.filenameSuffix      = ".settings";
    options.osxLibrarySubFolder = "Application Support";
    properties.setStorageParameters (options);

    // Registers every plugin format enabled for this platform
    // (VST3 everywhere, AU on macOS, LV2 on Linux).
    juce::addDefaultFormatsToManager (formats);

    // And the instruments compiled into the studio, which are a plugin
    // format of their own so that everything else can treat them as
    // ordinary plugins.
    formats.addFormat (std::make_unique<InternalPluginFormat>());

    if (auto xml = settings().getXmlValue ("pluginList"))
        knownPlugins.recreateFromXml (*xml);

    // Added after the saved list is restored, and every time, rather than
    // left to a scan. A built-in instrument that only appears once the user
    // has thought to scan for plugins is no better than not shipping one,
    // since the whole point is that a fresh install makes sound. addType
    // refuses duplicates by identifier, so doing this on every launch costs
    // nothing and repairs a list saved before the instrument existed.
    for (const auto& description : InternalPluginFormat::descriptions())
        knownPlugins.addType (description);

    // Plugins are examined in a child process, so one that crashes on load
    // cannot take the studio down with it.
    knownPlugins.setCustomScanner (std::make_unique<OutOfProcessScanner>());

    knownPlugins.addChangeListener (this);
}

PluginManager::~PluginManager()
{
    knownPlugins.removeChangeListener (this);
    save();
}

juce::File PluginManager::crashedPluginsFile()
{
    // If a plugin crashes during scanning, it is recorded here and skipped next time.
    return settings().getFile().getSiblingFile ("RecentlyCrashedPlugins");
}

void PluginManager::showPluginWindow()
{
    auto* list = new juce::PluginListComponent (formats, knownPlugins, crashedPluginsFile(), &settings(), true);
    list->setSize (780, 540);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (list);
    o.dialogTitle                  = "Plugins";
    o.dialogBackgroundColour       = Ahp::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar            = true;
    o.resizable                    = true;
    if (auto* w = o.launchAsync())
        Ahp::applyDarkTitleBar (*w);
}

void PluginManager::changeListenerCallback (juce::ChangeBroadcaster*)
{
    save();
}

void PluginManager::save()
{
    if (auto xml = knownPlugins.createXml())
        settings().setValue ("pluginList", xml.get());

    settings().saveIfNeeded();
}
