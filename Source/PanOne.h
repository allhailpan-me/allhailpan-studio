#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

#include "PanOneParams.h"
#include "Synth/Voice.h"
#include "VoiceAllocator.h"

#include <array>
#include <atomic>
#include <cmath>
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
    tests: the wavetables and the oscillator that reads them, the wavefolder,
    the saturator, the state variable filter, the low pass gate, the slopes,
    the envelopes, the modulation matrix, and `Synth/Voice.h` where they meet.
    The controls and the presets are a table in PanOneParams.h, which carries
    no JUCE either and is tested the same way. This file is the part that
    cannot be tested without JUCE, so it deliberately holds as little
    thinking as possible: it is plumbing between MIDI, that table and those
    headers, and nothing else.

    It is an AudioPluginInstance rather than anything special, which is what
    makes it ordinary everywhere else. It arrives in the same channel dropdown
    as a scanned VST3, saves into a project by PluginDescription, and works
    with MIDI recording, the modulators and the mixer with no special casing
    anywhere. Its parameters are ordinary hosted parameters, so a modulator
    can be wired to its cutoff exactly as it would be to a third party synth.

    **Every choice parameter is saved by its id and not by its index.** A
    choice holds an index, and an index is exactly what must not reach a
    project file: inserting one wavetable or one modulation destination in the
    middle of a list would leave every patch anybody had ever saved pointing
    at the next one along, still loading and still playing. So the state goes
    out as strings and comes back resolved, and an id nobody recognises falls
    back to the first entry of its list, which every list is ordered to make
    safe.
*/
class PanOne final : public juce::AudioPluginInstance
{
public:
    static constexpr int numVoices = 16;

    static juce::String identifier()   { return "AHP-PAN-ONE"; }
    static juce::String displayName()  { return "PAN One"; }

    //==========================================================================
    /** The wavetables, built once and shared by every voice of every
        instance.

        Shared because they are read only once built and are the same for
        everybody, and because building them is a few hundred transforms and
        forty megabytes. Built eagerly rather than on the first patch that
        asks for a particular table: forty megabytes held from launch is
        cheap, and a pause in the middle of somebody's work is not.

        `prepareSharedResources` exists so the studio can get that out of the
        way at launch instead of on the first instrument somebody loads. It is
        not required: the accessor builds the bank if nobody has.
    */
    static const Synth::WaveBank& bank()
    {
        // A function local static, so the build happens once however many
        // instances exist and whichever thread arrives first.
        static const Synth::WaveBank built = []
        {
            Synth::WaveBank b;
            b.buildAll();
            return b;
        }();

        return built;
    }

    static void prepareSharedResources() { (void) bank(); }

    //==========================================================================
    PanOne()
        : juce::AudioPluginInstance (BusesProperties()
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
        // Built from the table rather than written out, so that a control
        // added there needs nothing adding here. The first preset supplies
        // the defaults, which is what an instrument should sound like before
        // anybody has touched it.
        const auto defaults = PanOneParams::fromPatch (PanOneParams::presets().front().patch);
        const auto& specs = PanOneParams::all();

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
                // Each choice draws its own names from its own list. One
                // shared list was enough when the only choices were the four
                // oscillator shapes, and would now put wavetable names in
                // the filter's menu.
                juce::StringArray names;

                for (const auto& choice : spec.choices())
                    names.add (choice.name);

                auto parameter = std::make_unique<juce::AudioParameterChoice>
                                   (id, spec.name, names, (int) defaults[i]);

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
        // sample rate has to reach every filter, envelope and oversampler
        // before a note arrives, and processBlock must never be the place
        // that notices.
        const auto patch = currentPatch();

        for (auto& v : voices)
        {
            v.setSampleRate (sampleRate);
            v.setBank (&bank());
            v.setPatch (patch);
        }

        allocator.reset();

        applied = PanOneParams::fromPatch (patch);
        scratch.resize (applied.size());

        // The voice runs its folder, its drive and its filter at four times
        // the sample rate, and the filters that get it there and back delay
        // everything through them. An instrument has no dry path of its own
        // to drift against, so this is not a correctness problem the way it
        // is for an effect, but it is the delay between a note arriving and
        // the first sample of it leaving, and the studio can only take it off
        // everything else if it is told.
        setLatencySamples ((int) std::lround (voices.front().latencySamples()));
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
    double getTailLengthSeconds() const override  { return 12.0; }
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
        it from. Choosing a preset moves every control to it, through
        setValueNotifyingHost, so an open editor follows and the host records
        the change rather than finding the values different later. */
    void setCurrentProgram (int index) override
    {
        const auto& all = PanOneParams::presets();

        if (all.empty())
            return;

        const int chosen = juce::jlimit (0, (int) all.size() - 1, index);

        currentProgram.store (chosen);

        const auto values = PanOneParams::fromPatch (all[(std::size_t) chosen].patch);

        for (std::size_t i = 0; i < values.size() && i < parameters.size(); ++i)
            setParameterValue (i, values[i]);
    }

    const juce::String getProgramName (int index) override
    {
        const auto& all = PanOneParams::presets();

        if (! juce::isPositiveAndBelow (index, (int) all.size()))
            return {};

        // The category is in the name because a host's program menu is one
        // flat list, so without it a bank of twenty two is a wall of words
        // with no order anybody can see.
        const auto& preset = all[(std::size_t) index];

        return juce::String (preset.category) + ": " + preset.name;
    }

    void changeProgramName (int, const juce::String&) override {}

    //==========================================================================
    /** The controls by name, not by position, because a later version that
        adds one in the middle must not shift every saved value along by one.

        A choice goes out as the id of the entry it is on rather than as the
        index of it. That is the whole reason this instrument's lists can be
        reordered, extended or have entries removed from them without
        breaking a project, and it is the discipline `Synth/ModMatrix.h` and
        `Synth/WaveBank.h` are both built on.
    */
    void getStateInformation (juce::MemoryBlock& destination) override
    {
        juce::XmlElement state ("PanOne");
        state.setAttribute ("program", currentProgram.load());

        const auto& specs = PanOneParams::all();

        for (std::size_t i = 0; i < specs.size() && i < parameters.size(); ++i)
        {
            if (specs[i].choices != nullptr)
                state.setAttribute (specs[i].id,
                                    juce::String (PanOneParams::idOfChoice
                                                    (specs[i].choices(),
                                                     (int) parameterValue (i))));
            else
                state.setAttribute (specs[i].id, (double) parameterValue (i));
        }

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
        // what makes a project from before the controls existed load
        // correctly: it carries a program and nothing else, and has to come
        // back as that preset rather than as a default patch.
        setCurrentProgram (xml->getIntAttribute ("program", 0));

        const auto& specs = PanOneParams::all();

        for (std::size_t i = 0; i < specs.size() && i < parameters.size(); ++i)
        {
            if (! xml->hasAttribute (specs[i].id))
                continue;

            if (specs[i].choices == nullptr)
            {
                setParameterValue (i, (float) xml->getDoubleAttribute (specs[i].id));
                continue;
            }

            // A choice saved by id. Version one wrote numbers here, so a
            // value that is not a name it knows but is a number is read as
            // one: `getIntAttribute` on a string that is not a number gives
            // zero, which is the first entry, which is the fallback this
            // wants anyway.
            const auto& list = specs[i].choices();
            const auto saved = xml->getStringAttribute (specs[i].id);

            const int found = PanOneParams::choiceFromId (list, saved.toRawUTF8());

            setParameterValue (i, (float) (found >= 0
                                             ? found
                                             : juce::jlimit (0, (int) list.size() - 1,
                                                             saved.getIntValue())));
        }

        applyAnythingVersionOneSaved (*xml);
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
        d.descriptiveName   = "Wavetables, a wavefolder and a low pass gate, "
                              "wired through a modulation matrix";
        d.pluginFormatName  = "Built in";
        d.category          = "Synth";
        d.manufacturerName  = "ALLHAILPAN";
        d.version           = "2.0.0";
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
    /** Translates the four controls version one had that this version does
        not, if the state came from version one at all.

        Done after the saved values have been written, because the
        translation reads the patch those values make and adds to it. A
        project from version one is recognised by it carrying an oscillator
        shape, which nothing since has written.

        Without this a project opens with the right envelope times and
        entirely the wrong sound, which is the quiet kind of wrong this
        codebase keeps meeting. The arithmetic is in PanOneParams, with no
        JUCE in it, and is checked by playing each of version one's four
        shapes and measuring the harmonics that come out.
    */
    void applyAnythingVersionOneSaved (const juce::XmlElement& xml)
    {
        if (! xml.hasAttribute ("osc1shape"))
            return;

        PanOneParams::Legacy old;
        old.present = true;
        old.osc1Shape = xml.getIntAttribute ("osc1shape", 1);
        old.osc2Shape = xml.getIntAttribute ("osc2shape", 1);
        old.pulseWidth = (float) xml.getDoubleAttribute ("pulsewidth", 0.5);
        old.envOctaves = (float) xml.getDoubleAttribute ("envoctaves", 0.0);

        auto patch = currentPatch();
        PanOneParams::applyLegacy (old, patch);

        const auto values = PanOneParams::fromPatch (patch);

        for (std::size_t i = 0; i < values.size() && i < parameters.size(); ++i)
            setParameterValue (i, values[i]);
    }

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

    /** Called at the top of every block. Reading a couple of hundred atomics
        and a handful of skew curves costs nothing next to a block of audio,
        and pushing the patch into the voices is only done when something
        actually moved, since that part recomputes filter coefficients,
        envelope rates and slope increments sixteen times over.

        Nothing here allocates: both vectors were sized once in the
        constructor and only have their contents overwritten. */
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

    std::array<Synth::Voice, (std::size_t) numVoices> voices;
    VoiceAllocator<numVoices> allocator;

    std::vector<juce::RangedAudioParameter*> parameters;   // owned by the base class
    std::atomic<int> currentProgram { 0 };

    // Audio thread only, both of them, and both sized before a block ever
    // runs so that comparing them cannot allocate.
    std::vector<float> applied, scratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PanOne)
};
