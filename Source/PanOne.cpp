#include "PanOne.h"
#include "PanOneEditor.h"

// The only place that knows both the instrument and its front panel. PanOne.h
// is included by the plugin format, by the engine and by anything that loads a
// project, none of which need the editor, and the editor needs the instrument.
// Defining this one function here is what keeps that one way round.
juce::AudioProcessorEditor* PanOne::createEditor()
{
    return new PanOneEditor (*this);
}
