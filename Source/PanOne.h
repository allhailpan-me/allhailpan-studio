#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

#include "PanOneParams.h"
#include "SynthVoice.h"
#include "VoiceAllocator.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

//==============================================================================
/** PAN One: the instrument that is built in, so a fresh install makes sound.

    Until this existed the studio was silent on a new machine until someone
    supplied their own VST3s, which is the point where most people who try a
    studio stop trying it. Bundling other people's plugins is not an answer:
    almost every free one forbids redistribution, and the ones that allow it
    are large binaries that do not belong in a source repository.

    The synthesis is all in headers that carry no JUCE and have their own
    tests: Oscillator.h, Envelope.h, Filter.h, SynthVoice.h and
    VoiceAllocator.h. The knobs and the presets are a table in PanOneParams.h,
    which carries no JUCE either and is tested the same way. This file is the
    part that cannot be tested without JUCE, so it deliberately holds as
    little thinking as possible: it is plumbing between MIDI, that table and
    those headers, and nothing else.

    It is an AudioPluginInstance rather than anything special, which is what
    makes it ordinary everywhere else. It arrives in the same channel dropdown
    as a scanned VST3, saves into a project by PluginDescription, and works
    with MIDI recording, the modulators and the mixer with no special casing
    anywhere. Its parameters are ordinary hosted parameters, so a modulator
    can be wired to its cutoff exactly as it would be to a third party synth.
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
        // Built from the table rather than written out, so that a knob added
        // there needs nothing adding here. The first preset supplies the
        // defaults, which is what an instrument should sound like before
        // anybody has touched it.
        const auto defaults = PanOneParams::fromPatch (PanOneParams::presets().front().patch);
        const auto& specs = PanOneParams::all();

        juce::StringArray shapes;

        for (int i = 0; i < PanOneParams::numShapes; ++i)
            shapes.add (PanOneParams::shapeNames[i]);

        parameters.reserve (specs.size());

        // Sized here and never resized again, so that comparing them on the
        // audio thread cannot allocate.
        applied.assign (specs.size(), 0.0f);
        scratch.assign (specs.size(), 0.0f);

        for (std::size_t i = 0; i < specs.size(); ++i)
        {
            const auto& spec = specs[i];

            // Version one of every id. A later version that renames one has
            // to leave the old id reachable or every project that used it
            // loads a default in its place.
            const juce::ParameterID id { spec.id, 1 };

            if (spec.kind == PanOneParams::Kind::choice)
            {
                auto parameter = std::make_unique<juce::AudioParameterChoice>
                                   (id, spec.name, shapes, (int) defaults[i]);

                parameters.push_back (parameter.get());
                addHostedParameter (std::move (parameter));
            }
            else
            {
                juce::NormalisableRange<float> range (spec.minimum, spec.maximum);

                // JUCE works the skew out from where the middle of the
                // control should land, which is the honest way to say that a
                // cutoff knob has to travel logarithmically.
                range.setSkewForCentre (spec.centre);

                auto attributes = juce::AudioParameterFloatAttributes()
                                    .withLabel (spec.unit);

                auto parameter = std::make_unique<juce::AudioParameterFloat>
                                   (id, spec.name, range, defaults[i], attributes);

                parameters.push_back (parameter.get());
                addHostedParameter (std::move (parameter));
            }
        }
    }

    //==========================================================================
    const juce::String getName() const override   { return displayName(); }

    void prepareToPlay (double sampleRate, int) override
    {
        // Everything that needs memory happens here. The voice array is a
        // fixed member so there is nothing to allocate even now, but the
        // sample rate has to reach every filter and envelope before a note
        // arrives, and processBlock must never be the place that notices.
        const auto patch = currentPatch();

        for (auto& v : voices)
        {
            v.setSampleRate (sampleRate);
            v.setPatch (patch);
        }

        allocator.reset();

        applied = PanOneParams::fromPatch (patch);
        scratch.resize (applied.size());
    }

    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals noDenormals;

        buffer.clear();

        refreshPatchIfChanged();

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
    bool hasEditor() const override               { return true; }

    /** Defined in PanOne.cpp, which is the only place that knows the editor
        exists. Keeping it out of here is what stops the editor and the
        instrument including each other. */
    juce::AudioProcessorEditor* createEditor() override;

    //==========================================================================
    int getNumPrograms() override                 { return (int) PanOneParams::presets().size(); }
    int getCurrentProgram() override              { return currentProgram.load(); }

    /** Message thread only, which is where both a host and this studio call
        it from. Choosing a preset moves every knob to it, through
        setValueNotifyingHost, so an open editor follows and the host records
        the change rather than finding the values different later. */
    void setCurrentProgram (int index) override
    {
        const auto& all = PanOneParams::presets();
        const int chosen = juce::jlimit (0, (int) all.size() - 1, index);

        currentProgram.store (chosen);

        const auto values = PanOneParams::fromPatch (all[(std::size_t) chosen].patch);

        for (std::size_t i = 0; i < values.size() && i < parameters.size(); ++i)
            setParameterValue (i, values[i]);
    }

    const juce::String getProgramName (int index) override
    {
        const auto& all = PanOneParams::presets();

        return juce::isPositiveAndBelow (index, (int) all.size())
                 ? juce::String (all[(std::size_t) index].name) : juce::String();
    }

    void changeProgramName (int, const juce::String&) override {}

    //==========================================================================
    /** The knobs by name, not by position, because a later version that adds
        one in the middle must not shift every saved value along by one. */
    void getStateInformation (juce::MemoryBlock& destination) override
    {
        juce::XmlElement state ("PanOne");
        state.setAttribute ("program", currentProgram.load());

        const auto& specs = PanOneParams::all();

        for (std::size_t i = 0; i < specs.size() && i < parameters.size(); ++i)
            state.setAttribute (specs[i].id, (double) parameterValue (i));

        copyXmlToBinary (state, destination);
    }

    void setStateInformation (const void* data, int size) override
    {
        auto xml = getXmlFromBinary (data, size);

        if (xml == nullptr || ! xml->hasTagName ("PanOne"))
            return;

        // Message thread, like setCurrentProgram below it: this studio loads
        // projects there, and a host that called it from anywhere else would
        // be writing parameters from under the audio thread's feet.
        //
        // The preset first, then the saved values over the top. That order is
        // what makes a project from before the knobs existed load correctly:
        // it carries a program and nothing else, and has to come back as that
        // preset rather than as a default patch.
        setCurrentProgram (xml->getIntAttribute ("program", 0));

        const auto& specs = PanOneParams::all();

        for (std::size_t i = 0; i < specs.size() && i < parameters.size(); ++i)
            if (xml->hasAttribute (specs[i].id))
                setParameterValue (i, (float) xml->getDoubleAttribute (specs[i].id));
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

    //==========================================================================
    /** For the editor, which lays itself out from the same table. */
    juce::RangedAudioParameter* parameterAt (std::size_t index) const
    {
        return index < parameters.size() ? parameters[index] : nullptr;
    }

private:
    float parameterValue (std::size_t index) const
    {
        return parameters[index]->convertFrom0to1 (parameters[index]->getValue());
    }

    void setParameterValue (std::size_t index, float newValue)
    {
        parameters[index]->setValueNotifyingHost (parameters[index]->convertTo0to1 (newValue));
    }

    PanOneParams::Patch currentPatch() const
    {
        PanOneParams::Patch patch;
        PanOneParams::toPatch (readParameters(), patch);
        return patch;
    }

    std::vector<float> readParameters() const
    {
        std::vector<float> values;
        values.reserve (parameters.size());

        for (std::size_t i = 0; i < parameters.size(); ++i)
            values.push_back (parameterValue (i));

        return values;
    }

    /** Called at the top of every block. Reading eighteen atomics and a
        handful of skew curves costs nothing next to a block of audio, and
        pushing the patch into the voices is only done when something actually
        moved, since that part recomputes filter coefficients and envelope
        rates sixteen times over.

        Nothing here allocates: both vectors were sized once in prepareToPlay
        and only have their contents overwritten. */
    void refreshPatchIfChanged()
    {
        for (std::size_t i = 0; i < parameters.size(); ++i)
            scratch[i] = parameterValue (i);

        if (scratch == applied)
            return;

        applied = scratch;

        PanOneParams::Patch patch;
        PanOneParams::toPatch (applied, patch);

        for (auto& v : voices)
            v.setPatch (patch);
    }

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

    std::array<SynthVoice, (std::size_t) numVoices> voices;
    VoiceAllocator<numVoices> allocator;

    std::vector<juce::RangedAudioParameter*> parameters;   // owned by the base class
    std::atomic<int> currentProgram { 0 };

    // Audio thread only, both of them, and both sized before a block ever
    // runs so that comparing them cannot allocate.
    std::vector<float> applied, scratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PanOne)
};
