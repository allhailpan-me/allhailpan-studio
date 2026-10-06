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
#include "ChannelRackComponent.h"
#include "ModulatorPanel.h"
#include "MixReportPanel.h"
#include <array>
#include <atomic>
#include "MixerComponent.h"
#include "PluginPicker.h"
#include "PreferencesWindow.h"
#include "Logo.h"
#include "ProjectIO.h"
#include "ExportJob.h"
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
    enum class View { playlist, pianoRoll, rack, modulators, mixReport };

    // Records parameter moves from the plugin being recorded.
    // Only knobs the user actually grabs are recorded: plugins like Synplant
    // animate their own parameters constantly, and recording that flood would
    // play back as nonsense.
    struct ParamRecorder : public juce::AudioProcessorListener
    {
        explicit ParamRecorder (AudioEngine& e) : engine (e) {}

        // A hosted plugin is free to report a parameter change from inside
        // its own processBlock, which happens on the audio thread. So the
        // "is this knob being held" question has to be answerable without a
        // lock: a std::set behind a spin lock meant the audio thread could
        // spin waiting for a lock the message thread was holding across the
        // set's own allocation. A flag per parameter answers it with one
        // atomic read and nothing to allocate.
        //
        // Fixed length rather than sized from the plugin, because resizing it
        // is exactly what must not happen while the audio thread is reading.
        // No plugin exposes four thousand parameters; anything past the end
        // is simply not recorded, which is the same as the gesture never
        // having been seen.
        static constexpr int maxParams = 4096;

        void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int index) override
        {
            if (juce::isPositiveAndBelow (index, maxParams))
                held[(size_t) index].store (true);
            sawGesture.store (true);
        }
        void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int index) override
        {
            if (juce::isPositiveAndBelow (index, maxParams))
                held[(size_t) index].store (false);
        }
        void audioProcessorParameterChanged (juce::AudioProcessor*, int index, float value) override
        {
            if (! juce::isPositiveAndBelow (index, maxParams) || ! held[(size_t) index].load())
                return;                          // the plugin moved it, not the user
            engine.pushRecordedParam (index, value);
        }
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}

        void reset()
        {
            for (auto& h : held)
                h.store (false);
            sawGesture.store (false);
        }

        AudioEngine& engine;
        std::array<std::atomic<bool>, (size_t) maxParams> held {};
        std::atomic<bool> sawGesture { false };
    };

    // Notices when the user tweaks any plugin, so the project counts as changed.
    // It also remembers the last knob actually grabbed, which is how a
    // modulator learns what to attach to: the user opens the plugin's own
    // window and moves the control, rather than hunting for it by number.
    struct EditWatcher : public juce::AudioProcessorListener
    {
        std::atomic<bool> touched { false };
        std::atomic<bool> grabbed { false };

        void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor* p, int index) override
        {
            const juce::SpinLock::ScopedLockType lock (grabLock);
            grabbedProcessor = p;
            grabbedParam = index;
            grabbed.store (true);
        }

        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override { touched.store (true); }
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}

        /** Takes the pending grab, if there is one. */
        bool takeGrab (juce::AudioProcessor*& processor, int& index)
        {
            if (! grabbed.exchange (false))
                return false;

            const juce::SpinLock::ScopedLockType lock (grabLock);
            processor = grabbedProcessor;
            index = grabbedParam;
            return processor != nullptr && index >= 0;
        }

        juce::SpinLock grabLock;
        juce::AudioProcessor* grabbedProcessor = nullptr;
        int grabbedParam = -1;
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
    void finishAudioRecording (std::vector<Recorder::Take> passes, bool throughInterface);

    // channels, instruments, effects
    void refreshPluginLists();
    void refreshChannelControls();
    void selectChannel (int channel);
    void loadInstrument (const juce::PluginDescription&);
    void unloadInstrument (int channel);
    void showFxMenu (int insert, int slot);
    void showBusRouting (int channel);
    void loadFx (int insert, int slot, const juce::PluginDescription&);
    void openPluginWindow (juce::AudioPluginInstance&);
    void closePluginWindow (juce::AudioProcessor*);
    void updateFloatingWindows();

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
    void exportAudio (bool stems);

    // MIDI file import and export. The export writes the arrangement, or just
    // the selected clips, as a standard multi-track file; the import is the
    // same path a dropped file takes, so there is one set of behaviour.
    void exportMidi (bool onlySelected);
    void importMidiDialog();
    void checkExportProgress();
    void clearSession();
    void markDirty();
    /** Takes the loop range as it stands to be the saved one, so that opening
        or saving a project does not leave it looking modified. */
    void noteLoopRange() { lastLoopStart = project.loopStart; lastLoopEnd = project.loopEnd; }
    void updateTitle();
    void ignoreEditsForAWhile() { ignoreEditsUntil = juce::Time::getMillisecondCounter() + 2000; }
    std::unique_ptr<juce::AudioPluginInstance> createPluginForProject (const juce::PluginDescription&, juce::String& error);
    static juce::File projectsFolder();
    void showPreferences (PreferencesComponent::Tab tab);

    // What each setting in the preferences window does, handed to it so that
    // the window owns the controls and this owns the effect.
    PreferencesActions preferenceActions();
    void setStatus (const juce::String&);

    /** Fills the input menu from the device that is open now, keeping the
        stored choice when it still exists. Called at startup and whenever the
        device changes, because an interface with eight inputs and one with
        two do not offer the same list. */
    void refreshInputSources();
    void updateTypingLabel();
    void pushArmedState();
    void checkModulationLearn();
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
    ChannelRackComponent rack { project, engine };
    ModulatorPanel       modPanel { project, engine };
    MixReportPanel       mixReport { engine };
    MixerComponent     mixer     { engine, project };
    SpinningLogo       logo;

    // top bar
    juce::TextButton playButton { "Play" }, stopButton { "Stop" }, recordButton { "Rec" }, clickButton { "Click" };
    juce::ComboBox   recordMode, countInBox, monitorBox, inputBox;

    // What the input menu was last built for. Starts at a count no device can
    // have, so the menu is built on the first timer tick whatever happens.
    int lastInputChannelCount = -1;
    juce::TextButton playlistTab { "Playlist" }, pianoTab { "Piano roll" }, rackTab { "Rack" };
    juce::TextButton modTab { "Mod" }, reportTab { "Report" }, mixerTab { "Mixer" };
    juce::TextButton audioButton { "Audio" }, pluginsButton { "Plugins" };
    juce::TextButton undoButton { "Undo" }, redoButton { "Redo" }, fileButton { "File" };
    juce::Slider     tempo;
    juce::Label      tempoLabel { {}, "Tempo" }, clock;

    // channel bar
    juce::Label      channelLabel { {}, "Channel" }, insertLabel { {}, "Mixer" }, typingLabel;
    juce::ComboBox   channelBox, instrumentBox, insertBox;
    juce::TextButton showButton { "Show" }, unloadButton { "Unload" }, sumOutsButton { "Mix all outs" };
    juce::TextButton routeOutsButton { "Route outs..." };
    juce::TextButton browseButton { "Find..." };
    juce::MidiKeyboardComponent piano { engine.keyboard(), juce::MidiKeyboardComponent::horizontalKeyboard };

    juce::Array<juce::PluginDescription> instrumentTypes, effectTypes;
    std::map<juce::AudioProcessor*, std::unique_ptr<PluginWindow>> pluginWindows;
    bool windowsFloat = false;      // see updateFloatingWindows

    juce::Rectangle<int> topBar, channelBar, workArea, pianoArea, statusBar;
    juce::String startupError, statusMessage;
    juce::int64  statusTime = 0;
    juce::String loadingName;
    View view = View::playlist;
    bool focusGrabbed = false;

    // project file state
    juce::File currentFile;
    bool dirty = false;
    // What the loop range was when it was last noticed, so that moving it
    // marks the project as worth saving without becoming an undo step.
    double lastLoopStart = 0.0, lastLoopEnd = 0.0;
    juce::uint32 ignoreEditsUntil = 0;
    int autosaveTicks = 0;

    // Ticks of this component's timer between autosaves, or zero for off. Set
    // from the settings file rather than compiled in, which it used to be.
    int autosaveEvery = 0;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<MixerWindow> mixerWindow;
    std::unique_ptr<ExportJob> exportJob;
    juce::RecentlyOpenedFilesList recent;

    // recording state
    bool   recordingAudio = false, recordingMidi = false, recordingInstrument = false;
    int    audioTrack = 0, midiTrack = 0, midiChannel = 0;
    juce::AudioPluginInstance* recordingPlugin = nullptr;
    bool recordingPluginProducesMidi = false;
    std::vector<MidiNote> takeNotes;                    // absolute beats
    std::map<int, std::pair<double, float>> heldNotes;  // note -> (start, velocity)
    std::map<int, AutoLane> takeLanes;                  // param index -> lane (absolute beats)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
