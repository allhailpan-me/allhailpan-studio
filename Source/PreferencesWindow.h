#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "AhpLookAndFeel.h"
#include "AudioSettingsPanel.h"
#include "DarkTitleBar.h"
#include "PluginManager.h"
#include "Preferences.h"
#include <functional>

//==============================================================================
/** One window for the settings that belong to the application rather than to a
    project.

    They used to be in three places that had nothing to do with each other: the
    audio device behind one button, the plugin list behind another, and the rest
    nowhere at all, because several of them were constants nobody could reach.
    The metronome was a fixed level, the autosave a fixed two minutes, and a new
    project always opened at 128.

    What is deliberately not in here is the transport: monitoring, the record
    mode and the count-in stay on the toolbar. They look like preferences and
    they are saved like preferences, but they are decisions made while working,
    several times a session, and a setting someone changes between two takes
    does not belong two clicks deep in a window. Only their validation is
    shared, in Preferences.h.

    Everything applies as it is changed. A preferences window with an apply
    button asks the user to tell it twice, and for a level control it also hides
    the one thing they want, which is to hear the change while the metronome is
    running.
*/

//==============================================================================
/** Reads a number from the settings file, treating a value that is not there,
    or not a number, as not set.

    PropertiesFile hands back zero for anything it cannot parse, and zero is a
    real setting for most of these: it is off for the autosave, and full level
    for the metronome. So the text is checked before it is trusted, which is
    the only way a garbled line reaches the default rather than the loudest
    click in the range.
*/
inline double storedPreference (juce::PropertiesFile& settings, const char* key, const Prefs::Range& range)
{
    const auto text = settings.getValue (key).trim();

    if (! Prefs::looksNumeric (text.toRawUTF8()))
        return range.fallback;

    return Prefs::number (text.getDoubleValue(), range);
}

//==============================================================================
/** What the settings in this window actually do. The window owns the controls
    and the saving; the application owns the effect. */
struct PreferencesActions
{
    /** Linear gain for the metronome, zero being silent. */
    std::function<void (float)> setMetronomeGain;

    /** Ticks of the main timer between autosaves, zero turning them off. */
    std::function<void (int)> setAutosaveTicks;

    /** The tempo a new project starts at. */
    std::function<void (double)> setDefaultTempo;
};

/** Reads every setting this window owns and applies it.

    Called once at startup and again whenever a control moves, rather than each
    control applying only itself. One path from a stored setting to its effect
    cannot drift from another, and these are cheap enough that doing all three
    costs nothing.
*/
inline void applyPreferences (juce::PropertiesFile& settings, const PreferencesActions& actions)
{
    if (actions.setMetronomeGain)
        actions.setMetronomeGain (Prefs::metronomeGain (storedPreference (settings, Prefs::Key::metronomeLevelDb,
                                                                         Prefs::metronomeLevelDb)));

    if (actions.setAutosaveTicks)
    {
        const int id = Prefs::choiceId (settings.getIntValue (Prefs::Key::autosaveInterval,
                                                             Prefs::autosaveChoice.fallback),
                                       Prefs::autosaveChoice);

        actions.setAutosaveTicks (Prefs::autosaveTicks (Prefs::autosaveMinutesFor (id),
                                                        Prefs::mainTimerHz));
    }

    if (actions.setDefaultTempo)
        actions.setDefaultTempo (storedPreference (settings, Prefs::Key::defaultTempo, Prefs::defaultTempo));
}

//==============================================================================
/** The settings that had nowhere else to live. */
class GeneralPreferencesPanel : public juce::Component
{
public:
    GeneralPreferencesPanel (juce::PropertiesFile& s, PreferencesActions a)
        : settings (s), actions (std::move (a))
    {
        addAndMakeVisible (autosaveLabel);
        addAndMakeVisible (autosaveBox);
        addAndMakeVisible (autosaveNote);
        addAndMakeVisible (clickLabel);
        addAndMakeVisible (clickLevel);
        addAndMakeVisible (clickNote);
        addAndMakeVisible (tempoLabel);
        addAndMakeVisible (tempoValue);
        addAndMakeVisible (tempoNote);
        addAndMakeVisible (folderNote);
        addAndMakeVisible (revealButton);
        addAndMakeVisible (resetButton);

        for (auto* l : { &autosaveLabel, &clickLabel, &tempoLabel })
            l->setJustificationType (juce::Justification::centredLeft);

        for (auto* l : { &autosaveNote, &clickNote, &tempoNote, &folderNote })
        {
            l->setColour (juce::Label::textColourId, Ahp::muted);
            l->setFont (juce::FontOptions (12.0f));
            l->setJustificationType (juce::Justification::topLeft);
        }

        // ---- autosave ----
        for (int i = 0; i < Prefs::autosaveChoiceCount; ++i)
            autosaveBox.addItem (describeInterval (Prefs::autosaveChoices[(size_t) i]), i + 1);

        autosaveBox.onChange = [this]
        {
            settings.setValue (Prefs::Key::autosaveInterval, autosaveBox.getSelectedId());
            applyAndSave();
        };
        autosaveNote.setText ("The autosave is a separate file, offered back if the studio did not close "
                              "cleanly. It never overwrites your project, and it is skipped while you are "
                              "recording or in the middle of an edit.",
                              juce::dontSendNotification);

        // ---- metronome ----
        clickLevel.setSliderStyle (juce::Slider::LinearHorizontal);
        clickLevel.setRange (Prefs::metronomeLevelDb.minimum, Prefs::metronomeLevelDb.maximum, 0.1);
        clickLevel.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);

        // The bottom of the range is silence rather than a very quiet click, so
        // the number there would be a lie.
        clickLevel.textFromValueFunction = [] (double db)
        {
            return db <= Prefs::metronomeLevelDb.minimum ? juce::String ("off")
                                                         : juce::String (db, 1) + " dB";
        };
        clickLevel.valueFromTextFunction = [] (const juce::String& text)
        {
            return text.containsIgnoreCase ("off") ? Prefs::metronomeLevelDb.minimum
                                                   : text.getDoubleValue();
        };
        clickLevel.onValueChange = [this]
        {
            settings.setValue (Prefs::Key::metronomeLevelDb, clickLevel.getValue());
            applyAndSave();
        };
        clickNote.setText ("How loud the count-in and the metronome are. The click is added after the mix, "
                           "so it is never exported and never part of what the meters read.",
                           juce::dontSendNotification);

        // ---- tempo of a new project ----
        tempoValue.setSliderStyle (juce::Slider::LinearBar);
        tempoValue.setRange (Prefs::defaultTempo.minimum, Prefs::defaultTempo.maximum, 0.01);
        tempoValue.setNumDecimalPlacesToDisplay (2);
        tempoValue.onValueChange = [this]
        {
            settings.setValue (Prefs::Key::defaultTempo, tempoValue.getValue());
            applyAndSave();
        };
        tempoNote.setText ("Where File > New starts. A dropped MIDI or audio file still offers its own tempo "
                           "while the project is at this one, because that is how the studio tells a tempo "
                           "nobody chose from one you meant.",
                           juce::dontSendNotification);

        // ---- where the file is ----
        folderNote.setText ("Settings, the plugin list and the autosave are kept in\n"
                              + settings.getFile().getParentDirectory().getFullPathName(),
                            juce::dontSendNotification);

        revealButton.onClick = [this] { settings.getFile().revealToUser(); };
        revealButton.setTooltip ("Opens the folder these settings are stored in.");

        resetButton.onClick = [this] { resetToDefaults(); };
        resetButton.setTooltip ("Puts the three settings above back to their defaults. "
                                "Your audio device and plugin list are not touched.");

        refreshFromSettings();
        setSize (560, 460);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16, 14);

        auto row = [&area] (int height, int gap = 6)
        {
            auto r = area.removeFromTop (height);
            area.removeFromTop (gap);
            return r;
        };

        autosaveLabel.setBounds (row (20, 2));
        autosaveBox  .setBounds (row (26).removeFromLeft (220));
        autosaveNote .setBounds (row (48, 18));

        clickLabel.setBounds (row (20, 2));
        clickLevel.setBounds (row (26).removeFromLeft (360));
        clickNote .setBounds (row (34, 18));

        tempoLabel.setBounds (row (20, 2));
        tempoValue.setBounds (row (26).removeFromLeft (160));
        tempoNote .setBounds (row (48, 18));

        auto bottom = area.removeFromBottom (28);
        revealButton.setBounds (bottom.removeFromLeft (150));
        bottom.removeFromLeft (8);
        resetButton.setBounds (bottom.removeFromLeft (190));

        area.removeFromBottom (8);
        folderNote.setBounds (area.removeFromBottom (36));
    }

private:
    static juce::String describeInterval (double minutes)
    {
        if (minutes <= 0.0)
            return "Off";

        if (minutes < 1.0)
            return "Every " + juce::String (juce::roundToInt (minutes * 60.0)) + " seconds";

        if (minutes == 1.0)
            return "Every minute";

        return "Every " + juce::String (minutes, minutes == std::floor (minutes) ? 0 : 1) + " minutes";
    }

    void refreshFromSettings()
    {
        autosaveBox.setSelectedId (Prefs::choiceId (settings.getIntValue (Prefs::Key::autosaveInterval,
                                                                         Prefs::autosaveChoice.fallback),
                                                   Prefs::autosaveChoice),
                                   juce::dontSendNotification);

        clickLevel.setValue (storedPreference (settings, Prefs::Key::metronomeLevelDb, Prefs::metronomeLevelDb),
                             juce::dontSendNotification);

        tempoValue.setValue (storedPreference (settings, Prefs::Key::defaultTempo, Prefs::defaultTempo),
                             juce::dontSendNotification);
    }

    void applyAndSave()
    {
        applyPreferences (settings, actions);

        // Written out now rather than at shutdown. A preference the user set and
        // then lost to a crash is worse than one they never found.
        settings.saveIfNeeded();
    }

    void resetToDefaults()
    {
        settings.setValue (Prefs::Key::autosaveInterval,  Prefs::autosaveChoice.fallback);
        settings.setValue (Prefs::Key::metronomeLevelDb, Prefs::metronomeLevelDb.fallback);
        settings.setValue (Prefs::Key::defaultTempo,     Prefs::defaultTempo.fallback);

        refreshFromSettings();
        applyAndSave();
    }

    juce::PropertiesFile& settings;
    PreferencesActions actions;

    juce::Label    autosaveLabel { {}, "Autosave" }, autosaveNote;
    juce::ComboBox autosaveBox;

    juce::Label  clickLabel { {}, "Metronome level" }, clickNote;
    juce::Slider clickLevel;

    juce::Label  tempoLabel { {}, "Tempo of a new project" }, tempoNote;
    juce::Slider tempoValue;

    juce::Label      folderNote;
    juce::TextButton revealButton { "Show settings folder" };
    juce::TextButton resetButton  { "Reset these to defaults" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GeneralPreferencesPanel)
};

//==============================================================================
/** The window: the device chooser, the plugin list and the rest, in one place. */
class PreferencesComponent : public juce::Component
{
public:
    enum Tab { general = 0, audio, plugins };

    PreferencesComponent (PluginManager& pluginManager, juce::AudioDeviceManager& devices,
                          PreferencesActions actions)
        : tabs (juce::TabbedButtonBar::TabsAtTop)
    {
        addAndMakeVisible (tabs);
        tabs.setOutline (0);
        tabs.setTabBarDepth (28);

        tabs.addTab ("General", Ahp::panel,
                     new GeneralPreferencesPanel (pluginManager.settings(), std::move (actions)), true);

        tabs.addTab ("Audio", Ahp::panel, new AudioSettingsPanel (devices), true);

        // The same list component the Plugins button used to open on its own.
        // Scanning writes straight into the known plugin list, which saves
        // itself, so nothing here needs to be applied.
        tabs.addTab ("Plugins", Ahp::panel,
                     new juce::PluginListComponent (pluginManager.formats, pluginManager.knownPlugins,
                                                    pluginManager.crashedPluginsFile(),
                                                    &pluginManager.settings(), true),
                     true);

        setSize (820, 620);
    }

    void setTab (Tab t) { tabs.setCurrentTabIndex ((int) t); }

    void resized() override { tabs.setBounds (getLocalBounds()); }

private:
    juce::TabbedComponent tabs;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreferencesComponent)
};

//==============================================================================
/** Opens the preferences window on the given tab, or brings the open one
    forward and switches it.

    One window rather than one per click. Two copies of the device chooser both
    editing the same device manager is a way to lose a setting, and two plugin
    scans at once is worse.
*/
/** The one open preferences window, or null.

    Empties itself when the window closes, because DialogWindow::launchAsync
    deletes the window it returns and a SafePointer notices.
*/
inline juce::Component::SafePointer<juce::DialogWindow>& preferencesWindow()
{
    static juce::Component::SafePointer<juce::DialogWindow> open;
    return open;
}

inline void showPreferencesWindow (PluginManager& pluginManager, juce::AudioDeviceManager& devices,
                                   const PreferencesActions& actions,
                                   PreferencesComponent::Tab tab = PreferencesComponent::general)
{
    auto& open = preferencesWindow();

    if (open != nullptr)
    {
        if (auto* content = dynamic_cast<PreferencesComponent*> (open->getContentComponent()))
            content->setTab (tab);

        open->toFront (true);
        return;
    }

    auto content = std::make_unique<PreferencesComponent> (pluginManager, devices, actions);
    content->setTab (tab);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (content.release());
    o.dialogTitle                  = "Preferences";
    o.dialogBackgroundColour       = Ahp::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar            = true;
    o.resizable                    = true;

    if (auto* w = o.launchAsync())
    {
        Ahp::applyDarkTitleBar (*w);
        open = w;
    }
}

/** Closes the preferences window if it is open.

    The window is owned by the desktop rather than by whatever opened it, but it
    holds references to the plugin manager and the device manager, both of which
    belong to the main component. So the main component closes it on the way
    out, rather than leaving a window on screen pointing at things that have
    gone.
*/
inline void closePreferencesWindow()
{
    preferencesWindow().deleteAndZero();
}
