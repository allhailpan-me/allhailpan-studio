#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "AudioEngine.h"
#include "PluginManager.h"
#include "PluginWindow.h"
#include "TypingKeyboard.h"
#include "Project.h"
#include "StretchCache.h"
#include "BrowserPanel.h"
#include "PlaylistComponent.h"
#include "PianoRollComponent.h"
#include "MixerComponent.h"
#include "PluginPicker.h"
#include "Logo.h"
#include "ProjectIO.h"
#include <map>

class MainComponent : public juce::Component,
                      public juce::DragAndDropContainer,
                      private juce::Timer,
                      private juce::ChangeListener
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusLost (FocusChangeType) override;
    void mouseDown (const juce::MouseEvent&) override;

    // Asks to save unsaved work, then calls quitNow (unless the user cancels).
    void requestQuit (std::function<void()> quitNow);

private:
    enum class View { playlist, pianoRoll };

    // Records parameter moves from the plugin being recorded
    struct ParamRecorder : public juce::AudioProcessorListener
    {
        explicit ParamRecorder (AudioEngine& e) : engine (e) {}
        void audioProcessorParameterChanged (juce::AudioProcessor*, int index, float value) override { engine.pushRecordedParam (index, value); }
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
        AudioEngine& engine;
    };

    // Notices when the user tweaks any plugin, so the project counts as changed
    struct EditWatcher : public juce::AudioProcessorListener
    {
        std::atomic<bool> touched { false };
        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override { touched.store (true); }
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
    };

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    // transport + recording
    void togglePlay();
    void stopAll();
    void playFrom (double beat);
    void setPosition (double beat);
    void toggleRecord();
    void finishRecording();
    void collectRecordedMidi();

    // arrangement
    void pushArrangement (bool allowRenders);

    // channels, instruments, effects
    void refreshPluginLists();
    void refreshChannelControls();
    void selectChannel (int channel);
    void loadInstrument (const juce::PluginDescription&);
    void unloadInstrument (int channel);
    void showFxMenu (int insert, int slot);
    void loadFx (int insert, int slot, const juce::PluginDescription&);
    void openPluginWindow (juce::AudioPluginInstance&);
    void closePluginWindow (juce::AudioProcessor*);

    // views and misc
    void undoRedo (bool redo);
    void updateUndoButtons();
    void setView (View);
    void toggleMixerWindow();

    // projects
    void showFileMenu();
    void newProject();
    void openProjectDialog();
    bool loadProject (const juce::File&, bool recovered = false);
    void saveCurrent (std::function<void()> then = {});
    void saveAs (bool collectSamples, std::function<void()> then = {});
    bool saveProject (const juce::File&, bool collectSamples);
    void autosave();
    void offerRecovery();
    void confirmDiscard (std::function<void()> then);
    void showLoadProblems (const ProjectIO::LoadReport&);
    void findMissingSamples();
    void clearSession();
    void markDirty();
    void updateTitle();
    void ignoreEditsForAWhile() { ignoreEditsUntil = juce::Time::getMillisecondCounter() + 2000; }
    std::unique_ptr<juce::AudioPluginInstance> createPluginForProject (const juce::PluginDescription&, juce::String& error);
    static juce::File projectsFolder();
    void showAudioSettings();
    void setStatus (const juce::String&);
    void updateTypingLabel();
    void paintStatus (juce::Graphics&, juce::Rectangle<int>);

    EditWatcher    editWatcher;      // must outlive every plugin
    PluginManager  plugins;
    AudioEngine    engine;
    TypingKeyboard typing { engine.keyboard() };
    SampleCache    cache;
    Project        project;
    StretchCache   stretchCache { [this] { pushArrangement (true); playlist.refresh(); } };
    ParamRecorder  paramRecorder { engine };

    BrowserPanel       browser   { cache, engine, plugins.settings() };
    PlaylistComponent  playlist  { project, engine, cache };
    PianoRollComponent pianoRoll { project, engine };
    MixerComponent     mixer     { engine, project };
    SpinningLogo       logo;

    // top bar
    juce::TextButton playButton { "Play" }, stopButton { "Stop" }, recordButton { "Rec" }, clickButton { "Click" };
    juce::ComboBox   recordMode;
    juce::TextButton playlistTab { "Playlist" }, pianoTab { "Piano roll" }, mixerTab { "Mixer" };
    juce::TextButton audioButton { "Audio settings" }, pluginsButton { "Plugins" };
    juce::TextButton undoButton { "Undo" }, redoButton { "Redo" }, fileButton { "File" };
    juce::Slider     tempo;
    juce::Label      tempoLabel { {}, "Tempo" }, clock;

    // channel bar
    juce::Label      channelLabel { {}, "Channel" }, insertLabel { {}, "Mixer" }, typingLabel;
    juce::ComboBox   channelBox, instrumentBox, insertBox;
    juce::TextButton showButton { "Show" }, unloadButton { "Unload" }, sumOutsButton { "Mix all outs" };
    juce::TextButton browseButton { "Find..." };
    juce::MidiKeyboardComponent piano { engine.keyboard(), juce::MidiKeyboardComponent::horizontalKeyboard };

    juce::Array<juce::PluginDescription> instrumentTypes, effectTypes;
    std::map<juce::AudioProcessor*, std::unique_ptr<PluginWindow>> pluginWindows;

    juce::Rectangle<int> topBar, channelBar, workArea, pianoArea, statusBar;
    juce::String startupError, statusMessage;
    juce::int64  statusTime = 0;
    juce::String loadingName;
    View view = View::playlist;
    bool focusGrabbed = false;

    // project file state
    juce::File currentFile;
    bool dirty = false;
    juce::uint32 ignoreEditsUntil = 0;
    int autosaveTicks = 0;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<MixerWindow> mixerWindow;
    juce::RecentlyOpenedFilesList recent;

    // recording state
    bool   recordingAudio = false, recordingMidi = false;
    int    audioTrack = 0, midiTrack = 0, midiChannel = 0;
    juce::AudioPluginInstance* recordingPlugin = nullptr;
    std::vector<MidiNote> takeNotes;                    // absolute beats
    std::map<int, std::pair<double, float>> heldNotes;  // note -> (start, velocity)
    std::map<int, AutoLane> takeLanes;                  // param index -> lane (absolute beats)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
