#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "AudioEngine.h"
#include "SpectrumAnalyser.h"
#include "AhpLookAndFeel.h"

//==============================================================================
/** The mix report.

    Not a one knob that claims to master a track. It measures what actually
    leaves the studio and says plainly what is true about it, with the numbers
    that mastering engineers and streaming services use. A plugin cannot do
    this honestly, because a plugin only hears its own insert; the studio hears
    the finished thing.

    The spectrum below the numbers is the same idea in the other dimension:
    the four numbers say how loud, how peaky, how dynamic and how mono the mix
    is, and the bars say where its weight sits. SpectrumAnalyser.h carries the
    band definitions, the normalisation and every display convention, including
    the one that reads as a surprise: the tilt defaults to none, because
    constant ratio bands summed as power already read pink noise flat.

    What it reads is the mix and not the speaker feed. The tap is in
    `AudioEngine::measureMix`, upstream of the control room, so pressing Mono
    or Dim does not move these bars. That is deliberate and it is the whole
    point of having the control room apply on the way out only.
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

        addAndMakeVisible (bandsBox);
        bandsBox.addItem ("Bands: 1/3 octave", 1);
        bandsBox.addItem ("Bands: 1/6 octave", 2);
        bandsBox.setSelectedId (1, juce::dontSendNotification);
        bandsBox.setTooltip ("How wide each bar is. One third octave is the band set every "
                             "measurement microphone and analyser uses; one sixth splits each "
                             "of those in two, which needs a longer window to resolve the "
                             "bottom end.");
        bandsBox.onChange = [this] { repaint(); };

        addAndMakeVisible (resolutionBox);

        for (int i = 0; i < 4; ++i)
            resolutionBox.addItem ("Window: " + juce::String (Spectrum::transformSizes[i])
                                       + " samples", i + 1);

        resolutionBox.setSelectedId (4, juce::dontSendNotification);
        resolutionBox.setTooltip ("How much audio each reading looks at. Longer reaches further "
                                  "down: at 48 kHz the lowest band that can be separated at all "
                                  "is 125 Hz on the shortest setting and 20 Hz on the longest, so "
                                  "the short settings leave the whole bass range blank. Shorter "
                                  "follows the music more closely.");
        resolutionBox.onChange = [this] { repaint(); };

        addAndMakeVisible (ballisticsBox);
        ballisticsBox.addItem ("Speed: fast (125 ms)", 1);
        ballisticsBox.addItem ("Speed: slow (1 s)", 2);
        ballisticsBox.setSelectedId (1, juce::dontSendNotification);
        ballisticsBox.setTooltip ("The two time weightings IEC 61672-1 defines for a sound level "
                                  "meter. Fast follows a kick drum; slow shows the balance over "
                                  "a phrase.");
        ballisticsBox.onChange = [this]
        {
            analyser.setBallistics (ballisticsBox.getSelectedId() == 2 ? Spectrum::Ballistics::slow
                                                                       : Spectrum::Ballistics::fast);
            repaint();
        };

        addAndMakeVisible (tiltBox);
        tiltBox.addItem ("Tilt: none, pink is flat", 1);
        tiltBox.addItem ("Tilt: +1.5 dB/oct", 2);
        tiltBox.setSelectedId (1, juce::dontSendNotification);
        tiltBox.setTooltip ("None is the setting where pink noise draws a flat line, which is "
                            "what these bands do on their own. The tilted setting leans the "
                            "display the way the analysers built on a linear frequency scale "
                            "lean by default, which is a preference rather than a correction.");
        tiltBox.onChange = [this]
        {
            analyser.setTiltDbPerOctave (tiltBox.getSelectedId() == 2
                                             ? Spectrum::preferenceTiltDbPerOctave : 0.0);
            repaint();
        };

        addAndMakeVisible (holdButton);
        holdButton.setButtonText ("Peak hold");
        holdButton.setClickingTogglesState (true);
        holdButton.setTooltip ("Marks the highest each band has reached in the readings taken, "
                              "and keeps it until it is cleared. Readings are snapshots at the "
                              "display rate rather than a continuous measurement, so a very "
                              "short transient can fall between two of them.");
        holdButton.onClick = [this] { repaint(); };

        addAndMakeVisible (holdResetButton);
        holdResetButton.setButtonText ("Clear peaks");
        holdResetButton.onClick = [this] { analyser.resetHold(); repaint(); };

        startTimerHz (20);
    }

    ~MixReportPanel() override
    {
        // The tap is a copy per block in the device callback, so it is turned
        // off with the display rather than left running for a panel nobody is
        // looking at.
        engine.setSpectrumListening (false);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Ahp::black);

        const auto r = engine.getMixReading();
        auto area = getLocalBounds().reduced (16).withTrimmedTop (controlsHeight);

        if (r.integratedLufs < -150.0)
        {
            // The spectrum is drawn even with no loudness reading yet, because
            // it is not gated on the transport: it shows a guitar being played
            // in through a monitored insert, where the loudness deliberately
            // measures nothing.
            drawSpectrum (g, area.removeFromBottom (area.getHeight() - 56));

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

        // ---- where the weight sits ----
        drawSpectrum (g, area.removeFromTop (spectrumHeight));
        area.removeFromTop (14);

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

        r.removeFromTop (8);

        // A second row rather than the right hand end of the first, so that a
        // narrow window pushes nothing off the edge.
        auto second = r.removeFromTop (26);
        bandsBox.setBounds (second.removeFromLeft (150));
        second.removeFromLeft (8);
        resolutionBox.setBounds (second.removeFromLeft (180));
        second.removeFromLeft (8);
        ballisticsBox.setBounds (second.removeFromLeft (160));
        second.removeFromLeft (8);
        tiltBox.setBounds (second.removeFromLeft (190));
        second.removeFromLeft (8);
        holdButton.setBounds (second.removeFromLeft (90));
        second.removeFromLeft (8);
        holdResetButton.setBounds (second.removeFromLeft (100));
    }

    void visibilityChanged() override
    {
        const bool showing = isVisible();

        engine.setSpectrumListening (showing);

        // Cleared rather than left to decay from whatever was on screen when
        // the view was last closed, so the first frame after it comes back
        // seeds the ballistics with the present rather than approaching it
        // from a reading that may be minutes old.
        if (showing)
        {
            analyser.clear();
            lastPushMs = 0.0;
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        cursorX = e.x;
        cursorY = e.y;
        repaint();
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        cursorX = -1;
        cursorY = -1;
        repaint();
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

    void timerCallback() override
    {
        if (isVisible())
        {
            configureIfNeeded();

            if (analyser.isConfigured() && engine.readSpectrumFrame (frameLeft, frameRight))
            {
                const double now = juce::Time::getMillisecondCounterHiRes();

                // The real interval rather than the timer's nominal one,
                // because the ballistics integrate over it and a timer that
                // was late by half of it would otherwise make the display
                // decay at the wrong rate.
                //
                // Clamped at both ends. A zero or negative interval the
                // analyser refuses anyway. A very long one, which is what a
                // window buried behind another for a minute produces, is
                // clamped to half a second: that is four time constants on the
                // fast weighting, so very nearly a jump to the new reading,
                // and half of one on the slow weighting, so a third of the way
                // towards it. Either is better than integrating a minute of
                // elapsed time into one frame, and neither matters much,
                // because the engine refuses a frame at all until a whole
                // window of audio has arrived since the display was shown.
                const double seconds = lastPushMs > 0.0 ? (now - lastPushMs) * 0.001
                                                        : 1.0 / 20.0;
                lastPushMs = now;

                analyser.push (frameLeft.data(), frameRight.data(),
                               analyser.getFftSize(), juce::jlimit (0.001, 0.5, seconds));
            }
        }

        repaint();
    }

    /** Rebuilt whenever the sample rate or a choice moves, and not otherwise:
        `configure` allocates and throws the readings away, so calling it every
        frame would mean a display that never settles. */
    void configureIfNeeded()
    {
        const double rate = engine.getSampleRate();

        if (! (rate > 0.0))
            return;

        const int index = juce::jlimit (0, 3, resolutionBox.getSelectedId() - 1);
        const int size  = Spectrum::transformSizes[index];
        const int perOctave = bandsBox.getSelectedId() == 2 ? 6 : 3;

        if (analyser.getSampleRate() == rate
            && analyser.getFftSize() == size
            && analyser.getBandsPerOctave() == perOctave)
            return;

        analyser.configure (size, rate, perOctave);
        analyser.setBallistics (ballisticsBox.getSelectedId() == 2 ? Spectrum::Ballistics::slow
                                                                   : Spectrum::Ballistics::fast);
        analyser.setTiltDbPerOctave (tiltBox.getSelectedId() == 2
                                         ? Spectrum::preferenceTiltDbPerOctave : 0.0);

        frameLeft.assign ((std::size_t) std::max (0, analyser.getFftSize()), 0.0f);
        frameRight.assign ((std::size_t) std::max (0, analyser.getFftSize()), 0.0f);
        lastPushMs = 0.0;
    }

    /** The octave series this axis labels, and the text for each.

        ISO 266's own preferred frequencies, by way of
        `Spectrum::nominalThirdOctaveFrequency`, so what is printed is a
        preferred frequency and not a rounded exact centre. Only the octave
        series gets a label: thirty one across a window will not fit and the
        eye does not need them. */
    static juce::String octaveLabel (double frequency)
    {
        if (frequency >= 1000.0)
            return juce::String ((int) std::round (frequency / 1000.0)) + "k";

        return frequency == 31.5 ? juce::String ("31.5") : juce::String ((int) frequency);
    }

    /** Where a frequency sits across the plot.

        Labels are placed by frequency rather than under a bar, and that is not
        a tidiness point: at one sixth octave NO band is centred on a preferred
        frequency, because the exact nesting of sixths inside thirds puts every
        third octave centre on a sixth octave EDGE. Matching labels to band
        centres therefore gave sixty two bars and a completely blank frequency
        axis, which an audit found. Placing them by frequency labels the join
        between the two sixths, which is exactly where that frequency is.

        The mapping is a logarithm because the bands are geometrically spaced
        and drawn at equal width, so a band index is linear in log frequency
        and so is this. The two agree by construction rather than by being
        tuned to look right. */
    static float xForFrequency (double frequency, juce::Rectangle<int> plot,
                                double lowEdge, double highEdge)
    {
        if (! (frequency > 0.0) || ! (lowEdge > 0.0) || ! (highEdge > lowEdge))
            return (float) plot.getX();

        const double across = std::log (frequency / lowEdge) / std::log (highEdge / lowEdge);

        return (float) plot.getX()
             + (float) (juce::jlimit (0.0, 1.0, across) * (double) plot.getWidth());
    }

    void drawSpectrum (juce::Graphics& g, juce::Rectangle<int> area)
    {
        if (area.getHeight() < 40 || area.getWidth() < 80)
            return;

        g.setColour (Ahp::panel);
        g.fillRect (area);
        g.setColour (Ahp::line);
        g.drawRect (area);

        auto inner = area.reduced (1);
        auto scale = inner.removeFromLeft (30);
        auto axis  = inner.removeFromBottom (15);
        auto plot  = inner;

        // Pro-Q 4's documented default range, which is the only published
        // default for one anywhere on the reference list. The top is 0 dB
        // because that is where a full scale mix sits: SpectrumAnalyser.h
        // references the scale to a full scale sine in both channels.
        //
        // A band CAN go above it, which an earlier version of this comment
        // denied. Any tilt other than none lifts the top of the display, by
        // 5.96 dB at 16 kHz on the +1.5 dB per octave setting, and a bar that
        // went over used to flatten silently against the top while the cursor
        // read out a number above the scale. A bar over the top is capped in
        // red now, so it is visibly pinned.
        const double topDb   = 0.0;
        const double rangeDb = 90.0;

        auto yFor = [&] (double db)
        {
            const double fraction = juce::jlimit (0.0, 1.0, (topDb - db) / rangeDb);
            return (float) plot.getY() + (float) (fraction * (double) plot.getHeight());
        };

        // ---- the decibel grid ----
        g.setFont (juce::FontOptions (9.5f));

        for (double db = topDb; db >= topDb - rangeDb; db -= 10.0)
        {
            const float y = yFor (db);

            g.setColour (Ahp::line.withAlpha (db == topDb ? 0.9f : 0.5f));
            g.drawHorizontalLine ((int) y, (float) plot.getX(), (float) plot.getRight());

            if (((int) db) % 20 == 0)
            {
                g.setColour (Ahp::muted);
                g.drawText (juce::String ((int) db),
                            scale.getX(), (int) y - 7, scale.getWidth() - 4, 14,
                            juce::Justification::centredRight);
            }
        }

        if (! analyser.isConfigured() || analyser.getNumBands() == 0)
            return;

        const auto& bands = analyser.getBands();
        const int   count = (int) bands.size();
        const int   perOctave = analyser.getBandsPerOctave();

        if (analyser.getFrameCount() == 0)
        {
            g.setColour (Ahp::muted);
            g.setFont (juce::FontOptions (12.0f));
            g.drawText ("Play something, or play in through a monitored strip, and the "
                        "spectrum builds from the master.",
                        plot.reduced (10), juce::Justification::centred, true);
            return;
        }

        // The bands are geometrically spaced, so equal width bars already give
        // a logarithmic frequency axis. That is the reason to draw the display
        // this way round rather than drawing a curve over a log scale and
        // hoping the two agree.
        const double barWidth = (double) plot.getWidth() / (double) count;

        for (int i = 0; i < count; ++i)
        {
            const int x     = plot.getX() + (int) std::round ((double) i * barWidth);
            const int right = plot.getX() + (int) std::round ((double) (i + 1) * barWidth);
            const int width = std::max (1, right - x - 1);

            if (! analyser.resolves (i))
            {
                // Said rather than left blank, because an empty column at the
                // bottom of the display would read as silence down there,
                // which is the one thing refusing a band is supposed to avoid.
                // Faint vertical stripes, which look like nothing and do not
                // look like a measurement.
                g.setColour (Ahp::line.withAlpha (0.6f));

                for (int y = plot.getY() + 3; y < plot.getBottom(); y += 6)
                    g.fillRect (x, y, width, 2);

                continue;
            }

            const double level = analyser.levelDb (i);
            const float  top   = yFor (level);

            if (top < (float) plot.getBottom() - 0.5f)
            {
                g.setColour (Ahp::bone.withAlpha (0.78f));
                g.fillRect ((float) x, top, (float) width, (float) plot.getBottom() - top);
            }

            // Over the top of the scale rather than at it. `yFor` clamps, so
            // without this the bar would sit flat against the top and look
            // like a band that happened to reach full scale.
            if (level > topDb)
            {
                g.setColour (Ahp::rec);
                g.fillRect ((float) x, (float) plot.getY(), (float) width, 3.0f);
            }

            if (holdButton.getToggleState())
            {
                const double held = analyser.holdDb (i);

                if (held > topDb - rangeDb)
                {
                    g.setColour (Ahp::rec);
                    g.fillRect ((float) x, yFor (held) - 1.0f, (float) width, 2.0f);
                }
            }
        }

        // ---- the frequency axis ----
        const double lowEdge  = bands.front().lower;
        const double highEdge = bands.back().upper;

        static const double octaves[10] = { 31.5, 63.0, 125.0, 250.0, 500.0,
                                            1000.0, 2000.0, 4000.0, 8000.0, 16000.0 };

        g.setColour (Ahp::muted);
        g.setFont (juce::FontOptions (9.5f));

        for (const double frequency : octaves)
        {
            if (frequency < lowEdge || frequency > highEdge)
                continue;

            const int at = (int) std::round (xForFrequency (frequency, plot, lowEdge, highEdge));

            g.drawText (octaveLabel (frequency), at - 20, axis.getY(), 40, axis.getHeight(),
                        juce::Justification::centred);
        }

        // ---- and what the stripes at the bottom mean ----
        //
        // Said in words rather than left to the stripes. A refused band is not
        // only blank: the lowest band that IS shown has part of any tone near
        // its lower edge sitting in the refused one, so it reads up to 4 dB
        // low as well. SpectrumAnalyser.h has the measurement. Somebody
        // looking at the bottom of this display deserves to know that the
        // window length and not the mix is what they are looking at.
        int lowestResolved = -1;

        for (int i = 0; i < count; ++i)
            if (analyser.resolves (i))
            {
                lowestResolved = i;
                break;
            }

        if (lowestResolved > 0)
        {
            const double from = bands[(std::size_t) lowestResolved].lower;

            g.setColour (Ahp::muted);
            g.setFont (juce::FontOptions (10.0f));
            g.drawText ("Below " + juce::String ((int) std::round (from))
                            + " Hz the window is too short to separate the bands. "
                              "The lowest bar shown reads low too.",
                        plot.reduced (8, 6).withTrimmedRight (120),
                        juce::Justification::bottomLeft, true);
        }

        // ---- what is under the pointer ----
        if (cursorX >= plot.getX() && cursorX < plot.getRight()
            && cursorY >= plot.getY() && cursorY < plot.getBottom())
        {
            const int index = juce::jlimit (0, count - 1,
                                            (int) ((double) (cursorX - plot.getX()) / barWidth));
            const auto& band = bands[(std::size_t) index];

            g.setColour (Ahp::bone.withAlpha (0.25f));
            g.drawVerticalLine (cursorX, (float) plot.getY(), (float) plot.getBottom());

            const double nominal = Spectrum::nominalThirdOctaveFrequency (band.x, perOctave);

            juce::String readout = (nominal > 0.0 ? juce::String (nominal, nominal < 100.0 ? 1 : 0)
                                                  : juce::String (band.centre, band.centre < 100.0 ? 1 : 0))
                                 + " Hz   ";

            readout += analyser.resolves (index)
                         ? juce::String (analyser.levelDb (index), 1) + " dB"
                         : juce::String ("not resolved at this window length");

            g.setColour (Ahp::bone);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (readout, plot.reduced (8, 6), juce::Justification::topRight);
        }
    }

    AudioEngine&     engine;
    juce::Label      title;
    juce::ComboBox   targetBox;
    juce::TextButton resetButton;

    juce::ComboBox   bandsBox, resolutionBox, ballisticsBox, tiltBox;
    juce::TextButton holdButton, holdResetButton;

    Spectrum::Analyser analyser;
    std::vector<float> frameLeft, frameRight;
    double             lastPushMs = 0.0;
    int                cursorX    = -1;
    int                cursorY    = -1;

    // The title row, the target row and the analyser row with the gaps between
    // them, plus the gap below the last one, all measured from inside the
    // panel's own margin. One constant rather than a number in `resized` and a
    // different number in `paint`, which is how a panel ends up drawing over
    // its own controls the first time a row is added to it.
    static constexpr int controlsHeight = 24 + 8 + 26 + 8 + 26 + 16;
    static constexpr int spectrumHeight = 220;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixReportPanel)
};
