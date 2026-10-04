#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

#include "SynthVoice.h"
#include "VoiceAllocator.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>

//==============================================================================
/** PAN One: the instrument that is built in, so a fresh install makes sound.

    Until this existed the studio was silent on a new machine until someone
    supplied their own VST3s, which is the point where most people who try a
    studio stop trying it. Bundling other people's plugins is not an answer:
    almost every free one forbids redistribution, and the ones that allow it
    are large binaries that do not belong in a source repository.

    The synthesis is all in headers that carry no JUCE and have their own
    tests: Oscillator.h, Envelope.h, Filter.h, SynthVoice.h and
    VoiceAllocator.h. This file is the part that cannot be tested without
    JUCE, so it deliberately holds as little thinking as possible: it is
    plumbing between MIDI and those headers, and nothing else.

    It is an AudioPluginInstance rather than anything special, which is what
    makes it ordinary everywhere else. It arrives in the same channel
    dropdown as a scanned VST3, saves into a project by PluginDescription,
    and works with MIDI recording, the modulators and the mixer with no
    special casing anywhere.
*/
class PanOne final : public juce::AudioPluginInstance
{
public:
    static constexpr int numVoices = 16;

    static juce::String identifier()   { return "AHP-PAN-ONE"; }
    static juce::String displayName()  { return "PAN One"; }

    PanOne()
        : juce::AudioPluginInstance (BusesProperties()
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
    }

    // Worth saying plainly: this exposes no parameters yet, only presets.
    // That means the modulators cannot reach it, because they drive plugin
    // parameters, and the generic editor it falls back on has nothing to
    // show. Adding them is not a line or two: an AudioPluginInstance may
    // only carry HostedParameters, not plain ones, which the base class
    // enforces by hiding addParameter. It is the obvious next step and it
    // is deliberately not being half done here.

    //==========================================================================
    const juce::String getName() const override   { return displayName(); }

    void prepareToPlay (double sampleRate, int) override
    {
        // Everything that needs memory happens here. The voice array is a
        // fixed member so there is nothing to allocate even now, but the
        // sample rate has to reach every filter and envelope before a note
        // arrives, and processBlock must never be the place that notices.
        for (auto& v : voices)
        {
            v.setSampleRate (sampleRate);
            v.setPatch (presets[(std::size_t) currentProgram.load()].patch);
        }

        allocator.reset();
        appliedProgram = currentProgram.load();
    }

    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals noDenormals;

        buffer.clear();

        // A preset change arrives from the message thread as a number. It is
        // picked up here rather than applied there, because applying it
        // writes to every voice and those are being read by this thread.
        // Nothing below allocates, so doing it at the top of a block costs
        // nothing worth measuring.
        if (const int wanted = currentProgram.load(); wanted != appliedProgram)
        {
            appliedProgram = wanted;

            for (auto& v : voices)
                v.setPatch (presets[(std::size_t) wanted].patch);
        }

        const int numSamples = buffer.getNumSamples();
        int position = 0;

        for (const auto metadata : midi)
        {
            const int when = juce::jlimit (0, numSamples, metadata.samplePosition);

            render (buffer, position, when - position);
            position = when;

            handle (metadata.getMessage());
        }

        render (buffer, position, numSamples - position);
    }

    //==========================================================================
    double getTailLengthSeconds() const override  { return 2.0; }
    bool acceptsMidi() const override             { return true; }
    bool producesMidi() const override            { return false; }
    bool hasEditor() const override               { return false; }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }

    //==========================================================================
    int getNumPrograms() override                 { return (int) presets.size(); }
    int getCurrentProgram() override              { return currentProgram.load(); }

    void setCurrentProgram (int index) override
    {
        currentProgram.store (juce::jlimit (0, (int) presets.size() - 1, index));
    }

    const juce::String getProgramName (int index) override
    {
        return juce::isPositiveAndBelow (index, (int) presets.size())
                 ? presets[(std::size_t) index].name : juce::String();
    }

    void changeProgramName (int, const juce::String&) override {}

    //==========================================================================
    /** Only the chosen preset is saved. The patch itself is not editable yet,
        so storing its values would be storing a copy of a constant, and a
        project saved today would then ignore any later improvement to the
        preset it names. When the parameters become editable this has to
        store them too. */
    void getStateInformation (juce::MemoryBlock& destination) override
    {
        juce::XmlElement state ("PanOne");
        state.setAttribute ("program", currentProgram.load());
        copyXmlToBinary (state, destination);
    }

    void setStateInformation (const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary (data, size))
            if (xml->hasTagName ("PanOne"))
                setCurrentProgram (xml->getIntAttribute ("program", 0));
    }

    //==========================================================================
    void fillInPluginDescription (juce::PluginDescription& description) const override
    {
        description = describe();
    }

    /** The same description the format hands to the plugin list, so a
        project saved with this instrument finds it again by identifier. */
    static juce::PluginDescription describe()
    {
        juce::PluginDescription d;
        d.name              = displayName();
        d.descriptiveName   = "Two oscillators, a sub and a resonant filter";
        d.pluginFormatName  = "Built in";
        d.category          = "Synth";
        d.manufacturerName  = "ALLHAILPAN";
        d.version           = "1.0.0";
        d.fileOrIdentifier  = identifier();
        d.uniqueId          = 0x41484F31;          // "AHO1"
        d.deprecatedUid     = d.uniqueId;
        d.isInstrument      = true;
        d.numInputChannels  = 0;
        d.numOutputChannels = 2;
        return d;
    }

private:
    struct Preset
    {
        juce::String name;
        SynthVoice::Patch patch;
    };

    void handle (const juce::MidiMessage& message)
    {
        if (message.isNoteOn())
        {
            int stolen = -1;
            const int index = allocator.noteOn (message.getNoteNumber(), stolen);

            if (index >= 0)
                voices[(std::size_t) index].start (message.getNoteNumber(),
                                                   message.getFloatVelocity());
        }
        else if (message.isNoteOff())
        {
            releaseEvery (message.getNoteNumber());
        }
        else if (message.isAllNotesOff() || message.isAllSoundOff())
        {
            allocator.allNotesOff();

            for (auto& v : voices)
                v.stop();
        }
    }

    void releaseEvery (int note)
    {
        // The allocator can legitimately have the same note on more than one
        // voice after a steal, so every one of them has to be let go.
        for (int i = 0; i < numVoices; ++i)
            if (allocator.voice (i).held && allocator.voice (i).note == note)
                voices[(std::size_t) i].stop();

        allocator.noteOff (note);
    }

    void render (juce::AudioBuffer<float>& buffer, int start, int count)
    {
        if (count <= 0)
            return;

        auto* left  = buffer.getWritePointer (0, start);
        auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1, start) : nullptr;

        for (int i = 0; i < numVoices; ++i)
        {
            auto& voice = voices[(std::size_t) i];

            if (! voice.isActive())
            {
                // A voice that finished during the last block is handed back
                // here rather than inside the loop below, so the allocator is
                // only touched once per voice per block.
                if (allocator.voice (i).sounding)
                    allocator.voiceFinished (i);

                continue;
            }

            for (int n = 0; n < count; ++n)
            {
                const float s = voice.nextSample() * 0.25f;   // room for sixteen of them
                left[n] += s;

                if (right != nullptr)
                    right[n] += s;
            }
        }
    }

    static std::array<Preset, 5> makePresets()
    {
        using Shape = Oscillator::Shape;

        SynthVoice::Patch bass;
        bass.osc1Shape = Shape::saw;   bass.osc2Shape = Shape::square;
        bass.osc2Detune = 0.04;        bass.subLevel = 0.45;
        bass.cutoff = 240.0;           bass.resonance = 2.2;  bass.envOctaves = 2.6;
        bass.amp    = { 0.002, 0.200, 0.55, 0.120 };
        bass.filter = { 0.001, 0.140, 0.18, 0.120 };

        SynthVoice::Patch keys;
        keys.osc1Shape = Shape::saw;   keys.osc2Shape = Shape::saw;
        keys.osc2Detune = 0.11;        keys.subLevel = 0.18;
        keys.cutoff = 1100.0;          keys.resonance = 1.1;  keys.envOctaves = 2.2;
        keys.amp    = { 0.010, 0.400, 0.70, 0.350 };
        keys.filter = { 0.020, 0.500, 0.40, 0.300 };

        SynthVoice::Patch pad;
        pad.osc1Shape = Shape::saw;    pad.osc2Shape = Shape::triangle;
        pad.osc2Detune = 0.19;         pad.subLevel = 0.22;
        pad.cutoff = 500.0;            pad.resonance = 0.9;   pad.envOctaves = 2.8;
        pad.amp    = { 0.450, 1.000, 0.85, 0.900 };
        pad.filter = { 0.700, 1.400, 0.55, 0.800 };

        SynthVoice::Patch pluck;
        pluck.osc1Shape = Shape::square; pluck.osc2Shape = Shape::saw;
        pluck.osc2Detune = 0.07;         pluck.subLevel = 0.12;
        pluck.pulseWidth = 0.32;
        pluck.cutoff = 420.0;            pluck.resonance = 3.4; pluck.envOctaves = 4.0;
        pluck.amp    = { 0.001, 0.260, 0.00, 0.180 };
        pluck.filter = { 0.001, 0.160, 0.00, 0.150 };

        SynthVoice::Patch lead;
        lead.osc1Shape = Shape::square; lead.osc2Shape = Shape::saw;
        lead.osc2Detune = 0.05;         lead.subLevel = 0.30;
        lead.pulseWidth = 0.42;
        lead.cutoff = 900.0;            lead.resonance = 2.8;  lead.envOctaves = 3.2;
        lead.amp    = { 0.004, 0.300, 0.80, 0.160 };
        lead.filter = { 0.003, 0.240, 0.45, 0.180 };

        return { Preset { "Sub Bass", bass },
                 Preset { "Soft Keys", keys },
                 Preset { "Slow Pad", pad },
                 Preset { "Short Pluck", pluck },
                 Preset { "Reed Lead", lead } };
    }

    std::array<SynthVoice, (std::size_t) numVoices> voices;
    VoiceAllocator<numVoices> allocator;

    const std::array<Preset, 5> presets = makePresets();
    std::atomic<int> currentProgram { 0 };
    int appliedProgram = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PanOne)
};
