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

    /** The correction to what the driver claims its latency is, in
        milliseconds, positive pulling recorded takes earlier. */
    std::function<void (double)> setRecordTrimMs;

    /** How solo behaves, in the order the engine takes them: the gain a strip
        silenced by somebody else's solo goes to, then whether a solo beats a
        mute on the same strip, then whether soloing one strip clears the
        others. All three are Ardour's, and all three are application settings
        there too.

        The order is written down because a std::function's parameters have no
        names, so this comment is the only thing a second caller would have to
        go on, and the two flags swapped is silent: exclusive solo would stop
        clearing the other strips and a strip that is both muted and soloed
        would become audible, with nothing erroring. An audit found this
        comment listing them the other way round. */
    std::function<void (float, bool, bool)> setSoloSettings;

    /** How far ahead of its arrival a frame of a capture belongs, as the
        engine works it out right now. Shown rather than set, so that a studio
        applying nothing can be told apart from one applying the wrong
        amount. */
    std::function<int()> recordOffsetSamples;

    /** Asks the engine to go and find the fastest driver this machine has,
        and returns a sentence about what it found. Blocks while it works. */
    std::function<juce::String()> findFastestDevice;
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

    if (actions.setRecordTrimMs)
        actions.setRecordTrimMs (storedPreference (settings, Prefs::Key::recordTrimMs, Prefs::recordTrimMs));

    if (actions.setSoloSettings)
        actions.setSoloSettings (
            Prefs::soloMuteGain (storedPreference (settings, Prefs::Key::soloMuteCutDb,
                                                   Prefs::soloMuteCutDb)),
            Prefs::flag (settings.getDoubleValue (Prefs::Key::soloOverridesMute, 0.0), false),
            Prefs::flag (settings.getDoubleValue (Prefs::Key::exclusiveSolo, 0.0), false));
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
        addAndMakeVisible (soloLabel);
        addAndMakeVisible (soloCut);
        addAndMakeVisible (exclusiveSolo);
        addAndMakeVisible (soloOverridesMute);
        addAndMakeVisible (soloNote);
        addAndMakeVisible (folderNote);
        addAndMakeVisible (revealButton);
        addAndMakeVisible (resetButton);

        for (auto* l : { &autosaveLabel, &clickLabel, &tempoLabel, &soloLabel })
            l->setJustificationType (juce::Justification::centredLeft);

        for (auto* l : { &autosaveNote, &clickNote, &tempoNote, &soloNote, &folderNote })
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

        // ---- solo ----
        //
        // Ardour's three, with its own labels where it has one. The cut is a
        // fader there too, labelled "Solo-in-place mute cut (dB)", and its
        // bottom end is full mute, which is what it ships.
        soloCut.setSliderStyle (juce::Slider::LinearHorizontal);
        soloCut.setRange (Prefs::soloMuteCutDb.minimum, Prefs::soloMuteCutDb.maximum, 0.5);
        soloCut.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);

        // The bottom of the range is silence, so the number there would be a
        // lie, and the top is 0 dB, where solo stops doing anything at all and
        // saying "0.0 dB" would not warn anybody.
        soloCut.textFromValueFunction = [] (double db)
        {
            if (db <= Prefs::soloMuteCutDb.minimum)
                return juce::String ("silent");

            return db >= Prefs::soloMuteCutDb.maximum ? juce::String ("no change")
                                                      : juce::String (db, 1) + " dB";
        };
        soloCut.valueFromTextFunction = [] (const juce::String& text)
        {
            if (text.containsIgnoreCase ("silent"))
                return Prefs::soloMuteCutDb.minimum;

            return text.containsIgnoreCase ("no change") ? Prefs::soloMuteCutDb.maximum
                                                         : text.getDoubleValue();
        };
        soloCut.onValueChange = [this]
        {
            settings.setValue (Prefs::Key::soloMuteCutDb, soloCut.getValue());
            applyAndSave();
        };

        exclusiveSolo.setButtonText ("Only one strip soloed at a time");
        exclusiveSolo.setTooltip ("Soloing a strip clears the others. Alt click does the opposite of "
                                  "whichever way this is set.");
        exclusiveSolo.onClick = [this]
        {
            settings.setValue (Prefs::Key::exclusiveSolo, exclusiveSolo.getToggleState() ? 1 : 0);
            applyAndSave();
        };

        soloOverridesMute.setButtonText ("Soloing overrides muting");
        soloOverridesMute.setTooltip ("With this on, a strip you have both muted and soloed is heard. "
                                      "A strip that is only audible because something feeding it was "
                                      "soloed stays muted either way.");
        soloOverridesMute.onClick = [this]
        {
            settings.setValue (Prefs::Key::soloOverridesMute, soloOverridesMute.getToggleState() ? 1 : 0);
            applyAndSave();
        };

        soloNote.setText ("Soloing a strip keeps whatever it sends to audible, so a soloed vocal still "
                          "has its reverb, and soloing a reverb keeps what feeds it. Right click a strip "
                          "in the mixer for Solo isolate, which keeps it audible whatever else is soloed: "
                          "useful on a shared effects return, and self defeating if you set it on "
                          "everything.",
                          juce::dontSendNotification);

        // ---- where the file is ----
        folderNote.setText ("Settings, the plugin list and the autosave are kept in\n"
                              + settings.getFile().getParentDirectory().getFullPathName(),
                            juce::dontSendNotification);

        revealButton.onClick = [this] { settings.getFile().revealToUser(); };
        revealButton.setTooltip ("Opens the folder these settings are stored in.");

        resetButton.onClick = [this] { resetToDefaults(); };
        resetButton.setTooltip ("Puts the settings above back to their defaults. "
                                "Your audio device and plugin list are not touched.");

        refreshFromSettings();
        setSize (560, 640);
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

        soloLabel        .setBounds (row (20, 2));
        soloCut          .setBounds (row (26).removeFromLeft (360));
        exclusiveSolo    .setBounds (row (22, 2));
        soloOverridesMute.setBounds (row (22));
        soloNote         .setBounds (row (62, 18));

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

        soloCut.setValue (storedPreference (settings, Prefs::Key::soloMuteCutDb, Prefs::soloMuteCutDb),
                          juce::dontSendNotification);

        exclusiveSolo.setToggleState (
            Prefs::flag (settings.getDoubleValue (Prefs::Key::exclusiveSolo, 0.0), false),
            juce::dontSendNotification);

        soloOverridesMute.setToggleState (
            Prefs::flag (settings.getDoubleValue (Prefs::Key::soloOverridesMute, 0.0), false),
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
        settings.setValue (Prefs::Key::soloMuteCutDb,    Prefs::soloMuteCutDb.fallback);
        settings.setValue (Prefs::Key::exclusiveSolo,     0);
        settings.setValue (Prefs::Key::soloOverridesMute, 0);

        refreshFromSettings();
        applyAndSave();
    }

    juce::PropertiesFile& settings;
    PreferencesActions actions;

    juce::Label    autosaveLabel { {}, "Autosave" }, autosaveNote;
    juce::ComboBox autosaveBox;

    juce::Label  clickLabel { {}, "Metronome level" }, clickNote;
    juce::Slider clickLevel;

    juce::Label        soloLabel { {}, "Solo" }, soloNote;
    juce::Slider       soloCut;
    juce::ToggleButton exclusiveSolo, soloOverridesMute;

    juce::Label  tempoLabel { {}, "Tempo of a new project" }, tempoNote;
    juce::Slider tempoValue;

    juce::Label      folderNote;
    juce::TextButton revealButton { "Show settings folder" };
    juce::TextButton resetButton  { "Reset these to defaults" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GeneralPreferencesPanel)
};

//==============================================================================
/** What an export is written as.

    Its own tab rather than another block on the General one, which is where
    the studios put it too: the bit depth is in the render dialog in Reaper,
    Pro Tools, Logic and Cubase alike. It is a stored setting here rather than
    a dialog in front of the file chooser because the export flow is a chooser
    launched asynchronously and threading a modal choice in front of it is a
    second thing to get right. What makes that safe rather than merely cheaper
    is that the status bar names the depth and the dither, in words, from
    before the file appears until after it has, so a setting left somewhere
    unexpected is visible at the moment it matters. It has to be on the
    progress line and not only on the message before it: that one is replaced
    by the first tick of the 60 Hz timer, so on its own it would be an
    announcement nobody could read.
*/
class ExportPreferencesPanel : public juce::Component
{
public:
    explicit ExportPreferencesPanel (juce::PropertiesFile& s)
        : settings (s)
    {
        addAndMakeVisible (depthLabel);
        addAndMakeVisible (depthBox);
        addAndMakeVisible (depthNote);
        addAndMakeVisible (ditherLabel);
        addAndMakeVisible (ditherBox);
        addAndMakeVisible (ditherNote);

        for (auto* l : { &depthLabel, &ditherLabel })
        {
            l->setFont (juce::FontOptions (15.0f, juce::Font::bold));
            l->setJustificationType (juce::Justification::centredLeft);
        }

        for (auto* l : { &depthNote, &ditherNote })
        {
            l->setColour (juce::Label::textColourId, Ahp::muted);
            l->setFont (juce::FontOptions (12.0f));
            l->setJustificationType (juce::Justification::topLeft);
        }

        // Built from Dither.h's own list rather than typed again here, so a
        // depth added there appears in the box and cannot be offered under a
        // name that does not match what the engine writes.
        for (int i = 0; i < Dither::numDepths; ++i)
            depthBox.addItem (Dither::name (Dither::depthAt (i)), i + 1);

        depthBox.onChange = [this]
        {
            settings.setValue (Prefs::Key::exportBitDepth, depthBox.getSelectedId());
            save();
            refreshDitherEnablement();
        };

        depthNote.setText ("16 bit is what a CD is and what a delivery usually asks for. 24 bit is a "
                           "working master, and is what every export was written at before there was a "
                           "choice. 32 bit float is the mix exactly as the engine computed it, for "
                           "handing on to somebody who is going to work on it further.",
                           juce::dontSendNotification);

        for (int i = 0; i < Dither::numModes; ++i)
            ditherBox.addItem (Dither::name (Dither::modeFromStored (i)), i + 1);

        ditherBox.onChange = [this]
        {
            settings.setValue (Prefs::Key::exportDither, ditherBox.getSelectedId());
            save();
        };

        refreshFromSettings();
        setSize (560, 420);
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

        depthLabel.setBounds (row (20, 2));
        depthBox  .setBounds (row (26).removeFromLeft (220));
        depthNote .setBounds (row (62, 18));

        ditherLabel.setBounds (row (20, 2));
        ditherBox  .setBounds (row (26).removeFromLeft (220));
        ditherNote .setBounds (row (104, 18));
    }

private:
    void refreshFromSettings()
    {
        depthBox.setSelectedId (Prefs::choiceId (settings.getIntValue (Prefs::Key::exportBitDepth,
                                                                      Prefs::exportBitDepth.fallback),
                                                 Prefs::exportBitDepth),
                                juce::dontSendNotification);

        ditherBox.setSelectedId (Prefs::choiceId (settings.getIntValue (Prefs::Key::exportDither,
                                                                        Prefs::exportDither.fallback),
                                                  Prefs::exportDither),
                                 juce::dontSendNotification);

        refreshDitherEnablement();
    }

    /** The dither control says what it will actually do rather than offering a
        choice that has no effect.

        Only a reduction to 16 bit is dithered, which is the policy in
        Dither.h, and the engine applies that policy itself rather than
        trusting this box: a greyed control and an engine free to disagree with
        it is two answers to one question. So this is the box agreeing with the
        engine, not the other way round, and the note says why rather than
        leaving somebody to wonder why their setting did nothing.
    */
    void refreshDitherEnablement()
    {
        const auto depth = Prefs::exportDepthFor (depthBox.getSelectedId());
        const bool dithered = depth == Dither::Depth::int16;

        ditherBox.setEnabled (dithered);

        ditherNote.setText (dithered
            ? juce::String ("Reducing a 32 bit mix to 16 bit quantises it, and without dither the error "
                            "is not noise: it follows the signal, so it arrives as distortion rather "
                            "than hiss and it gets proportionally worse the quieter the music is. Fade "
                            "outs and reverb tails are where it is heard. Triangular dither costs 4.8 dB "
                            "of noise floor, which at 16 bit puts it at -96 dBFS, and in exchange the "
                            "noise stops moving with the music. Turn it off only if this mix is going "
                            "to be mastered somewhere else, since dithering the same audio twice is "
                            "worse than doing it once.")
            : juce::String ("Nothing to dither at ") + Dither::name (depth) + ". "
                + (depth == Dither::Depth::int24
                       ? juce::String ("The quantisation floor of a 24 bit file is around -144 dBFS, "
                                       "below the noise of any microphone or preamp, so there is "
                                       "nothing there to linearise and dither would only add noise. ")
                       : juce::String ("A 32 bit float file is the mix exactly as the engine computed "
                                       "it, with no word length reduction at all, so there is no "
                                       "quantisation to dither. "))
                + "Dither is applied only when a file is written at 16 bit, which is the same rule "
                  "Ardour, Cubase and Logic follow.",
            juce::dontSendNotification);
    }

    void save()
    {
        // Written out now rather than at shutdown, for the reason the rest of
        // this window does it: a preference somebody set and then lost to a
        // crash is worse than one they never found.
        settings.saveIfNeeded();
    }

    juce::PropertiesFile& settings;

    juce::Label    depthLabel { {}, "Bit depth" }, depthNote;
    juce::ComboBox depthBox;

    juce::Label    ditherLabel { {}, "Dither" }, ditherNote;
    juce::ComboBox ditherBox;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExportPreferencesPanel)
};

//==============================================================================
/** The window: the device chooser, the plugin list and the rest, in one place. */
class PreferencesComponent : public juce::Component
{
public:
    // Export is appended rather than inserted, so the ids the rest of the
    // application already passes in keep pointing at the tabs they always did.
    enum Tab { general = 0, audio, plugins, exportSettings };

    PreferencesComponent (PluginManager& pluginManager, juce::AudioDeviceManager& devices,
                          PreferencesActions actions)
        : tabs (juce::TabbedButtonBar::TabsAtTop)
    {
        addAndMakeVisible (tabs);
        tabs.setOutline (0);
        tabs.setTabBarDepth (28);

        auto& settings = pluginManager.settings();

        // The actions are copied into both panels rather than moved into one,
        // because both apply every setting through the same applyPreferences
        // call: one path from a stored setting to its effect cannot drift
        // from another.
        tabs.addTab ("General", Ahp::panel,
                     new GeneralPreferencesPanel (settings, actions), true);

        tabs.addTab ("Audio", Ahp::panel,
                     new AudioSettingsPanel (devices,
                                             actions.findFastestDevice,
                                             actions.recordOffsetSamples,
                                             storedPreference (settings, Prefs::Key::recordTrimMs,
                                                               Prefs::recordTrimMs),
                                             [store = &settings, actions] (double ms)
                                             {
                                                 store->setValue (Prefs::Key::recordTrimMs, ms);
                                                 applyPreferences (*store, actions);

                                                 // Written out now rather than at shutdown: a
                                                 // correction somebody measured with a cable and
                                                 // then lost to a crash is worse than one they
                                                 // never found.
                                                 store->saveIfNeeded();
                                             }),
                     true);

        // The same list component the Plugins button used to open on its own.
        // Scanning writes straight into the known plugin list, which saves
        // itself, so nothing here needs to be applied.
        tabs.addTab ("Plugins", Ahp::panel,
                     new juce::PluginListComponent (pluginManager.formats, pluginManager.knownPlugins,
                                                    pluginManager.crashedPluginsFile(),
                                                    &pluginManager.settings(), true),
                     true);

        tabs.addTab ("Export", Ahp::panel,
                     new ExportPreferencesPanel (settings), true);

        // Taller than it was: the audio tab gained a line of readout and a
        // correction, and the device chooser is the thing that must not be
        // squeezed, since it is the one control in here somebody has to use.
        setSize (820, 680);
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
