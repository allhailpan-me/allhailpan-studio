#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "BinaryData.h"

namespace AhpImages
{
    inline juce::Image watermark()
    {
        return juce::ImageCache::getFromMemory (BinaryData::ahp_logo_png, BinaryData::ahp_logo_pngSize);
    }

    inline juce::Image spinSheet()
    {
        return juce::ImageCache::getFromMemory (BinaryData::logo_spin_png, BinaryData::logo_spin_pngSize);
    }
}

// The spinning ALLHAILPAN mark: 120 frames in a 12 x 10 sprite sheet.
class SpinningLogo : public juce::Component,
                     private juce::Timer
{
public:
    SpinningLogo()
    {
        sheet = AhpImages::spinSheet();
        setInterceptsMouseClicks (false, false);
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        if (! sheet.isValid())
            return;

        const auto area  = getLocalBounds().toFloat();
        const float scale = std::min (area.getWidth() / frameW, area.getHeight() / frameH);
        const int   w = juce::roundToInt (frameW * scale), h = juce::roundToInt (frameH * scale);
        const int   x = juce::roundToInt (area.getCentreX() - w / 2.0f);
        const int   y = juce::roundToInt (area.getCentreY() - h / 2.0f);

        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.drawImage (sheet, x, y, w, h,
                     (frame % columns) * frameW, (frame / columns) * frameH, frameW, frameH);
    }

private:
    void timerCallback() override
    {
        frame = (frame + 1) % numFrames;
        repaint();
    }

    static constexpr int frameW = 76, frameH = 96, columns = 12, numFrames = 120;
    juce::Image sheet;
    int frame = 0;
};
