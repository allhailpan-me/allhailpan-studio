#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "AudioEngine.h"
#include "AhpLookAndFeel.h"

//==============================================================================
/** The mix report.

    Not a one knob that claims to master a track. It measures what actually
    leaves the studio and says plainly what is true about it, with the numbers
    that mastering engineers and streaming services use. A plugin cannot do
    this honestly, because a plugin only hears its own insert; the studio hears
    the finished thing.
*/
class MixReportPanel  : public juce::Component,
                        private juce::Timer
{
public:
    explicit MixReportPanel (AudioEngine& e) : engine (e)
    {
        addAndMakeVisible (title);
        title.setText ("Mix report", juce::dontSendNotification);
        title.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        title.setColour (juce::Label::textColourId, Ahp::bone);

        addAndMakeVisible (resetButton);
        resetButton.setButtonText ("Measure again");
        resetButton.setTooltip ("Clears the measurement and starts over from here");
        resetButton.onClick = [this] { engine.resetMixAnalysis(); repaint(); };

        addAndMakeVisible (targetBox);
        targetBox.addItem ("Spotify, Amazon, YouTube   -14 LUFS", 1);
        targetBox.addItem ("Apple Music, Tidal   -16 LUFS", 2);
        targetBox.addItem ("Club and CD   -9 LUFS", 3);
        targetBox.addItem ("Broadcast, EBU R 128   -23 LUFS", 4);
        targetBox.setSelectedId (1, juce::dontSendNotification);
        targetBox.onChange = [this] { repaint(); };

        startTimerHz (10);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Ahp::black);

        const auto r = engine.getMixReading();
        auto area = getLocalBounds().reduced (16).withTrimmedTop (74);

        if (r.integratedLufs < -150.0)
        {
            g.setColour (Ahp::muted);
            g.setFont (juce::FontOptions (13.0f));
            g.drawText ("Press play. The report builds while the music runs, and reads the master "
                        "after every effect on it.",
                        area.removeFromTop (40), juce::Justification::topLeft, true);
            return;
        }

        const double target = targetLufs();
        const double offset = r.integratedLufs - target;
        const double truePeakDb = r.truePeak > 0.0f
                                    ? 20.0 * std::log10 (r.truePeak) : -200.0;

        // ---- the numbers ----
        auto number = [&] (juce::Rectangle<int> box, const juce::String& label,
                           const juce::String& value, juce::Colour colour)
        {
            g.setColour (Ahp::panel);
            g.fillRect (box);
            g.setColour (Ahp::line);
            g.drawRect (box);

            auto inner = box.reduced (10, 8);
            g.setColour (Ahp::muted);
            g.setFont (juce::FontOptions (10.5f));
            g.drawText (label, inner.removeFromTop (14), juce::Justification::topLeft);

            g.setColour (colour);
            g.setFont (juce::FontOptions (21.0f, juce::Font::bold));
            g.drawText (value, inner, juce::Justification::centredLeft);
        };

        auto row = area.removeFromTop (62);
        const int w = row.getWidth() / 4;

        number (row.removeFromLeft (w).reduced (0, 0), "LOUDNESS (INTEGRATED)",
                juce::String (r.integratedLufs, 1) + " LUFS",
                std::abs (offset) <= 1.0 ? Ahp::bone : Ahp::rec);

        number (row.removeFromLeft (w).reduced (4, 0), "TRUE PEAK",
                juce::String (truePeakDb, 1) + " dBTP",
                truePeakDb > -1.0 ? Ahp::rec : Ahp::bone);

        number (row.removeFromLeft (w).reduced (4, 0), "DYNAMICS (RANGE)",
                juce::String (r.loudnessRange, 1) + " LU",
                r.loudnessRange < 3.0 ? Ahp::rec : Ahp::bone);

        number (row.reduced (4, 0), "MONO COMPATIBILITY",
                juce::String (r.correlation, 2),
                r.correlation < 0.0f ? Ahp::rec : Ahp::bone);

        area.removeFromTop (16);

        // ---- what it means ----
        juce::StringArray findings;

        if (std::abs (offset) <= 1.0)
            findings.add ("Loudness is on target. Streaming will play this back as you hear it.");
        else if (offset > 0.0)
            findings.add ("Louder than the target by " + juce::String (offset, 1)
                          + " LU. Streaming will turn it down by that much, so the extra loudness "
                            "buys nothing and costs dynamics. Pull the master down rather than "
                            "pushing the limiter harder.");
        else
            findings.add ("Quieter than the target by " + juce::String (-offset, 1)
                          + " LU. Streaming will turn it up, so nothing is lost, but it will sit "
                            "quietly beside other material until it does.");

        if (truePeakDb > 0.0)
            findings.add ("True peak is above 0 dBTP (" + juce::String (truePeakDb, 1)
                          + "). The waveform a converter reconstructs between the samples goes "
                            "higher than anything stored in the file, and this much of it is "
                            "already clipping on playback, before any encoding. Pull the ceiling "
                            "down to -1 dBTP.");
        else if (truePeakDb > -1.0)
            findings.add ("True peak is above -1 dBTP. Converting to MP3 or AAC moves peaks "
                          "slightly, so this can clip after encoding even though it plays clean "
                          "now. Set the limiter ceiling to -1 dBTP.");

        if (r.loudnessRange > 0.0 && r.loudnessRange < 3.0)
            findings.add ("Very little dynamic range left (" + juce::String (r.loudnessRange, 1)
                          + " LU). Heavily limited masters measure like this and tire the ear "
                            "quickly. Worth checking whether the limiter is doing too much.");
        else if (r.loudnessRange > 15.0)
            findings.add ("Wide dynamic range (" + juce::String (r.loudnessRange, 1)
                          + " LU). Good for listening, but quiet sections may disappear in a car "
                            "or on a phone.");

        if (r.correlation < 0.0f)
            findings.add ("The channels are out of phase. Folded to mono, as club systems and many "
                          "phones do, parts of this mix will cancel and get quieter or vanish. "
                          "Usually a stereo widener or a doubled part with one side inverted.");
        else if (r.correlation < 0.3f)
            findings.add ("Very wide stereo image. Check it in mono before committing: wide mixes "
                          "often lose weight when the sides collapse.");

        g.setColour (Ahp::muted);
        g.setFont (juce::FontOptions (12.5f));

        for (const auto& line : findings)
        {
            const auto box = area.removeFromTop (46);
            g.setColour (Ahp::bone.withAlpha (0.55f));
            g.fillRect (box.withWidth (2).withTrimmedTop (2).withTrimmedBottom (6));
            g.setColour (Ahp::muted);
            g.drawFittedText (line, box.withTrimmedLeft (12).withTrimmedBottom (6),
                              juce::Justification::topLeft, 3);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        auto top = r.removeFromTop (24);
        title.setBounds (top.removeFromLeft (140));
        r.removeFromTop (8);

        auto controls = r.removeFromTop (26);
        targetBox.setBounds (controls.removeFromLeft (280));
        controls.removeFromLeft (8);
        resetButton.setBounds (controls.removeFromLeft (120));
    }

private:
    double targetLufs() const
    {
        switch (targetBox.getSelectedId())
        {
            case 2:  return -16.0;
            case 3:  return -9.0;
            case 4:  return -23.0;
            default: return -14.0;
        }
    }

    void timerCallback() override { repaint(); }

    AudioEngine&     engine;
    juce::Label      title;
    juce::ComboBox   targetBox;
    juce::TextButton resetButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixReportPanel)
};
