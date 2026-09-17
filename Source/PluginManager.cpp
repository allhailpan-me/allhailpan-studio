#include "PluginManager.h"
#include "AhpLookAndFeel.h"
#include "DarkTitleBar.h"

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

    if (auto xml = settings().getXmlValue ("pluginList"))
        knownPlugins.recreateFromXml (*xml);

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
