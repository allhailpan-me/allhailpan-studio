#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <array>
#include "Project.h"
#include "Recorder.h"

// ---------------------------------------------------------------------------
// The audio engine.
//
//   instrument channels (16) --+
//   playlist audio clips ------+--> mixer inserts 1..16 (8 FX each) --> master (8 FX) --> interface
//
// The message thread changes plugins and the arrangement under a spin lock,
// holding it only long enough to swap pointers. The audio thread only ever
// *tries* the lock, so it never waits.
// ---------------------------------------------------------------------------
class AudioEngine : public juce::AudioIODeviceCallback
{
public:
    AudioEngine();
    ~AudioEngine() override;

    juce::String start (const juce::XmlElement* savedDeviceState);

    juce::AudioDeviceManager& devices() noexcept   { return deviceManager; }
    juce::MidiKeyboardState&  keyboard() noexcept  { return keyboardState; }

    // ---- transport ----
    void setPlaying (bool shouldPlay) noexcept   { playing.store (shouldPlay); }
    bool isPlaying() const noexcept              { return playing.load(); }
    void stopAndRewind() noexcept                { playing.store (false); rewind.store (true); }
    void   setSongStart (double beat) noexcept   { songStart.store (std::max (0.0, beat)); }
    double getSongStart() const noexcept         { return songStart.load(); }
    void   locate (double beat) noexcept         { locateRequest.store (std::max (0.0, beat)); }

    void   setBpm (double newBpm) noexcept       { bpm.store (juce::jlimit (20.0, 400.0, newBpm)); }
    double getBpm() const noexcept               { return bpm.load(); }
    void setMetronome (bool on) noexcept         { metronome.store (on); }
    bool isMetronomeOn() const noexcept          { return metronome.load(); }

    double getBeatPosition() const noexcept      { return beatPosition.load(); }
    float  getInputLevel() const noexcept        { return inputLevel.load(); }
    double getSampleRate() const noexcept        { return sampleRate; }
    int    getBlockSize() const noexcept         { return blockSize; }
    int    getRoundTripLatencySamples();

    // ---- instrument channels ----
    void setChannelPlugin (int channel, std::unique_ptr<juce::AudioPluginInstance>);
    juce::AudioPluginInstance* getChannelPlugin (int channel) const noexcept;
    void setChannelInsert (int channel, int insert) noexcept;
    // Multi-output instruments (Microtonic Multi, drum plugins, samplers) put
    // their sounds on separate output buses. With this on, every output bus is
    // mixed down to the channel's stereo signal.
    void setChannelSumsOutputs (int channel, bool shouldSum) noexcept;
    bool getChannelSumsOutputs (int channel) const noexcept;
    int  getChannelOutputBuses (int channel) const noexcept;
    void setSelectedChannel (int channel) noexcept { selectedChannel.store (juce::jlimit (0, kNumChannels - 1, channel)); }
    // Stops every sounding note everywhere (the classic DAW panic button)
    void panic() noexcept { panicRequest.store (true); }
    int  getSelectedChannel() const noexcept       { return selectedChannel.load(); }

    // ---- mixer ----
    struct InsertControls
    {
        juce::String name;
        std::atomic<float> volume { 0.8f };     // fader position 0..1.25 (0.8 = 0 dB)
        std::atomic<float> pan    { 0.0f };     // -1..1
        std::atomic<bool>  mute   { false };
        std::atomic<float> peakL  { 0.0f }, peakR { 0.0f };
        std::array<std::atomic<bool>, kNumFxSlots> bypass {};
    };
    InsertControls& insert (int index) noexcept { return controls[(size_t) juce::jlimit (0, kNumInserts - 1, index)]; }

    void setFx (int insert, int slot, std::unique_ptr<juce::AudioPluginInstance>);
    juce::AudioPluginInstance* getFx (int insert, int slot) const noexcept;

    template <typename Fn> void forEachPlugin (Fn&& fn) const
    {
        for (auto& c : channelSlots) if (c.plugin) fn (*c.plugin);
        for (auto& i : insertSlots) for (auto& f : i.fx) if (f.plugin) fn (*f.plugin);
    }

    // ---- arrangement snapshot for the audio thread ----
    struct AudioClipRT
    {
        const SampleData* data = nullptr;
        int    insert = 1;
        double start = 0.0;         // beats
        double readOffset = 0.0;    // seconds into data
        double playLength = 0.0;    // output seconds
        double rate = 1.0;          // data seconds per output second
        float  gain = 1.0f;
    };
    struct NoteRT  { double on = 0.0, off = 0.0; int note = 60; juce::uint8 velocity = 100; };
    struct LaneRT
    {
        int paramIndex = 0;
        AutoLane lane;              // points in absolute beats
    };
    struct MidiClipRT
    {
        int    channel = 0;
        double start = 0.0, end = 0.0;
        std::vector<NoteRT> notes;
        std::vector<LaneRT> lanes;
    };
    struct Snapshot
    {
        std::vector<AudioClipRT> audio;
        std::vector<MidiClipRT>  midi;
        double songEnd = 0.0;
    };
    void setSnapshot (Snapshot&&);

    // ---- browser preview ----
    void preview (std::shared_ptr<SampleData> sample);
    void stopPreview() { preview (nullptr); }

    // ---- recording ----
    void startAudioRecording()                         { recorder.begin(); }
    std::optional<Recorder::Take> stopAudioRecording() { return recorder.end(); }
    bool isRecordingAudio() const noexcept             { return recorder.isActive(); }

    void startMidiRecording (int channel);
    void stopMidiRecording()                           { midiRecording.store (false); }
    bool isRecordingMidi() const noexcept              { return midiRecording.load(); }
    int  getMidiRecordChannel() const noexcept         { return midiRecordChannel.load(); }
    double getMidiRecordStartBeat() const noexcept     { return midiRecordStart.load(); }

    struct RecordedMidi  { double beat = 0.0; juce::uint8 bytes[3] {}; int size = 0; };
    struct RecordedParam { double beat = 0.0; int index = 0; float value = 0.0f; };
    void drainRecordedMidi (std::vector<RecordedMidi>& out);
    void drainRecordedParams (std::vector<RecordedParam>& out);
    void pushRecordedParam (int index, float value);   // any thread

    // ---- AudioIODeviceCallback ----
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    // Plugins with their own sequencers (drum machines, arpeggiators, loop
    // players) follow this, so it reports everything a host normally would.
    struct PlayHead : public juce::AudioPlayHead
    {
        double bpm = 128.0, ppq = 0.0, seconds = 0.0;
        juce::int64 samples = 0;
        bool playing = false, recording = false, looping = false;
        double loopStart = 0.0, loopEnd = 0.0;

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setBpm (bpm);
            info.setTimeSignature (juce::AudioPlayHead::TimeSignature {});
            info.setPpqPosition (ppq);
            info.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
            info.setTimeInSamples (samples);
            info.setTimeInSeconds (seconds);
            info.setIsPlaying (playing);
            info.setIsRecording (recording);
            info.setIsLooping (looping);
            if (looping)
                info.setLoopPoints (juce::AudioPlayHead::LoopPoints { loopStart, loopEnd });
            return info;
        }
    };

    struct FxSlot
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        juce::AudioBuffer<float> scratch;
        int channels = 2;
    };
    struct InsertSlot
    {
        std::array<FxSlot, kNumFxSlots> fx;
        juce::AudioBuffer<float> buffer;
    };
    struct ChannelSlot
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        juce::AudioBuffer<float> buffer;
        juce::MidiBuffer midi;
        int channels = 2;
        std::atomic<int> insert { 1 };
        std::atomic<bool> sumOutputs { false };
        int outputBuses = 1;
        std::vector<float> lastAuto;
    };

    void preparePlugin (juce::AudioPluginInstance&);
    static int channelsFor (const juce::AudioPluginInstance&);
    int  bufferCapacity() const noexcept { return std::max (blockSize * 2, 8192); }
    void processInsert (int index, int numSamples);
    void renderAudioClips (int numSamples);
    void scheduleMidi (int numSamples, bool sendAllOff, bool isRunning);
    void stopTrackedNotes (int slot, bool includeLive, bool includeClips);
    void releaseStaleClipNotes (double beat);
    void trackLiveMessage (int slot, const juce::MidiMessage&);
    void applyAutomation (double beat);
    void renderPreview (float* left, float* right, int numSamples);

    juce::AudioDeviceManager   deviceManager;
    juce::MidiKeyboardState    keyboardState;
    juce::MidiMessageCollector midiCollector;
    juce::MidiBuffer           liveMidi, fxMidi;
    PlayHead                   playHead;

    juce::SpinLock graphLock;
    std::array<ChannelSlot, kNumChannels>   channelSlots;
    std::array<InsertSlot,  kNumInserts>    insertSlots;
    std::array<InsertControls, kNumInserts> controls;
    Snapshot snapshot;
    int capacity = 0;

    std::shared_ptr<SampleData> previewHold;
    const SampleData* previewData = nullptr;
    double previewPos = 0.0;
    juce::SpinLock previewLock;

    Recorder recorder;

    std::atomic<bool>   midiRecording { false };
    std::atomic<int>    midiRecordChannel { 0 };
    std::atomic<double> midiRecordStart { -1.0 };
    static constexpr int recCapacity = 8192;
    std::vector<RecordedMidi>  midiRing  = std::vector<RecordedMidi> (recCapacity);
    juce::AbstractFifo         midiFifo { recCapacity };
    std::vector<RecordedParam> paramRing = std::vector<RecordedParam> (recCapacity);
    juce::AbstractFifo         paramFifo { recCapacity };
    juce::SpinLock             paramWriteLock;

    std::atomic<bool>   playing { false }, rewind { false }, metronome { true };
    std::atomic<bool>   panicRequest { false }, snapshotChanged { false };
    std::atomic<double> bpm { 128.0 }, beatPosition { 0.0 }, songStart { 0.0 }, locateRequest { -1.0 }, songEnd { 0.0 };
    std::atomic<float>  inputLevel { 0.0f };
    std::atomic<int>    selectedChannel { 0 };

    double sampleRate = 44100.0;
    int    blockSize  = 512;

    // audio-thread-only: every note the engine has started, so none can hang
    static constexpr int midiChannels = 16;
    std::vector<juce::uint8> clipNotes = std::vector<juce::uint8> ((size_t) kNumChannels * 128);
    std::vector<juce::uint8> liveNotes = std::vector<juce::uint8> ((size_t) kNumChannels * midiChannels * 128);
    int liveChannel = 0;

    double position = 0.0;
    juce::int64 lastBeat = -1;
    bool   wasRunning = false;
    std::vector<double> blockBeats;
    std::vector<float>  clickBuffer;
    int    clickSamplesLeft = 0;
    double clickPhase = 0.0, clickFreq = 1000.0, clickDecay = 0.999;
    float  clickAmp = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
