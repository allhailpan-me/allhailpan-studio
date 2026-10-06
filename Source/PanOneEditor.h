#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

#include "AhpLookAndFeel.h"
#include "PanOne.h"
#include "PanOneParams.h"

#include <cmath>
#include <memory>
#include <vector>

//==============================================================================
/** The front of PAN One.

    This exists because of a report that said more than it looks: the window
    opened black, with no controls and no way to reach the presets. The
    instrument had no parameters, so the generic editor every host falls back
    on had nothing to draw, and an empty window does not read as "no
    parameters yet". It reads as broken, and there is no reason for somebody
    to think otherwise.

    Everything here is laid out from PanOneParams, which is also what builds
    the parameters, so a knob added to that table appears here with no change
    to this file. The grouping comes from the same place, and a test checks
    that the groups cover every parameter exactly once, because the failure
    otherwise is a control that exists, works and saves but is drawn nowhere.

    The preset box is not a parameter. Presets move every knob at once, which
    is not something a host should be able to automate a smooth path through,
    and JUCE already has programs for exactly this. It is polled rather than
    listened to because a preset can also change from a project being loaded,
    and four times a second is both cheap and faster than anyone notices.
*/
class PanOneEditor final : public juce::AudioProcessorEditor,
                           private juce::Timer
{
public:
    explicit PanOneEditor (PanOne& owner)
        : juce::AudioProcessorEditor (owner), synth (owner)
    {
        addAndMakeVisible (title);
        title.setText ("PAN One", juce::dontSendNotification);
        title.setFont (juce::FontOptions (17.0f, juce::Font::bold));
        title.setColour (juce::Label::textColourId, Ahp::bone);

        addAndMakeVisible (presetLabel);
        presetLabel.setText ("Preset", juce::dontSendNotification);
        presetLabel.setFont (juce::FontOptions (12.0f));
        presetLabel.setColour (juce::Label::textColourId, Ahp::muted);
        presetLabel.setJustificationType (juce::Justification::centredRight);

        addAndMakeVisible (presetBox);

        for (int i = 0; i < synth.getNumPrograms(); ++i)
            presetBox.addItem (synth.getProgramName (i), i + 1);

        presetBox.setSelectedItemIndex (synth.getCurrentProgram(), juce::dontSendNotification);
        presetBox.onChange = [this]
        {
            const int index = presetBox.getSelectedItemIndex();

            if (index >= 0 && index != synth.getCurrentProgram())
                synth.setCurrentProgram (index);
        };

        addAndMakeVisible (edited);
        edited.setFont (juce::FontOptions (11.0f));
        edited.setColour (juce::Label::textColourId, Ahp::muted);
        edited.setJustificationType (juce::Justification::centredLeft);

        const auto& specs = PanOneParams::all();
        controls.reserve (specs.size());

        for (std::size_t i = 0; i < specs.size(); ++i)
        {
            auto control = std::make_unique<Control>();
            auto* parameter = synth.parameterAt (i);

            addAndMakeVisible (control->name);
            control->name.setText (specs[i].name, juce::dontSendNotification);
            control->name.setFont (juce::FontOptions (11.0f));
            control->name.setColour (juce::Label::textColourId, Ahp::muted);
            control->name.setJustificationType (juce::Justification::centred);

            if (specs[i].kind == PanOneParams::Kind::choice)
            {
                control->isChoice = true;
                addAndMakeVisible (control->choice);

                // Before the attachment, which maps the selected index onto
                // the parameter's range and so has to know how many there
                // are. An empty box here divides by zero items.
                for (int s = 0; s < PanOneParams::numShapes; ++s)
                    control->choice.addItem (PanOneParams::shapeNames[s], s + 1);

                if (parameter != nullptr)
                    control->choiceAttachment
                        = std::make_unique<juce::ComboBoxParameterAttachment> (*parameter,
                                                                               control->choice);
            }
            else
            {
                addAndMakeVisible (control->slider);
                control->slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
                control->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 74, 14);
                control->slider.setColour (juce::Slider::textBoxOutlineColourId,
                                           juce::Colours::transparentBlack);
                control->slider.setColour (juce::Slider::textBoxTextColourId, Ahp::bone);
                control->slider.setColour (juce::Slider::rotarySliderFillColourId, Ahp::rec);

                if (parameter != nullptr)
                    control->sliderAttachment
                        = std::make_unique<juce::SliderParameterAttachment> (*parameter,
                                                                             control->slider);
            }

            controls.push_back (std::move (control));
        }

        startTimerHz (4);
        setSize (600, 480);
    }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        g.fillAll (Ahp::panel);

        auto area = getLocalBounds().reduced (10);
        area.removeFromTop (headerHeight);

        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));

        for (const auto& group : PanOneParams::groups())
        {
            auto box = area.removeFromTop (groupHeight);
            area.removeFromTop (groupGap);

            g.setColour (Ahp::panel2);
            g.fillRoundedRectangle (box.toFloat(), 4.0f);
            g.setColour (Ahp::line);
            g.drawRoundedRectangle (box.toFloat().reduced (0.5f), 4.0f, 1.0f);

            g.setColour (Ahp::muted);
            g.drawText (juce::String (group.heading).toUpperCase(),
                        box.removeFromTop (headingHeight).reduced (10, 0),
                        juce::Justification::centredLeft);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10);

        auto header = area.removeFromTop (headerHeight);
        title.setBounds (header.removeFromLeft (110));
        presetLabel.setBounds (header.removeFromLeft (52));
        presetBox.setBounds (header.removeFromLeft (180).reduced (6, 6));
        edited.setBounds (header.reduced (8, 0));

        for (const auto& group : PanOneParams::groups())
        {
            auto box = area.removeFromTop (groupHeight);
            area.removeFromTop (groupGap);

            box.removeFromTop (headingHeight);
            box = box.reduced (6, 2);

            if (group.count == 0)
                continue;

            const int each = box.getWidth() / (int) group.count;

            for (std::size_t i = group.first;
                 i < group.first + group.count && i < controls.size();
                 ++i)
            {
                auto cell = box.removeFromLeft (each);
                auto& control = *controls[i];

                control.name.setBounds (cell.removeFromTop (14));

                if (control.isChoice)
                    control.choice.setBounds (cell.withSizeKeepingCentre (juce::jmin (84, cell.getWidth()), 24));
                else
                    control.slider.setBounds (cell);
            }
        }
    }

private:
    struct Control
    {
        juce::Label    name;
        juce::Slider   slider;
        juce::ComboBox choice;
        bool           isChoice = false;

        std::unique_ptr<juce::SliderParameterAttachment>   sliderAttachment;
        std::unique_ptr<juce::ComboBoxParameterAttachment> choiceAttachment;
    };

    void timerCallback() override
    {
        const int program = synth.getCurrentProgram();

        if (presetBox.getSelectedItemIndex() != program)
            presetBox.setSelectedItemIndex (program, juce::dontSendNotification);

        // Saying so matters more than it looks. Somebody who turns a knob and
        // then wonders why the preset no longer sounds like the preset is one
        // of the few people who will go looking for a bug that is not there.
        const bool changed = differsFromPreset (program);

        if (changed != showingEdited)
        {
            showingEdited = changed;
            edited.setText (changed ? "edited" : "", juce::dontSendNotification);
        }
    }

    bool differsFromPreset (int program) const
    {
        const auto& all = PanOneParams::presets();

        if (! juce::isPositiveAndBelow (program, (int) all.size()))
            return false;

        const auto wanted = PanOneParams::fromPatch (all[(std::size_t) program].patch);

        for (std::size_t i = 0; i < wanted.size(); ++i)
        {
            auto* parameter = synth.parameterAt (i);

            if (parameter == nullptr)
                continue;

            const float value = parameter->convertFrom0to1 (parameter->getValue());

            // Loose, because the value has been through a float parameter and
            // in the case of a skewed range through a pair of logarithms as
            // well. A thousandth of the range is far below anything audible
            // and far above what that arithmetic loses.
            const auto& range = parameter->getNormalisableRange();

            if (std::abs (value - wanted[i]) > 0.001f * (range.end - range.start))
                return true;
        }

        return false;
    }

    static constexpr int headerHeight  = 36;
    static constexpr int headingHeight = 18;
    static constexpr int groupHeight   = 100;
    static constexpr int groupGap      = 6;

    PanOne& synth;

    juce::Label    title, presetLabel, edited;
    juce::ComboBox presetBox;
    bool           showingEdited = false;

    std::vector<std::unique_ptr<Control>> controls;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PanOneEditor)
};
