#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "AhpLookAndFeel.h"

// A small search window for picking a plugin: type to filter, Up/Down to move,
// Enter or double-click to load.
class PluginPicker : public juce::Component,
                     private juce::ListBoxModel,
                     private juce::KeyListener
{
public:
    using Chosen = std::function<void (const juce::PluginDescription&)>;

    static void show (const juce::String& title,
                      const juce::Array<juce::PluginDescription>& available,
                      Chosen onChosen)
    {
        auto* picker = new PluginPicker (available, std::move (onChosen));
        picker->setSize (540, 470);

        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned (picker);
        options.dialogTitle                  = title;
        options.dialogBackgroundColour       = Ahp::panel;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar            = true;
        options.resizable                    = true;
        options.launchAsync();

        juce::Timer::callAfterDelay (60, [safe = juce::Component::SafePointer<PluginPicker> (picker)]
        {
            if (safe != nullptr)
                safe->search.grabKeyboardFocus();
        });
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        search.setBounds (r.removeFromTop (34));
        r.removeFromTop (8);
        hint.setBounds (r.removeFromBottom (20));
        r.removeFromBottom (6);
        list.setBounds (r);
    }

    void paint (juce::Graphics& g) override { g.fillAll (Ahp::panel); }

private:
    PluginPicker (const juce::Array<juce::PluginDescription>& available, Chosen chosen)
        : all (available), onChosen (std::move (chosen))
    {
        search.setTextToShowWhenEmpty ("Type to search effects...", Ahp::muted);
        search.setFont (juce::FontOptions (15.0f));
        search.setColour (juce::TextEditor::backgroundColourId, Ahp::panel2);
        search.setColour (juce::TextEditor::outlineColourId, Ahp::line);
        search.setColour (juce::TextEditor::focusedOutlineColourId, Ahp::bone.withAlpha (0.6f));
        search.onTextChange = [this] { rebuild(); };
        search.onReturnKey  = [this] { choose (list.getSelectedRow()); };
        search.addKeyListener (this);
        addAndMakeVisible (search);

        list.setModel (this);
        list.setRowHeight (30);
        list.setColour (juce::ListBox::backgroundColourId, Ahp::panel2);
        list.setColour (juce::ListBox::outlineColourId, Ahp::line);
        list.setOutlineThickness (1);
        addAndMakeVisible (list);

        hint.setText ("Enter loads the highlighted plugin. Esc closes.", juce::dontSendNotification);
        hint.setColour (juce::Label::textColourId, Ahp::muted);
        hint.setFont (juce::FontOptions (11.5f));
        addAndMakeVisible (hint);

        rebuild();
    }

    void rebuild()
    {
        const auto words = juce::StringArray::fromTokens (search.getText().toLowerCase().trim(), " ", {});
        filtered.clearQuick();

        for (const auto& d : all)
        {
            const auto haystack = (d.name + " " + d.manufacturerName + " " + d.pluginFormatName + " " + d.category).toLowerCase();
            bool matches = true;
            for (const auto& w : words)
                matches = matches && (w.isEmpty() || haystack.contains (w));
            if (matches)
                filtered.add (d);
        }

        list.updateContent();
        list.selectRow (filtered.isEmpty() ? -1 : 0);
        list.repaint();
    }

    int getNumRows() override { return filtered.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, filtered.size()))
            return;
        const auto& d = filtered.getReference (row);

        if (selected)
        {
            g.setColour (Ahp::bone);
            g.fillRect (0, 0, width, height);
        }
        g.setColour (Ahp::line);
        g.fillRect (0, height - 1, width, 1);

        g.setColour (selected ? Ahp::black : Ahp::bone);
        g.setFont (juce::FontOptions (13.5f));
        g.drawText (d.name, 10, 0, width - 180, height, juce::Justification::centredLeft);

        g.setColour (selected ? Ahp::black.withAlpha (0.7f) : Ahp::muted);
        g.setFont (juce::FontOptions (11.5f));
        g.drawText (d.manufacturerName + "   " + d.pluginFormatName, width - 190, 0, 180, height, juce::Justification::centredRight);
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { choose (row); }
    void returnKeyPressed (int row) override { choose (row); }

    bool keyPressed (const juce::KeyPress& key, juce::Component*) override
    {
        const int rows = filtered.size();
        if (rows == 0)
            return false;

        if (key.getKeyCode() == juce::KeyPress::upKey || key.getKeyCode() == juce::KeyPress::downKey)
        {
            const int step = key.getKeyCode() == juce::KeyPress::downKey ? 1 : -1;
            const int row  = juce::jlimit (0, rows - 1, std::max (0, list.getSelectedRow()) + step);
            list.selectRow (row);
            return true;
        }
        return false;
    }

    void choose (int row)
    {
        if (! juce::isPositiveAndBelow (row, filtered.size()))
            return;

        const auto description = filtered.getReference (row);
        auto callback = onChosen;

        if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>())
            dialog->exitModalState (1);

        if (callback)
            juce::MessageManager::callAsync ([callback, description] { callback (description); });
    }

    juce::Array<juce::PluginDescription> all, filtered;
    Chosen onChosen;
    juce::TextEditor search;
    juce::ListBox    list;
    juce::Label      hint;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginPicker)
};
