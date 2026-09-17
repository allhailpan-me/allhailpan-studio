#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "AhpLookAndFeel.h"
#include "DarkTitleBar.h"

// Shows a plugin's own interface. Plugins without one get a generic slider panel.
class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioProcessor& processor, std::function<void()> onCloseRequested)
        : DocumentWindow (processor.getName(), Ahp::black,
                          DocumentWindow::minimiseButton | DocumentWindow::closeButton),
          onClose (std::move (onCloseRequested))
    {
        setUsingNativeTitleBar (true);

        juce::AudioProcessorEditor* editor = processor.hasEditor() ? processor.createEditorAndMakeActive() : nullptr;
        if (editor == nullptr)
            editor = new juce::GenericAudioProcessorEditor (processor);

        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        Ahp::applyDarkTitleBar (*this);
    }

    ~PluginWindow() override
    {
        clearContentComponent();   // the editor must go before the plugin does
    }

    void closeButtonPressed() override
    {
        if (onClose)
            onClose();
    }

private:
    std::function<void()> onClose;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
};
