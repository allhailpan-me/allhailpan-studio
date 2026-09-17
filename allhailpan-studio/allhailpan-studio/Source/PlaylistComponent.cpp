#include "PlaylistComponent.h"
#include "AhpLookAndFeel.h"
#include "Logo.h"
#include <cmath>

namespace
{
    const std::vector<std::pair<juce::String, double>> snapChoices {
        { "Bar", 4.0 }, { "Beat", 1.0 }, { "1/2 beat", 0.5 }, { "1/3 beat", 1.0 / 3.0 },
        { "Step", 0.25 }, { "1/2 step", 0.125 }, { "1/3 step", 1.0 / 12.0 },
        { "1/4 step", 0.0625 }, { "1/6 step", 1.0 / 24.0 }, { "None", 0.0 }
    };

    juce::String formatPosition (double beats)
    {
        beats = std::max (0.0, beats);
        const auto bar  = (int) (beats / 4.0) + 1;
        const auto beat = (int) std::fmod (beats, 4.0) + 1;
        const auto tick = (int) ((beats - std::floor (beats)) * 96.0 + 1e-6) % 96;
        return juce::String (bar) + ":" + juce::String (beat) + ":" + juce::String (tick).paddedLeft ('0', 2);
    }

    void quiet (juce::Component& c)
    {
        c.setWantsKeyboardFocus (false);
        c.setMouseClickGrabsKeyboardFocus (false);
    }
}

PlaylistComponent::PlaylistComponent (Project& p, AudioEngine& e, SampleCache& c)
    : project (p), engine (e), cache (c)
{
    watermark = AhpImages::watermark();

    for (auto* b : { &drawButton, &sliceButton, &eraseButton, &stretchButton, &zoomOut, &zoomFit, &zoomIn })
    {
        quiet (*b);
        addAndMakeVisible (b);
    }
    drawButton.onClick  = [this] { setTool (Tool::draw); };
    sliceButton.onClick = [this] { setTool (Tool::slice); };
    eraseButton.onClick = [this] { setTool (Tool::erase); };
    stretchButton.setClickingTogglesState (true);
    stretchButton.setTooltip ("On: dragging an audio clip's edge stretches it. Off: it trims. Hold Shift to do the opposite.");
    zoomOut.onClick = [this] { zoom (1.0 / 1.4, (float) grid.getCentreX()); };
    zoomIn.onClick  = [this] { zoom (1.4, (float) grid.getCentreX()); };
    zoomFit.onClick = [this]
    {
        const double end = std::max (16.0, project.songEndBeats());
        ppb = juce::jlimit (2.0, 400.0, (grid.getWidth() - 20) / end);
        scrollBeats = 0.0;
        updateScrollBars();
        repaint();
    };

    for (int i = 0; i < (int) snapChoices.size(); ++i)
        snapBox.addItem (snapChoices[(size_t) i].first, i + 1);
    snapBox.setSelectedId (5, juce::dontSendNotification);
    snapBox.onChange = [this] { repaint(); };
    snapBox.setTooltip ("Grid for placing, trimming and stretching. Hold Alt to ignore it.");
    quiet (snapBox);
    snapLabel.setColour (juce::Label::textColourId, Ahp::muted);
    positionLabel.setColour (juce::Label::textColourId, Ahp::muted);
    positionLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (snapBox);
    addAndMakeVisible (snapLabel);
    addAndMakeVisible (positionLabel);

    for (auto* sb : { &hbar, &vbar })
    {
        sb->addListener (this);
        sb->setAutoHide (false);
        sb->setColour (juce::ScrollBar::thumbColourId, Ahp::panel3.brighter (0.15f));
        addAndMakeVisible (sb);
    }

    // ---- clip bar ----
    clipName.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    clipStatus.setColour (juce::Label::textColourId, Ahp::muted);
    for (auto* l : { &pitchLabel, &stretchLabel, &gainLabel })
        l->setColour (juce::Label::textColourId, Ahp::muted);

    auto setupSlider = [this] (juce::Slider& s)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);
        s.setColour (juce::Slider::trackColourId, Ahp::bone);
        s.setColour (juce::Slider::backgroundColourId, Ahp::panel3);
        s.onDragStart = [this] { sliderDragging = true; };
        s.onDragEnd   = [this] { sliderDragging = false; project.changed(); };
        quiet (s);
    };

    setupSlider (pitchSlider);
    pitchSlider.setRange (-24.0, 24.0, 0.01);
    pitchSlider.setDoubleClickReturnValue (true, 0.0);
    pitchSlider.textFromValueFunction = [] (double v) { return (v > 0 ? "+" : "") + juce::String (v, 2) + " st"; };
    pitchSlider.valueFromTextFunction = [] (const juce::String& t) { return t.retainCharacters ("-.0123456789").getDoubleValue(); };
    pitchSlider.onValueChange = [this]
    {
        if (updatingClipBar) return;
        if (auto* c = singleSelected(); c != nullptr && c->isAudio())
        {
            c->pitch = pitchSlider.getValue();
            project.changed();
        }
    };

    setupSlider (stretchSlider);
    stretchSlider.setRange (0.25, 4.0, 0.001);
    stretchSlider.setSkewFactorFromMidPoint (1.0);
    stretchSlider.setDoubleClickReturnValue (true, 1.0);
    stretchSlider.textFromValueFunction = [] (double v) { return "x" + juce::String (v, 3); };
    stretchSlider.valueFromTextFunction = [] (const juce::String& t) { return t.retainCharacters (".0123456789").getDoubleValue(); };
    stretchSlider.onValueChange = [this]
    {
        if (updatingClipBar) return;
        if (auto* c = singleSelected(); c != nullptr && c->isAudio())
        {
            c->stretch = stretchSlider.getValue();
            project.changed();
        }
    };

    setupSlider (gainSlider);
    gainSlider.setRange (-36.0, 12.0, 0.1);
    gainSlider.setDoubleClickReturnValue (true, 0.0);
    gainSlider.textFromValueFunction = [] (double v) { return juce::String (v, 1) + " dB"; };
    gainSlider.valueFromTextFunction = [] (const juce::String& t) { return t.retainCharacters ("-.0123456789").getDoubleValue(); };
    gainSlider.onValueChange = [this]
    {
        if (updatingClipBar) return;
        if (auto* c = singleSelected())
        {
            c->gainDb = (float) gainSlider.getValue();
            project.changed();
        }
    };

    resetButton.onClick = [this]
    {
        if (auto* c = singleSelected(); c != nullptr && c->isAudio())
        {
            c->pitch = 0.0;
            c->stretch = 1.0;
            c->gainDb = 0.0f;
            project.changed();
            updateClipBar();
        }
    };
    openRollButton.onClick = [this]
    {
        if (auto* c = singleSelected(); c != nullptr && ! c->isAudio() && onOpenPianoRoll)
            onOpenPianoRoll (c->id);
    };
    channelBox.onChange = [this]
    {
        if (updatingClipBar) return;
        if (auto* c = singleSelected(); c != nullptr && ! c->isAudio())
        {
            c->channel = channelBox.getSelectedId() - 1;
            project.changed();
        }
    };

    for (auto* comp : { static_cast<juce::Component*> (&clipName), static_cast<juce::Component*> (&clipStatus),
                        static_cast<juce::Component*> (&pitchLabel), static_cast<juce::Component*> (&stretchLabel),
                        static_cast<juce::Component*> (&gainLabel), static_cast<juce::Component*> (&resetButton),
                        static_cast<juce::Component*> (&openRollButton), static_cast<juce::Component*> (&channelBox) })
    {
        quiet (*comp);
        addChildComponent (comp);
    }
    addChildComponent (pitchSlider);
    addChildComponent (stretchSlider);
    addChildComponent (gainSlider);

    setTool (Tool::draw);
    quiet (*this);
}

PlaylistComponent::~PlaylistComponent()
{
    hbar.removeListener (this);
    vbar.removeListener (this);
}

bool PlaylistComponent::isEditing() const noexcept
{
    return sliderDragging
        || drag.mode == DragState::Mode::move
        || drag.mode == DragState::Mode::trimLeft  || drag.mode == DragState::Mode::trimRight
        || drag.mode == DragState::Mode::stretchLeft || drag.mode == DragState::Mode::stretchRight;
}

// ---------------------------------------------------------------------------
// Layout

void PlaylistComponent::resized()
{
    auto r = getLocalBounds();
    toolbar = r.removeFromTop (toolbarH);

    auto tb = toolbar.reduced (10, 6);
    drawButton   .setBounds (tb.removeFromLeft (56));
    sliceButton  .setBounds (tb.removeFromLeft (56));
    eraseButton  .setBounds (tb.removeFromLeft (62));  tb.removeFromLeft (10);
    stretchButton.setBounds (tb.removeFromLeft (70));  tb.removeFromLeft (14);
    snapLabel    .setBounds (tb.removeFromLeft (42));
    snapBox      .setBounds (tb.removeFromLeft (110)); tb.removeFromLeft (14);
    zoomOut      .setBounds (tb.removeFromLeft (30));
    zoomFit      .setBounds (tb.removeFromLeft (44));
    zoomIn       .setBounds (tb.removeFromLeft (30));
    positionLabel.setBounds (tb);

    clipBar = r.removeFromBottom (clipBarH);
    layoutClipBar (clipBar);

    hbar.setBounds (r.removeFromBottom (barThickness).withTrimmedLeft (headerW).withTrimmedRight (barThickness));
    vbar.setBounds (r.removeFromRight (barThickness).withTrimmedTop (rulerH));

    ruler  = r.removeFromTop (rulerH).withTrimmedLeft (headerW);
    header = r.removeFromLeft (headerW);
    grid   = r;

    updateScrollBars();
}

void PlaylistComponent::layoutClipBar (juce::Rectangle<int> r)
{
    r = r.reduced (10, 8);
    clipName.setBounds (r.removeFromLeft (200));
    r.removeFromLeft (8);

    pitchLabel  .setBounds (r.removeFromLeft (38));
    pitchSlider .setBounds (r.removeFromLeft (190));  r.removeFromLeft (10);
    stretchLabel.setBounds (r.removeFromLeft (52));
    stretchSlider.setBounds (r.removeFromLeft (190)); r.removeFromLeft (10);
    gainLabel   .setBounds (r.removeFromLeft (34));
    gainSlider  .setBounds (r.removeFromLeft (170));  r.removeFromLeft (10);
    resetButton .setBounds (r.removeFromLeft (60));   r.removeFromLeft (10);

    // MIDI clip controls reuse the same row
    auto midiRow = pitchLabel.getBounds().getUnion (pitchSlider.getBounds());
    channelBox.setBounds (midiRow.withWidth (220));
    openRollButton.setBounds (stretchLabel.getBounds().getUnion (stretchSlider.getBounds()).withWidth (140));

    clipStatus.setBounds (r);
}

Clip* PlaylistComponent::singleSelected()
{
    if (project.selection.size() != 1)
        return nullptr;
    return project.find (*project.selection.begin());
}

void PlaylistComponent::updateClipBar()
{
    const juce::ScopedValueSetter<bool> svs (updatingClipBar, true);
    auto* c = singleSelected();
    const bool audio = c != nullptr && c->isAudio();
    const bool midi  = c != nullptr && ! c->isAudio();

    clipName.setVisible (c != nullptr);
    clipStatus.setVisible (c != nullptr || project.selection.size() > 1);
    for (auto* comp : { static_cast<juce::Component*> (&pitchLabel), static_cast<juce::Component*> (&pitchSlider),
                        static_cast<juce::Component*> (&stretchLabel), static_cast<juce::Component*> (&stretchSlider),
                        static_cast<juce::Component*> (&resetButton) })
        comp->setVisible (audio);
    gainLabel.setVisible (c != nullptr);
    gainSlider.setVisible (c != nullptr);
    channelBox.setVisible (midi);
    openRollButton.setVisible (midi);

    if (project.selection.size() > 1)
        clipStatus.setText (juce::String ((int) project.selection.size()) + " clips selected.  Ctrl+C copy, Ctrl+X cut, Ctrl+B duplicate, Delete removes.",
                            juce::dontSendNotification);

    if (c == nullptr)
        return;

    clipName.setText (c->displayName (project.channels), juce::dontSendNotification);
    gainSlider.setValue (c->gainDb, juce::dontSendNotification);

    if (audio)
    {
        pitchSlider.setValue (c->pitch, juce::dontSendNotification);
        stretchSlider.setValue (c->stretch, juce::dontSendNotification);
        const bool rendering = isRendering && isRendering (*c);
        clipStatus.setText (rendering ? "Rendering high-quality stretch..."
                                      : "Starts " + formatPosition (c->start) + "   Length "
                                            + juce::String (c->lengthBeats (project.bpm) / 4.0, 3) + " bars",
                            juce::dontSendNotification);
    }
    else
    {
        channelBox.clear (juce::dontSendNotification);
        for (int i = 0; i < kNumChannels; ++i)
        {
            const auto& ch = project.channels[(size_t) i];
            channelBox.addItem ("Plays channel " + juce::String (i + 1) + (ch.name.isNotEmpty() ? ": " + ch.name : juce::String()), i + 1);
        }
        channelBox.setSelectedId (c->channel + 1, juce::dontSendNotification);
        clipStatus.setText (juce::String ((int) c->pattern->notes.size()) + " notes, "
                                + juce::String ((int) c->pattern->lanes.size()) + " automation lanes.  Double-click the clip to edit.",
                            juce::dontSendNotification);
    }
}

void PlaylistComponent::updateScrollBars()
{
    const double visibleBeats = grid.getWidth() / ppb;
    const double totalBeats   = std::max (project.songEndBeats() + 64.0, scrollBeats + visibleBeats);
    hbar.setRangeLimits (0.0, totalBeats, juce::dontSendNotification);
    hbar.setCurrentRange (scrollBeats, visibleBeats, juce::dontSendNotification);

    const double totalH = (double) project.tracks.size() * trackH;
    scrollY = (float) juce::jlimit (0.0, std::max (0.0, totalH - grid.getHeight()), (double) scrollY);
    vbar.setRangeLimits (0.0, std::max (totalH, (double) grid.getHeight()), juce::dontSendNotification);
    vbar.setCurrentRange (scrollY, grid.getHeight(), juce::dontSendNotification);
}

void PlaylistComponent::scrollBarMoved (juce::ScrollBar* bar, double start)
{
    if (bar == &hbar) scrollBeats = std::max (0.0, start);
    else              scrollY = (float) start;
    repaint();
}

void PlaylistComponent::zoom (double factor, float anchorX)
{
    const double anchorBeat = xToBeat (anchorX);
    ppb = juce::jlimit (2.0, 400.0, ppb * factor);
    scrollBeats = std::max (0.0, anchorBeat - (anchorX - grid.getX()) / ppb);
    updateScrollBars();
    repaint();
}

void PlaylistComponent::setTool (Tool t)
{
    tool = t;
    drawButton .setToggleState (t == Tool::draw,  juce::dontSendNotification);
    sliceButton.setToggleState (t == Tool::slice, juce::dontSendNotification);
    eraseButton.setToggleState (t == Tool::erase, juce::dontSendNotification);
}

void PlaylistComponent::refresh()
{
    updateScrollBars();
    if (! sliderDragging)
        updateClipBar();
    repaint();
}

void PlaylistComponent::updatePlayhead()
{
    const double beat = engine.getBeatPosition();
    positionLabel.setText (mouseOverGrid ? formatPosition (hoverBeat) : formatPosition (beat), juce::dontSendNotification);

    if (engine.isPlaying())
    {
        const double visible = grid.getWidth() / ppb;
        if (drag.mode == DragState::Mode::none && (beat < scrollBeats || beat > scrollBeats + visible * 0.95))
        {
            scrollBeats = std::max (0.0, beat - visible * 0.05);
            updateScrollBars();
            repaint();
            lastPlayhead = beat;
            return;
        }
    }

    if (beat != lastPlayhead)
    {
        const auto x0 = beatToX (lastPlayhead), x1 = beatToX (beat);
        repaint (juce::Rectangle<float> (std::min (x0, x1) - 3.0f, (float) ruler.getY(),
                                         std::abs (x1 - x0) + 6.0f, (float) (grid.getBottom() - ruler.getY())).toNearestInt());
        lastPlayhead = beat;
    }
}

// ---------------------------------------------------------------------------
// Drawing

juce::Rectangle<float> PlaylistComponent::clipBounds (const Clip& c) const
{
    const float x = beatToX (c.start);
    const float w = std::max (3.0f, (float) (c.lengthBeats (project.bpm) * ppb));
    return { x, trackToY (c.track) + 2.0f, w, (float) trackH - 5.0f };
}

void PlaylistComponent::paint (juce::Graphics& g)
{
    g.fillAll (Ahp::panel);
    g.setColour (Ahp::line);
    g.fillRect (toolbar.withTop (toolbar.getBottom() - 1));

    const double b0 = scrollBeats, b1 = xToBeat ((float) grid.getRight());

    // ---- grid ----
    {
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (grid);

        for (int bar = (int) (b0 / 4.0); bar * 4.0 <= b1; ++bar)
            if ((bar / 4) % 2 == 1)
            {
                g.setColour (Ahp::panel2);
                g.fillRect (juce::Rectangle<float> (beatToX (bar * 4.0), (float) grid.getY(), (float) (4.0 * ppb), (float) grid.getHeight()));
            }

        // the ALLHAILPAN mark, big and quiet, fixed behind everything
        if (watermark.isValid())
        {
            auto area = grid.toFloat().reduced (grid.getWidth() * 0.08f, grid.getHeight() * 0.08f);
            g.setOpacity (0.055f);
            g.drawImage (watermark, area, juce::RectanglePlacement::centred);
            g.setOpacity (1.0f);
        }

        const double snapSize = snapValue ({});
        const double minor = (snapSize > 0.0 && snapSize * ppb >= 6.0) ? snapSize : (ppb >= 6.0 ? 1.0 : 4.0);
        for (double b = std::floor (b0 / minor) * minor; b <= b1; b += minor)
        {
            const bool isBar  = std::abs (b / 4.0 - std::round (b / 4.0)) < 1e-6;
            const bool isBeat = std::abs (b - std::round (b)) < 1e-6;
            g.setColour (isBar ? Ahp::muted.withAlpha (0.8f) : isBeat ? Ahp::line : Ahp::line.withAlpha (0.45f));
            g.fillRect (juce::Rectangle<float> (std::round (beatToX (b)), (float) grid.getY(), 1.0f, (float) grid.getHeight()));
        }

        g.setColour (Ahp::line);
        for (int t = 0; t <= (int) project.tracks.size(); ++t)
            g.fillRect (juce::Rectangle<float> ((float) grid.getX(), trackToY (t) - 1.0f, (float) grid.getWidth(), 1.0f));

        // loop end
        const double end = project.songEndBeats();
        if (end > 0.0)
        {
            g.setColour (Ahp::bone.withAlpha (0.25f));
            g.fillRect (juce::Rectangle<float> (beatToX (end), (float) grid.getY(), 1.0f, (float) grid.getHeight()));
        }

        for (const auto& c : project.clips)
        {
            auto r = clipBounds (c);
            if (r.getRight() < grid.getX() || r.getX() > grid.getRight() || r.getBottom() < grid.getY() || r.getY() > grid.getBottom())
                continue;
            paintClip (g, c, r, project.selection.count (c.id) > 0);
        }

        if (showDrop)
        {
            const float dash[] = { 4.0f, 3.0f };
            juce::Path p, dashed;
            p.addRectangle (beatToX (dropBeat), trackToY (dropTrack) + 2.0f, (float) (4.0 * ppb), (float) trackH - 5.0f);
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, p, dash, 2);
            g.setColour (Ahp::bone);
            g.fillPath (dashed);
        }

        if (drag.mode == DragState::Mode::marquee)
        {
            const float x0 = beatToX (std::min (drag.beat0, drag.beat1)), x1 = beatToX (std::max (drag.beat0, drag.beat1));
            const float y0 = trackToY (std::min (drag.trackA, drag.trackB)), y1 = trackToY (std::max (drag.trackA, drag.trackB) + 1);
            juce::Rectangle<float> m (x0, y0, x1 - x0, y1 - y0);
            g.setColour (Ahp::bone.withAlpha (0.08f));
            g.fillRect (m);
            g.setColour (Ahp::bone);
            g.drawRect (m, 1.0f);
        }
    }

    // ---- ruler ----
    g.setColour (Ahp::panel3);
    g.fillRect (ruler);
    {
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (ruler);
        g.setFont (juce::FontOptions (11.0f));
        const int every = ppb * 4 >= 36 ? 1 : ppb * 16 >= 36 ? 4 : 16;
        for (int bar = (int) (b0 / 4.0); bar * 4.0 <= b1; ++bar)
        {
            if (bar % every) continue;
            const float x = beatToX (bar * 4.0);
            g.setColour (Ahp::bone);
            g.drawText (juce::String (bar + 1), juce::Rectangle<float> (x + 3.0f, (float) ruler.getY(), 40.0f, 14.0f), juce::Justification::centredLeft);
            g.fillRect (juce::Rectangle<float> (x, (float) ruler.getY() + 14.0f, 1.0f, 12.0f));
        }

        const double end = project.songEndBeats();
        if (end > 0.0)
        {
            g.setColour (Ahp::muted);
            g.fillRect (juce::Rectangle<float> (beatToX (end) - 1.0f, (float) ruler.getY(), 2.0f, (float) ruler.getHeight()));
        }

        const float sx = beatToX (engine.getSongStart());
        juce::Path tri;
        tri.addTriangle (sx - 6.0f, (float) ruler.getY() + 12.0f, sx + 6.0f, (float) ruler.getY() + 12.0f, sx, (float) ruler.getBottom() - 1.0f);
        g.setColour (Ahp::bone);
        g.fillPath (tri);
    }

    // ---- track headers ----
    g.setColour (Ahp::panel);
    g.fillRect (header);
    g.setColour (Ahp::panel3);
    g.fillRect (juce::Rectangle<int> (header.getX(), ruler.getY(), headerW, rulerH));
    {
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (header);
        for (int t = 0; t < (int) project.tracks.size(); ++t)
        {
            const auto& tr = project.tracks[(size_t) t];
            const float y  = trackToY (t);
            const float cy = y + trackH / 2.0f;

            g.setColour (Ahp::line);
            g.fillRect (juce::Rectangle<float> ((float) header.getX(), y + trackH - 1.0f, (float) headerW, 1.0f));

            juce::Rectangle<float> led (10.0f, cy - 5.0f, 10.0f, 10.0f);
            if (tr.muted) { g.setColour (Ahp::muted); g.drawEllipse (led, 1.0f); }
            else          { g.setColour (Ahp::bone);  g.fillEllipse (led); }

            g.setColour (tr.muted ? Ahp::muted : Ahp::bone);
            g.setFont (juce::FontOptions (12.5f));
            g.drawText (tr.name, juce::Rectangle<float> (28.0f, y + 6.0f, (float) headerW - 60.0f, 18.0f), juce::Justification::centredLeft);
            g.setColour (Ahp::muted);
            g.setFont (juce::FontOptions (10.5f));
            g.drawText (tr.insert == 0 ? juce::String ("Master") : "Insert " + juce::String (tr.insert),
                        juce::Rectangle<float> (28.0f, y + 24.0f, (float) headerW - 60.0f, 16.0f), juce::Justification::centredLeft);

            juce::Rectangle<float> arm ((float) headerW - 26.0f, cy - 7.0f, 14.0f, 14.0f);
            if (tr.armed) { g.setColour (Ahp::rec); g.fillEllipse (arm); }
            else          { g.setColour (Ahp::muted); g.drawEllipse (arm.reduced (1.0f), 1.0f); }
        }
    }
    g.setColour (Ahp::line);
    g.fillRect (header.getRight() - 1, ruler.getY(), 1, grid.getBottom() - ruler.getY());

    // ---- clip bar ----
    g.setColour (Ahp::panel2);
    g.fillRect (clipBar);
    g.setColour (Ahp::line);
    g.fillRect (clipBar.withHeight (1));
    if (project.selection.empty())
    {
        g.setColour (Ahp::muted);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("Click the ruler to play from there.  Ctrl+drag to select.  Ctrl+C / Ctrl+V copy and paste.  Ctrl+Z / Ctrl+Y undo and redo.  Alt ignores the grid.",
                    clipBar.reduced (12, 0), juce::Justification::centredLeft);
    }

    // ---- playhead ----
    if (engine.isPlaying())
    {
        const float px = beatToX (engine.getBeatPosition());
        if (px >= grid.getX() && px <= grid.getRight())
        {
            g.setColour (Ahp::bone.withAlpha (0.9f));
            g.fillRect (juce::Rectangle<float> (std::round (px), (float) ruler.getY(), 2.0f, (float) (grid.getBottom() - ruler.getY())));
        }
    }
}

void PlaylistComponent::paintClip (juce::Graphics& g, const Clip& c, juce::Rectangle<float> r, bool selected)
{
    const bool dim = c.muted || project.tracks[(size_t) c.track].muted;
    const float a  = dim ? 0.4f : 1.0f;

    g.setColour ((c.isAudio() ? Ahp::panel3 : Ahp::panel3.brighter (0.06f)).withAlpha (a));
    g.fillRect (r.reduced (1.0f, 0.0f));

    auto head = r.withHeight (14.0f).reduced (1.0f, 0.0f);
    const bool missing = c.isAudio() && c.sample != nullptr && c.sample->isMissing();
    g.setColour ((missing ? Ahp::rec : selected ? Ahp::bone : Ahp::muted).withAlpha (a));
    g.fillRect (head);

    juce::Graphics::ScopedSaveState s (g);
    g.reduceClipRegion (r.toNearestInt());

    g.setColour (selected ? Ahp::black : Ahp::panel);
    g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
    juce::String label = c.displayName (project.channels);
    if (c.isAudio())
    {
        if (std::abs (c.stretch - 1.0) > 1e-4) label << "   x" << juce::String (c.stretch, 2);
        if (std::abs (c.pitch) > 1e-4)         label << "   " << (c.pitch > 0 ? "+" : "") << juce::String (c.pitch, 2) << " st";
        if (isRendering && isRendering (c))    label << "   rendering...";
    }
    g.drawText (label, head.withTrimmedLeft (4.0f), juce::Justification::centredLeft);

    const int x0 = (int) std::max (r.getX() + 1.0f, (float) grid.getX());
    const int x1 = (int) std::min (r.getRight() - 1.0f, (float) grid.getRight());
    const float bodyTop = r.getY() + 15.0f, bodyH = r.getHeight() - 16.0f;

    if (c.isAudio() && c.sample != nullptr && ! c.sample->peaks.empty())
    {
        const auto& peaks = c.sample->peaks;
        const float midY  = bodyTop + bodyH / 2.0f;
        const float amp   = bodyH / 2.0f - 1.0f;
        const double sourceSecondsPerPixel = 60.0 / project.bpm / ppb / c.stretch;
        const double peaksPerSecond = c.sample->sampleRate / SampleData::peakBlock;
        const float gain = juce::Decibels::decibelsToGain (c.gainDb);

        g.setColour (Ahp::bone.withAlpha (dim ? 0.35f : 0.9f));
        for (int x = x0; x < x1; ++x)
        {
            const double t0 = c.offset + (x - r.getX()) * sourceSecondsPerPixel;
            const double t1 = t0 + sourceSecondsPerPixel;
            size_t i0 = (size_t) std::max (0.0, t0 * peaksPerSecond);
            size_t i1 = std::max (i0 + 1, (size_t) std::max (0.0, t1 * peaksPerSecond));
            float v = 0.0f;
            for (size_t i = i0; i < std::min (i1, peaks.size()); ++i)
                v = std::max (v, peaks[i]);
            v = std::min (1.0f, v * gain);
            g.fillRect ((float) x, midY - v * amp, 1.0f, std::max (1.0f, v * amp * 2.0f));
        }
    }
    else if (! c.isAudio() && c.pattern != nullptr && ! c.pattern->notes.empty())
    {
        int lo = 127, hi = 0;
        for (auto& n : c.pattern->notes) { lo = std::min (lo, n.note); hi = std::max (hi, n.note); }
        const float range = (float) std::max (12, hi - lo + 1);
        const float rowH  = std::max (1.5f, bodyH / range);

        g.setColour (Ahp::bone.withAlpha (dim ? 0.35f : 0.9f));
        for (auto& n : c.pattern->notes)
        {
            const double localStart = n.start - c.offset;
            if (localStart + n.length <= 0.0 || localStart >= c.length)
                continue;
            const float nx = beatToX (c.start + std::max (0.0, localStart));
            const float nw = (float) ((std::min (c.length, localStart + n.length) - std::max (0.0, localStart)) * ppb);
            const float ny = bodyTop + (float) (hi - n.note) / range * bodyH;
            g.fillRect (nx, ny, std::max (1.0f, nw - 1.0f), rowH);
        }
    }

    if (! c.isAudio() && c.pattern != nullptr && ! c.pattern->lanes.empty())
    {
        g.setColour (Ahp::bone.withAlpha (0.5f));
        g.setFont (juce::FontOptions (9.5f));
        g.drawText (juce::String ((int) c.pattern->lanes.size()) + " auto", r.withTrimmedRight (4.0f).withTrimmedTop (15.0f),
                    juce::Justification::topRight);
    }

    if (selected)
    {
        g.setColour (Ahp::bone);
        g.drawRect (r.reduced (1.0f, 0.0f), 1.5f);
    }
}

// ---------------------------------------------------------------------------
// Mouse

PlaylistComponent::Hit PlaylistComponent::hitTest (juce::Point<float> p) const
{
    Hit h;
    h.beat = xToBeat (p.x);
    if (ruler.contains (p.toInt()))  { h.zone = Zone::ruler; return h; }
    if (header.contains (p.toInt())) { h.zone = Zone::header; h.track = yToTrack (p.y); return h; }
    if (! grid.contains (p.toInt())) return h;

    h.zone  = Zone::grid;
    h.track = yToTrack (p.y);

    for (auto it = project.clips.rbegin(); it != project.clips.rend(); ++it)
    {
        if (it->track != h.track) continue;
        const auto r = clipBounds (*it);
        if (p.x >= r.getX() && p.x < r.getRight())
        {
            h.clipId = it->id;
            if (r.getWidth() > 18.0f)
            {
                if (r.getRight() - p.x < 7.0f)  h.edge = Edge::right;
                else if (p.x - r.getX() < 7.0f) h.edge = Edge::left;
            }
            break;
        }
    }
    return h;
}

double PlaylistComponent::snapValue (const juce::ModifierKeys& mods) const
{
    if (mods.isAltDown())
        return 0.0;
    return snapChoices[(size_t) std::max (0, snapBox.getSelectedId() - 1)].second;
}

double PlaylistComponent::snap (double beat, const juce::ModifierKeys& mods, bool roundDown) const
{
    const double s = snapValue (mods);
    if (s <= 0.0)
        return beat;
    return (roundDown ? std::floor (beat / s + 1e-9) : std::round (beat / s)) * s;
}

void PlaylistComponent::mouseMove (const juce::MouseEvent& e)
{
    const auto h = hitTest (e.position);
    hoverBeat     = std::max (0.0, snap (h.beat, e.mods, true));
    hoverTrack    = h.track;
    mouseOverGrid = h.zone == Zone::grid;

    auto cursor = juce::MouseCursor::NormalCursor;
    if (h.zone == Zone::ruler) cursor = juce::MouseCursor::PointingHandCursor;
    else if (h.zone == Zone::grid)
    {
        if (tool == Tool::slice)             cursor = juce::MouseCursor::CrosshairCursor;
        else if (tool == Tool::erase)        cursor = juce::MouseCursor::NormalCursor;
        else if (h.edge != Edge::none)       cursor = juce::MouseCursor::LeftRightResizeCursor;
        else if (h.clipId != 0)              cursor = juce::MouseCursor::DraggingHandCursor;
    }
    setMouseCursor (cursor);
}

void PlaylistComponent::mouseExit (const juce::MouseEvent&)
{
    mouseOverGrid = false;
}

void PlaylistComponent::mouseDown (const juce::MouseEvent& e)
{
    if (onInteraction)
        onInteraction();

    const auto h = hitTest (e.position);
    drag = {};

    if (h.zone == Zone::ruler)
    {
        // Click the timeline to play from there
        const double b = std::max (0.0, snap (h.beat, e.mods, true));
        if (onPlayFrom)
            onPlayFrom (b);
        repaint();
        return;
    }

    if (h.zone == Zone::header)
    {
        if (! juce::isPositiveAndBelow (h.track, (int) project.tracks.size()))
            return;
        if (e.mods.isPopupMenu())
        {
            showTrackMenu (h.track);
            return;
        }
        auto& tr = project.tracks[(size_t) h.track];
        if (e.x < 26)                tr.muted = ! tr.muted;
        else if (e.x > headerW - 34) tr.armed = ! tr.armed;
        project.changed();
        repaint();
        return;
    }

    if (h.zone != Zone::grid || ! juce::isPositiveAndBelow (h.track, (int) project.tracks.size()))
        return;

    // Ctrl+drag draws a selection box (Ctrl+click toggles a clip)
    if (e.mods.isCtrlDown() || e.mods.isCommandDown())
    {
        drag.mode      = DragState::Mode::marquee;
        drag.beat0     = drag.beat1 = h.beat;
        drag.trackA    = drag.trackB = h.track;
        drag.clipId    = h.clipId;
        drag.ctrlClick = true;
        return;
    }

    if (e.mods.isPopupMenu() || tool == Tool::erase)
    {
        drag.mode = DragState::Mode::erase;
        if (h.clipId != 0)
        {
            project.selection = { h.clipId };
            project.removeSelected();
            updateClipBar();
        }
        return;
    }

    if (tool == Tool::slice)
    {
        if (h.clipId != 0)
            project.split (h.clipId, snap (h.beat, e.mods));
        repaint();
        return;
    }

    if (h.clipId == 0)
    {
        // Empty space: deselect and move the playhead there
        project.selection.clear();
        if (onSetPosition)
            onSetPosition (std::max (0.0, snap (h.beat, e.mods, true)));
        updateClipBar();
        repaint();
        return;
    }

    auto* clip = project.find (h.clipId);
    if (clip == nullptr)
        return;

    if (e.mods.isShiftDown() && h.edge == Edge::none)
    {
        // Shift+drag duplicates the selection
        if (! project.selection.count (clip->id))
            project.selection = { clip->id };
        const auto picked = project.selectedClips();
        project.selection.clear();
        int newGrabbed = 0;
        for (auto& original : picked)
        {
            const int id = project.addClip (original.deepCopy());
            project.selection.insert (id);
            if (original.id == clip->id)
                newGrabbed = id;
        }
        clip = project.find (newGrabbed);
        if (clip == nullptr)
            return;
    }
    else if (! project.selection.count (clip->id))
    {
        project.selection = { clip->id };
    }

    drag.clipId  = clip->id;
    drag.grab    = h.beat - clip->start;
    drag.track0  = h.track;
    drag.anchor0 = clip->start;

    const bool stretchMode = clip->isAudio() && (stretchButton.getToggleState() != e.mods.isShiftDown());
    if (h.edge == Edge::right)      drag.mode = stretchMode ? DragState::Mode::stretchRight : DragState::Mode::trimRight;
    else if (h.edge == Edge::left)  drag.mode = stretchMode ? DragState::Mode::stretchLeft  : DragState::Mode::trimLeft;
    else                            drag.mode = DragState::Mode::move;

    if (drag.mode != DragState::Mode::move)
        project.selection = { clip->id };

    for (auto& c : project.clips)
        if (project.selection.count (c.id))
            drag.originals.push_back (c);

    updateClipBar();
    repaint();
}

void PlaylistComponent::mouseDrag (const juce::MouseEvent& e)
{
    const auto h = hitTest (e.position);
    hoverBeat = std::max (0.0, snap (h.beat, e.mods, true));
    const double spb = 60.0 / project.bpm;

    auto original = [this] (int id) -> const Clip*
    {
        for (auto& o : drag.originals)
            if (o.id == id)
                return &o;
        return nullptr;
    };

    switch (drag.mode)
    {
        case DragState::Mode::none:
            return;

        case DragState::Mode::marquee:
            drag.beat1  = h.beat;
            drag.trackB = juce::jlimit (0, (int) project.tracks.size() - 1, yToTrack (e.position.y));
            if (e.getDistanceFromDragStart() > 3)
                drag.ctrlClick = false;
            repaint (grid);
            return;

        case DragState::Mode::erase:
            if (h.clipId != 0)
            {
                project.selection = { h.clipId };
                project.removeSelected();
            }
            return;

        case DragState::Mode::move:
        {
            const double target = std::max (0.0, snap (h.beat - drag.grab, e.mods));
            double delta = target - drag.anchor0;

            double minStart = 1e12; int minTrack = 1 << 20, maxTrack = -1;
            for (auto& o : drag.originals)
            {
                minStart = std::min (minStart, o.start);
                minTrack = std::min (minTrack, o.track);
                maxTrack = std::max (maxTrack, o.track);
            }
            if (minStart + delta < 0.0) delta = -minStart;

            int dt = juce::jlimit (0, (int) project.tracks.size() - 1, h.track) - drag.track0;
            dt = juce::jlimit (-minTrack, (int) project.tracks.size() - 1 - maxTrack, dt);

            for (auto& o : drag.originals)
                if (auto* c = project.find (o.id))
                {
                    c->start = o.start + delta;
                    c->track = o.track + dt;
                }
            drag.moved = true;
            break;
        }

        case DragState::Mode::trimRight:
        {
            auto* c = project.find (drag.clipId);
            auto* o = original (drag.clipId);
            if (c == nullptr || o == nullptr) return;
            const double minBeats = std::max (snapValue (e.mods), 1.0 / 64.0);
            const double end = std::max (o->start + minBeats, snap (h.beat, e.mods));
            if (c->isAudio())
            {
                if (c->sample == nullptr) return;
                c->length = juce::jlimit (0.005, c->sample->durationSeconds() - o->offset, (end - o->start) * spb / o->stretch);
            }
            else
            {
                c->length = end - o->start;
            }
            drag.moved = true;
            break;
        }

        case DragState::Mode::trimLeft:
        {
            auto* c = project.find (drag.clipId);
            auto* o = original (drag.clipId);
            if (c == nullptr || o == nullptr) return;
            const double originalEnd = o->endBeat (project.bpm);
            double newStart = std::min (snap (h.beat, e.mods), originalEnd - 1.0 / 64.0);

            if (c->isAudio())
            {
                const double beatsPerSourceSecond = o->stretch / spb;
                newStart = std::max ({ newStart, o->start - o->offset * beatsPerSourceSecond, 0.0 });
                const double shiftSource = (newStart - o->start) / beatsPerSourceSecond;
                c->start  = newStart;
                c->offset = o->offset + shiftSource;
                c->length = o->length - shiftSource;
            }
            else
            {
                newStart = std::max ({ newStart, o->start - o->offset, 0.0 });
                c->start  = newStart;
                c->offset = o->offset + (newStart - o->start);
                c->length = o->length - (newStart - o->start);
            }
            drag.moved = true;
            break;
        }

        case DragState::Mode::stretchRight:
        {
            auto* c = project.find (drag.clipId);
            auto* o = original (drag.clipId);
            if (c == nullptr || o == nullptr || o->length <= 0.0) return;
            const double minBeats = std::max (snapValue (e.mods), 1.0 / 64.0);
            const double end = std::max (o->start + minBeats, snap (h.beat, e.mods));
            c->stretch = juce::jlimit (0.1, 10.0, (end - o->start) * spb / o->length);
            drag.moved = true;
            break;
        }

        case DragState::Mode::stretchLeft:
        {
            auto* c = project.find (drag.clipId);
            auto* o = original (drag.clipId);
            if (c == nullptr || o == nullptr || o->length <= 0.0) return;
            const double originalEnd = o->endBeat (project.bpm);
            const double minBeats = std::max (snapValue (e.mods), 1.0 / 64.0);
            const double newStart = std::max (0.0, std::min (snap (h.beat, e.mods), originalEnd - minBeats));
            c->stretch = juce::jlimit (0.1, 10.0, (originalEnd - newStart) * spb / o->length);
            c->start   = originalEnd - c->lengthBeats (project.bpm);
            drag.moved = true;
            break;
        }
    }

    project.changed();
    repaint();
}

void PlaylistComponent::mouseUp (const juce::MouseEvent& e)
{
    if (drag.mode == DragState::Mode::marquee)
    {
        if (drag.ctrlClick)
        {
            if (drag.clipId != 0)
            {
                if (project.selection.count (drag.clipId)) project.selection.erase (drag.clipId);
                else                                        project.selection.insert (drag.clipId);
            }
        }
        else
        {
            const double a = std::min (drag.beat0, drag.beat1), b = std::max (drag.beat0, drag.beat1);
            const int t0 = std::min (drag.trackA, drag.trackB), t1 = std::max (drag.trackA, drag.trackB);
            if (! e.mods.isShiftDown())
                project.selection.clear();
            for (auto& c : project.clips)
                if (c.track >= t0 && c.track <= t1 && c.start < b && c.endBeat (project.bpm) > a)
                    project.selection.insert (c.id);
        }
    }
    else if (drag.mode == DragState::Mode::move && ! drag.moved && ! e.mods.isShiftDown())
    {
        project.selection = { drag.clipId };
    }

    const bool edited = drag.moved;
    drag = {};
    if (edited)
        project.changed();   // now the stretch can render
    updateScrollBars();
    updateClipBar();
    repaint();
}

void PlaylistComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto h = hitTest (e.position);

    if (h.zone == Zone::grid && h.clipId != 0)
    {
        if (auto* c = project.find (h.clipId); c != nullptr && ! c->isAudio() && onOpenPianoRoll)
            onOpenPianoRoll (c->id);
        return;
    }

    if (h.zone == Zone::grid && h.clipId == 0 && ! e.mods.isAnyModifierKeyDown())
    {
        if (onPlayFrom)
            onPlayFrom (std::max (0.0, snap (h.beat, e.mods, true)));
        return;
    }

    if (h.zone != Zone::header || ! juce::isPositiveAndBelow (h.track, (int) project.tracks.size()))
        return;
    if (e.x < 26 || e.x > headerW - 34)
        return;

    auto* editor = new juce::AlertWindow ("Rename track", {}, juce::MessageBoxIconType::NoIcon);
    editor->addTextEditor ("name", project.tracks[(size_t) h.track].name);
    editor->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    editor->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    const int track = h.track;
    juce::Component::SafePointer<PlaylistComponent> safeThis (this);
    editor->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, editor, track] (int result)
    {
        if (result == 1 && safeThis != nullptr)
        {
            const auto name = editor->getTextEditorContents ("name").trim().substring (0, 24);
            if (name.isNotEmpty())
            {
                safeThis->project.tracks[(size_t) track].name = name;
                safeThis->project.changed();
                safeThis->repaint();
            }
        }
    }), true);
}

void PlaylistComponent::showTrackMenu (int track)
{
    juce::PopupMenu route;
    const int current = project.tracks[(size_t) track].insert;
    route.addItem (1000, "Master", true, current == 0);
    for (int i = 1; i < kNumInserts; ++i)
        route.addItem (1000 + i, engine.insert (i).name, true, current == i);

    juce::PopupMenu menu;
    menu.addSectionHeader (project.tracks[(size_t) track].name);
    menu.addSubMenu ("Send audio to", route);
    menu.addItem (1, project.tracks[(size_t) track].armed ? "Disarm recording" : "Arm for recording");
    menu.addItem (2, project.tracks[(size_t) track].muted ? "Unmute" : "Mute");

    juce::Component::SafePointer<PlaylistComponent> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options(), [safeThis, track] (int result)
    {
        if (safeThis == nullptr || result == 0)
            return;
        auto& tr = safeThis->project.tracks[(size_t) track];
        if (result >= 1000)
        {
            tr.insert = result - 1000;
            if (safeThis->onRouteTrack)
                safeThis->onRouteTrack (track);
        }
        else if (result == 1) tr.armed = ! tr.armed;
        else if (result == 2) tr.muted = ! tr.muted;
        safeThis->project.changed();
        safeThis->repaint();
    });
}

void PlaylistComponent::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCtrlDown() || e.mods.isCommandDown())
    {
        zoom (w.deltaY > 0 ? 1.15 : 1.0 / 1.15, e.position.x);
        return;
    }

    if (e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY))
    {
        const float d = std::abs (w.deltaX) > 0.0f ? w.deltaX : w.deltaY;
        scrollBeats = std::max (0.0, scrollBeats - d * 300.0 / ppb);
    }
    else
    {
        scrollY -= w.deltaY * 300.0f;
    }
    updateScrollBars();
    repaint();
}

// ---------------------------------------------------------------------------
// Editing commands

void PlaylistComponent::deleteSelection()
{
    project.removeSelected();
    updateClipBar();
    repaint();
}

void PlaylistComponent::duplicateSelection()
{
    project.duplicateSelected();
    updateScrollBars();
    updateClipBar();
    repaint();
}

void PlaylistComponent::copySelection()
{
    auto picked = project.selectedClips();
    if (picked.empty())
        return;

    double first = 1e12;
    clipboardFirstTrack = 1 << 20;
    for (auto& c : picked)
    {
        first = std::min (first, c.start);
        clipboardFirstTrack = std::min (clipboardFirstTrack, c.track);
    }

    clipboard.clear();
    for (auto& c : picked)
    {
        auto copy = c.deepCopy();
        copy.start -= first;
        clipboard.push_back (copy);
    }
}

void PlaylistComponent::cutSelection()
{
    copySelection();
    deleteSelection();
}

void PlaylistComponent::paste()
{
    if (clipboard.empty())
        return;

    // Paste where the mouse is; otherwise at the playhead or start marker
    double at = engine.isPlaying() ? snap (engine.getBeatPosition(), {}, true) : engine.getSongStart();
    int trackShift = 0;
    if (mouseOverGrid)
    {
        at = hoverBeat;
        if (hoverTrack >= 0)
            trackShift = hoverTrack - clipboardFirstTrack;
    }

    int maxTrack = 0;
    for (auto& c : clipboard) maxTrack = std::max (maxTrack, c.track);
    trackShift = juce::jlimit (-clipboardFirstTrack, (int) project.tracks.size() - 1 - maxTrack, trackShift);

    std::vector<Clip> placed = clipboard;
    for (auto& c : placed)
        c.track += trackShift;

    project.paste (placed, at);
    updateScrollBars();
    updateClipBar();
    repaint();
}

void PlaylistComponent::selectAll()
{
    project.selection.clear();
    for (auto& c : project.clips)
        project.selection.insert (c.id);
    updateClipBar();
    repaint();
}

void PlaylistComponent::addFiles (const juce::StringArray& paths, double beat, int track)
{
    juce::StringArray problems;
    project.selection.clear();

    for (const auto& path : paths)
    {
        if (track >= (int) project.tracks.size())
            break;

        juce::File file (path);
        if (! cache.isAudioFile (file))
            continue;

        juce::String error;
        auto sample = cache.load (file, error);
        if (sample == nullptr)
        {
            problems.add (error);
            continue;
        }

        Clip c;
        c.type   = ClipType::audio;
        c.sample = sample;
        c.track  = track++;
        c.start  = beat;
        c.length = sample->durationSeconds();
        project.selection.insert (project.addClip (c));
    }

    updateScrollBars();
    updateClipBar();
    repaint();

    if (! problems.isEmpty())
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::WarningIcon)
                                          .withTitle ("Some files couldn't be added")
                                          .withMessage (problems.joinIntoString ("\n"))
                                          .withButton ("OK"),
                                      nullptr);
}

void PlaylistComponent::setDropHint (int x, int y)
{
    const auto h = hitTest ({ (float) x, (float) y });
    showDrop  = h.zone == Zone::grid;
    dropBeat  = std::max (0.0, snap (h.beat, juce::ModifierKeys::getCurrentModifiers(), true));
    dropTrack = juce::jlimit (0, (int) project.tracks.size() - 1, h.track);
    repaint();
}

bool PlaylistComponent::isInterestedInDragSource (const SourceDetails& d)
{
    return d.description.toString() == "ahp-sample";
}

void PlaylistComponent::itemDragMove (const SourceDetails& d)   { setDropHint (d.localPosition.x, d.localPosition.y); }
void PlaylistComponent::itemDragExit (const SourceDetails&)     { showDrop = false; repaint(); }

void PlaylistComponent::itemDropped (const SourceDetails& d)
{
    setDropHint (d.localPosition.x, d.localPosition.y);
    showDrop = false;

    if (auto* tree = dynamic_cast<juce::FileTreeComponent*> (d.sourceComponent.get()))
    {
        juce::StringArray paths;
        for (int i = 0; i < tree->getNumSelectedFiles(); ++i)
            paths.add (tree->getSelectedFile (i).getFullPathName());
        addFiles (paths, dropBeat, dropTrack);
    }
}

bool PlaylistComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (cache.isAudioFile (juce::File (f)))
            return true;
    return false;
}

void PlaylistComponent::fileDragMove (const juce::StringArray&, int x, int y) { setDropHint (x, y); }
void PlaylistComponent::fileDragExit (const juce::StringArray&)               { showDrop = false; repaint(); }

void PlaylistComponent::filesDropped (const juce::StringArray& files, int x, int y)
{
    setDropHint (x, y);
    showDrop = false;
    addFiles (files, dropBeat, dropTrack);
}
