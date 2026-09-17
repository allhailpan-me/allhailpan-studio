#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

// ALLHAILPAN palette: true black, bone white, red only for recording.
namespace Ahp
{
    const juce::Colour black  { 0xff000000 };
    const juce::Colour panel  { 0xff0e0e0e };
    const juce::Colour panel2 { 0xff171717 };
    const juce::Colour panel3 { 0xff222222 };
    const juce::Colour line   { 0xff2c2c2c };
    const juce::Colour bone   { 0xffedebe6 };
    const juce::Colour muted  { 0xff8b8a85 };
    const juce::Colour rec    { 0xffe4432d };
}

class AhpLookAndFeel : public juce::LookAndFeel_V4
{
public:
    AhpLookAndFeel()
    {
        setColourScheme ({ Ahp::black, Ahp::panel2, Ahp::panel, Ahp::line, Ahp::bone,
                           Ahp::panel3, Ahp::black, Ahp::bone, Ahp::bone });

        setColour (juce::ResizableWindow::backgroundColourId, Ahp::black);
        setColour (juce::DocumentWindow::backgroundColourId,  Ahp::black);
        setColour (juce::TextButton::buttonColourId,          Ahp::panel2);
        setColour (juce::TextButton::buttonOnColourId,        Ahp::bone);
        setColour (juce::TextButton::textColourOffId,         Ahp::bone);
        setColour (juce::TextButton::textColourOnId,          Ahp::black);
        setColour (juce::Slider::backgroundColourId,          Ahp::panel2);
        setColour (juce::Slider::trackColourId,               Ahp::panel3);
        setColour (juce::Slider::thumbColourId,               Ahp::bone);
        setColour (juce::Slider::textBoxTextColourId,         Ahp::bone);
        setColour (juce::Slider::textBoxOutlineColourId,      Ahp::line);
        setColour (juce::Label::textColourId,                 Ahp::bone);
        setColour (juce::ComboBox::backgroundColourId,        Ahp::panel2);
        setColour (juce::ComboBox::outlineColourId,           Ahp::line);
        setColour (juce::ListBox::backgroundColourId,         Ahp::panel);
        setColour (juce::ComboBox::textColourId,              Ahp::bone);
        setColour (juce::ComboBox::arrowColourId,             Ahp::muted);
        setColour (juce::PopupMenu::backgroundColourId,       Ahp::panel2);
        setColour (juce::PopupMenu::textColourId,             Ahp::bone);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, Ahp::bone);
        setColour (juce::PopupMenu::highlightedTextColourId,  Ahp::black);
        setColour (juce::TableHeaderComponent::backgroundColourId, Ahp::panel2);
        setColour (juce::TableHeaderComponent::textColourId,       Ahp::bone);
        setColour (juce::TableHeaderComponent::outlineColourId,    Ahp::line);
        setColour (juce::TableHeaderComponent::highlightColourId,  Ahp::panel3);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&,
                               bool highlighted, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        auto fill = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId) : Ahp::panel2;
        if (down)
            fill = fill.brighter (0.08f);

        g.setColour (fill);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (highlighted || b.getToggleState() ? Ahp::muted : Ahp::line);
        g.drawRoundedRectangle (r, 3.0f, 1.0f);

        if (! b.isEnabled())
        {
            g.setColour (Ahp::black.withAlpha (0.5f));
            g.fillRoundedRectangle (r, 3.0f);
        }
    }
};
