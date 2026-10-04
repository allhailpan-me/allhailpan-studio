#include "PlaylistComponent.h"
#include "AhpLookAndFeel.h"
#include "Logo.h"
#include "MidiFileIO.h"
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
    for (auto* l : { &pitchLabel, &stretchLabel, &gainLabel, &bpmLabel })
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

    setupSlider (sourceBpmSlider);
    sourceBpmSlider.setRange (40.0, 240.0, 0.01);
    sourceBpmSlider.textFromValueFunction = [] (double v) { return juce::String (v, 2); };
    sourceBpmSlider.valueFromTextFunction = [] (const juce::String& t) { return t.retainCharacters (".0123456789").getDoubleValue(); };
    sourceBpmSlider.onValueChange = [this]
    {
        if (updatingClipBar) return;
        if (auto* c = singleSelected(); c != nullptr && c->isAudio())
        {
            c->sourceBpm = sourceBpmSlider.getValue();
            if (c->followTempo && project.bpm > 0.0)
                c->stretch = c->sourceBpm / project.bpm;
            project.changed();
            updateClipBar();
        }
    };

    syncButton.setClickingTogglesState (true);
    syncButton.setTooltip ("Stretch this clip to the project tempo, and keep it there if the tempo changes. "
                           "Pitch is unaffected.");
    syncButton.onClick = [this]
    {
        if (updatingClipBar) return;
        auto* c = singleSelected();
        if (c == nullptr || ! c->isAudio())
            return;

        c->followTempo = syncButton.getToggleState();

        if (c->followTempo)
        {
            // Nothing recorded yet means nothing to match to, so a loop's own
            // tempo is worked out from its length before falling back to the
            // project's.
            if (c->sourceBpm <= 0.0)
            {
                const double guessed = guessSourceBpm (*c);
                c->sourceBpm = guessed > 0.0 ? guessed : project.bpm;
            }

            if (project.bpm > 0.0)
                c->stretch = c->sourceBpm / project.bpm;
        }

        project.changed();
        updateClipBar();
    };

    resetButton.onClick = [this]
    {
        auto* c = singleSelected();
        if (c == nullptr)
            return;

        if (c->isAudio())
        {
            c->pitch = 0.0;
            c->stretch = 1.0;
            c->gainDb = 0.0f;
            c->followTempo = false;
            c->sourceBpm = 0.0;
            c->warp.clear();
        }
        else if (c->pattern != nullptr)
        {
            c->pattern->lanes.clear();     // drop recorded knob moves
        }
        project.changed();
        updateClipBar();
    };
    clearWarpButton.setTooltip ("Remove every warp marker from this clip, keeping the length it has now.");
    clearWarpButton.onClick = [this]
    {
        if (auto* c = singleSelected(); c != nullptr && c->isWarped())
        {
            project.clearWarp (c->id);
            updateClipBar();
            repaint();
        }
    };

    openRollButton.onClick = [this]
    {
        if (auto* c = singleSelected(); c != nullptr && c->isMidi() && onOpenPianoRoll)
            onOpenPianoRoll (c->id);
    };
    channelBox.onChange = [this]
    {
        if (updatingClipBar) return;
        if (auto* c = singleSelected(); c != nullptr && c->isMidi())
        {
            c->channel = channelBox.getSelectedId() - 1;
            project.changed();
        }
    };

    for (auto* comp : { static_cast<juce::Component*> (&clipName), static_cast<juce::Component*> (&clipStatus),
                        static_cast<juce::Component*> (&pitchLabel), static_cast<juce::Component*> (&stretchLabel),
                        static_cast<juce::Component*> (&gainLabel), static_cast<juce::Component*> (&resetButton),
                        static_cast<juce::Component*> (&bpmLabel), static_cast<juce::Component*> (&syncButton),
                        static_cast<juce::Component*> (&openRollButton), static_cast<juce::Component*> (&channelBox),
                        static_cast<juce::Component*> (&clearWarpButton) })
    {
        quiet (*comp);
        addChildComponent (comp);
    }
    addChildComponent (pitchSlider);
    addChildComponent (stretchSlider);
    addChildComponent (gainSlider);
    addChildComponent (sourceBpmSlider);

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
        || drag.mode == DragState::Mode::stretchLeft || drag.mode == DragState::Mode::stretchRight
        || drag.mode == DragState::Mode::warp
        || drag.mode == DragState::Mode::autoPoint || drag.mode == DragState::Mode::autoBend;
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
    pitchSlider .setBounds (r.removeFromLeft (150));  r.removeFromLeft (10);
    stretchLabel.setBounds (r.removeFromLeft (52));
    stretchSlider.setBounds (r.removeFromLeft (150)); r.removeFromLeft (10);
    bpmLabel    .setBounds (r.removeFromLeft (74));
    sourceBpmSlider.setBounds (r.removeFromLeft (110)); r.removeFromLeft (6);
    syncButton  .setBounds (r.removeFromLeft (104));  r.removeFromLeft (10);
    gainLabel   .setBounds (r.removeFromLeft (34));
    gainSlider  .setBounds (r.removeFromLeft (140));  r.removeFromLeft (10);
    resetButton .setBounds (r.removeFromLeft (60));   r.removeFromLeft (10);
    if (clearWarpButton.isVisible())
    {
        clearWarpButton.setBounds (r.removeFromLeft (86));
        r.removeFromLeft (10);
    }

    // MIDI clip controls reuse the same row
    auto midiRow = pitchLabel.getBounds().getUnion (pitchSlider.getBounds());
    channelBox.setBounds (midiRow.withWidth (220));
    openRollButton.setBounds (stretchLabel.getBounds().getUnion (stretchSlider.getBounds()).withWidth (140));
    if (! pitchSlider.isVisible())
        resetButton.setBounds (gainLabel.getBounds().getUnion (gainSlider.getBounds()).withTrimmedLeft (210).withWidth (90));

    clipStatus.setBounds (r);
}

Clip* PlaylistComponent::singleSelected()
{
    if (project.selection.size() != 1)
        return nullptr;
    return project.find (*project.selection.begin());
}

double PlaylistComponent::guessSourceBpm (const Clip& clip) const
{
    // Loops are almost always a whole number of bars, so trying each likely bar
    // count identifies the tempo of most material without analysing the audio.
    //
    // Length alone is ambiguous: the same clip could be four bars at 110 or two
    // at 55. The project's own tempo breaks the tie, because audio being matched
    // to a session is nearly always near that session's tempo, or an exact
    // multiple of it. Comparing in log space judges half and double time
    // even-handedly rather than favouring the faster reading.
    if (clip.sample == nullptr || clip.length <= 0.0)
        return 0.0;

    const double seconds = clip.length;        // source seconds, before stretching
    const int    barCounts[] { 1, 2, 4, 8, 16, 32, 64 };
    const double anchor = project.bpm > 0.0 ? project.bpm : 120.0;

    double best = 0.0, bestDistance = 1.0e9;

    for (int bars : barCounts)
    {
        const double candidate = bars * 4.0 * 60.0 / seconds;

        if (candidate < 60.0 || candidate > 200.0)
            continue;

        const double distance = std::abs (std::log (candidate / anchor));
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = candidate;
        }
    }

    return best;
}

void PlaylistComponent::updateClipBar()
{
    const juce::ScopedValueSetter<bool> svs (updatingClipBar, true);
    auto* c = singleSelected();
    const bool audio = c != nullptr && c->isAudio();
    const bool midi  = c != nullptr && c->isMidi();

    clipName.setVisible (c != nullptr);
    clipStatus.setVisible (c != nullptr || project.selection.size() > 1);
    for (auto* comp : { static_cast<juce::Component*> (&pitchLabel), static_cast<juce::Component*> (&pitchSlider),
                        static_cast<juce::Component*> (&stretchLabel), static_cast<juce::Component*> (&stretchSlider),
                        static_cast<juce::Component*> (&bpmLabel), static_cast<juce::Component*> (&sourceBpmSlider),
                        static_cast<juce::Component*> (&syncButton) })
        comp->setVisible (audio);

    clearWarpButton.setVisible (audio && c->isWarped());
    resetButton.setVisible (audio || (midi && c->pattern != nullptr && ! c->pattern->lanes.empty()));
    resetButton.setButtonText (audio ? "Reset" : "Clear auto");
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

        sourceBpmSlider.setValue (c->sourceBpm > 0.0 ? c->sourceBpm : project.bpm, juce::dontSendNotification);
        syncButton.setToggleState (c->followTempo, juce::dontSendNotification);

        // Warp markers and a single ratio describe the same thing two ways, and
        // the markers win: a clip with markers has its timing set by them, so
        // the ratio and the tempo match are not the clip's own any more.
        const bool warped = c->isWarped();
        stretchSlider.setEnabled (! c->followTempo && ! warped);
        sourceBpmSlider.setEnabled (! warped);
        syncButton.setEnabled (! warped);

        const bool rendering = isRendering && isRendering (*c);
        juce::String status = rendering
            ? juce::String ("Rendering high-quality stretch...")
            : "Starts " + formatPosition (c->start) + "   Length "
                  + juce::String (c->lengthBeats (project.bpm) / 4.0, 3) + " bars";
        if (warped)
            status << "   " << (int) c->warp.size() << (c->warp.size() == 1 ? " warp marker" : " warp markers");
        else if (audio)
            status << "   Double-click the clip to add a warp marker";
        clipStatus.setText (status, juce::dontSendNotification);
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

    // Which controls are on the row has just changed, so the row has to be
    // laid out again rather than waiting for the next resize.
    layoutClipBar (clipBar);
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
        if (c.isWarped())                          label << "   warped";
        else if (std::abs (c.stretch - 1.0) > 1e-4) label << "   x" << juce::String (c.stretch, 2);
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
        const double beatsPerPixel = 1.0 / ppb;
        const double peaksPerSecond = c.sample->sampleRate / SampleData::peakBlock;
        const float gain = juce::Decibels::decibelsToGain (c.gainDb);
        const double fallback = c.fallbackSlope (project.bpm);

        // The waveform is drawn through the warp map, so a transient appears
        // under the grid line it has been pinned to. A waveform that disagreed
        // with what is heard would make markers impossible to place.
        //
        // Through the same segment table the audio thread reads, walked with
        // the same cursor, rather than the marker list: a wide clip with a
        // marker on every beat would otherwise search the whole list once per
        // pixel, on every repaint, which the playhead does constantly. An
        // unwarped clip builds no table and allocates nothing.
        std::vector<WarpSegment> segments;
        if (c.isWarped())
            buildWarpSegments (c.warp, c.offset, fallback, 0.0, segments);

        int cursor = 0;
        auto sourceAt = [&] (double beatsIn)
        {
            return segments.empty()
                 ? c.offset + beatsIn * fallback
                 : warpSourceAtSegment (segments.data(), (int) segments.size(), cursor, beatsIn);
        };

        g.setColour (Ahp::bone.withAlpha (dim ? 0.35f : 0.9f));
        double t0 = sourceAt ((x0 - r.getX()) * beatsPerPixel);

        for (int x = x0; x < x1; ++x)
        {
            const double t1 = sourceAt ((x + 1 - r.getX()) * beatsPerPixel);
            size_t i0 = (size_t) std::max (0.0, t0 * peaksPerSecond);
            size_t i1 = std::max (i0 + 1, (size_t) std::max (0.0, t1 * peaksPerSecond));
            float v = 0.0f;
            for (size_t i = i0; i < std::min (i1, peaks.size()); ++i)
                v = std::max (v, peaks[i]);
            v = std::min (1.0f, v * gain);
            g.fillRect ((float) x, midY - v * amp, 1.0f, std::max (1.0f, v * amp * 2.0f));
            t0 = t1;
        }
    }
    else if (c.isMidi() && c.pattern != nullptr && ! c.pattern->notes.empty())
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
    else if (c.isAutomation())
    {
        paintCurve (g, c, curveBody (r), selected, dim);
    }

    if (c.isMidi() && c.pattern != nullptr && ! c.pattern->lanes.empty())
    {
        g.setColour (Ahp::bone.withAlpha (0.5f));
        g.setFont (juce::FontOptions (9.5f));
        g.drawText (juce::String ((int) c.pattern->lanes.size()) + " auto", r.withTrimmedRight (4.0f).withTrimmedTop (15.0f),
                    juce::Justification::topRight);
    }

    // Warp markers, on top of the waveform they pin.
    if (warpVisible (c))
    {
        for (size_t i = 0; i < c.warp.size(); ++i)
        {
            const float mx = warpMarkerX (c, (int) i);
            if (mx < r.getX() || mx > r.getRight())
                continue;

            g.setColour (Ahp::rec);
            g.fillRect (mx - 0.5f, bodyTop, 1.0f, bodyH);

            // A grab handle at the top of the body, wide enough to hit without
            // aiming, and clear of the clip's name in the head.
            g.fillRect (mx - 3.5f, bodyTop, 7.0f, 5.0f);
        }
    }

    if (selected)
    {
        g.setColour (Ahp::bone);
        g.drawRect (r.reduced (1.0f, 0.0f), 1.5f);
    }
}

bool PlaylistComponent::warpVisible (const Clip& c) const
{
    return c.isWarped() && project.selection.count (c.id) > 0;
}

// A point dragged outside its own clip's window would be off the part of the
// curve that is drawn, so it could never be seen or grabbed again. The window
// is where points stay, and trimming the clip is how to change which part of
// the curve is on show.
static double clampToCurveWindow (const Clip& c, double beatsIn)
{
    return juce::jlimit (c.offset, c.offset + std::max (0.0, c.length), beatsIn);
}

// ---------------------------------------------------------------------------
// Automation curves on the playlist.
//
// A curve clip is drawn as the curve itself rather than as a block, because
// the shape is the content: a producer reads a filter sweep by its slope, and
// a rectangle with a name in it would say nothing about what the parameter is
// doing.
//
// Its handles only appear once the clip is selected. That is the same rule the
// warp markers follow, and it is what keeps the clip draggable: an unselected
// curve behaves like any other clip, and the body only becomes an editing
// surface once the producer has said which clip they mean. The name strip
// stays a grab handle either way, so a selected curve can still be moved.

juce::Rectangle<float> PlaylistComponent::curveBody (juce::Rectangle<float> clipRect) const
{
    return clipRect.withTrimmedTop (15.0f).reduced (1.0f, 1.0f);
}

bool PlaylistComponent::curveVisible (const Clip& c) const
{
    return c.isAutomation() && c.curve != nullptr && project.selection.count (c.id) > 0;
}

float PlaylistComponent::curveValueToY (juce::Rectangle<float> body, double value) const
{
    const double v = juce::jlimit (0.0, 1.0, value);
    return body.getBottom() - (float) v * body.getHeight();
}

double PlaylistComponent::curveYToValue (juce::Rectangle<float> body, float y) const
{
    if (body.getHeight() <= 0.0f)
        return 0.0;
    return juce::jlimit (0.0, 1.0, (double) ((body.getBottom() - y) / body.getHeight()));
}

float PlaylistComponent::curvePointX (const Clip& c, int index) const
{
    if (c.curve == nullptr || ! juce::isPositiveAndBelow (index, (int) c.curve->points.size()))
        return 0.0f;

    // The clip's offset moves the window onto the curve, exactly as it moves
    // the window onto a pattern, so trimming the left edge scrolls the curve
    // rather than rewriting its points.
    return beatToX (c.start - c.offset + c.curve->points[(size_t) index].beat);
}

juce::Point<float> PlaylistComponent::curveBendHandle (const Clip& c, juce::Rectangle<float> body, int segment) const
{
    if (c.curve == nullptr || segment < 0 || (size_t) segment + 1 >= c.curve->points.size())
        return {};

    const auto& a = c.curve->points[(size_t) segment];
    const auto& b = c.curve->points[(size_t) segment + 1];

    // Halfway along in beats, and on the curve rather than on the chord
    // between the points, so the handle is always somewhere the curve actually
    // goes and dragging it looks like bending the line under the mouse.
    const double midBeat = (a.beat + b.beat) * 0.5;
    const double value   = c.curve->valueAt (midBeat);

    return { beatToX (c.start - c.offset + midBeat), curveValueToY (body, value) };
}

void PlaylistComponent::paintCurve (juce::Graphics& g, const Clip& c, juce::Rectangle<float> body,
                                    bool selected, bool dim)
{
    if (c.curve == nullptr || body.getWidth() <= 0.0f || body.getHeight() <= 0.0f)
        return;

    const auto& points = c.curve->points;
    const float alpha  = dim ? 0.35f : 1.0f;

    // A horizontal rule where the control's own resting position is: 0 dB on a
    // fader, centre on a pan. Without it a fader curve cannot be drawn back to
    // where it started, since nothing on the clip says which height that is.
    // A plugin parameter has no such position, so it gets no rule rather than
    // a meaningless one at the top of the clip.
    const auto kind = c.curve->target.kind;
    if (kind == AutoTargetKind::insertVolume || kind == AutoTargetKind::insertPan)
    {
        const double rest = autoCurveValueFor (kind, kind == AutoTargetKind::insertVolume
                                                       ? (double) kUnityFaderGain : 0.0);
        g.setColour (Ahp::muted.withAlpha (0.3f * alpha));
        g.fillRect (body.getX(), curveValueToY (body, rest) - 0.5f, body.getWidth(), 1.0f);
    }

    // The curve itself, a pixel at a time, read through the same function the
    // engine plays it with. Drawing it from anything else would let the picture
    // and the sound disagree, which is the one thing a curve must not do.
    const int x0 = (int) std::max (body.getX(), (float) grid.getX());
    const int x1 = (int) std::min (body.getRight(), (float) grid.getRight());
    const double origin = c.start - c.offset;

    if (x1 > x0)
    {
        juce::Path path;
        for (int x = x0; x <= x1; ++x)
        {
            const double beat = xToBeat ((float) x) - origin;
            const float  y    = curveValueToY (body, autoCurveValueAt (points, beat));
            if (x == x0) path.startNewSubPath ((float) x, y);
            else         path.lineTo ((float) x, y);
        }

        // Filled under the line as well as stroked, because at this height a
        // one pixel line is hard to follow across a busy arrangement.
        auto filled = path;
        filled.lineTo ((float) x1, body.getBottom());
        filled.lineTo ((float) x0, body.getBottom());
        filled.closeSubPath();

        g.setColour (Ahp::bone.withAlpha (0.12f * alpha));
        g.fillPath (filled);

        g.setColour (Ahp::bone.withAlpha (0.9f * alpha));
        g.strokePath (path, juce::PathStrokeType (1.4f));
    }

    if (! curveVisible (c))
        return;

    // Bend handles first, so a point sitting on top of one stays grabbable.
    g.setColour (Ahp::rec.withAlpha (0.75f));
    for (size_t i = 0; i + 1 < points.size(); ++i)
    {
        const auto h = curveBendHandle (c, body, (int) i);
        if (h.x < body.getX() - 4.0f || h.x > body.getRight() + 4.0f)
            continue;
        g.drawEllipse (h.x - 3.0f, h.y - 3.0f, 6.0f, 6.0f, 1.2f);
    }

    for (size_t i = 0; i < points.size(); ++i)
    {
        const float px = curvePointX (c, (int) i);
        if (px < body.getX() - 4.0f || px > body.getRight() + 4.0f)
            continue;

        const float py = curveValueToY (body, points[i].value);
        g.setColour (Ahp::rec);
        g.fillRect (px - 3.0f, py - 3.0f, 6.0f, 6.0f);
    }

    juce::ignoreUnused (selected);
}

double PlaylistComponent::currentValueFor (const AutoTarget& t) const
{
    switch (t.kind)
    {
        case AutoTargetKind::insertVolume:
            return autoCurveValueFor (t.kind, (double) engine.insert (t.insert).volume.load());

        case AutoTargetKind::insertPan:
            return autoCurveValueFor (t.kind, (double) engine.insert (t.insert).pan.load());

        case AutoTargetKind::insertFxParam:
            if (auto* fx = engine.getFx (t.insert, t.fxSlot))
            {
                const auto& params = fx->getParameters();
                if (juce::isPositiveAndBelow (t.paramIndex, params.size()))
                    return (double) params[t.paramIndex]->getValue();
            }
            return 0.0;

        case AutoTargetKind::channelParam:
            if (auto* plugin = engine.getChannelPlugin (t.channel))
            {
                const auto& params = plugin->getParameters();
                if (juce::isPositiveAndBelow (t.paramIndex, params.size()))
                    return (double) params[t.paramIndex]->getValue();
            }
            return 0.0;
    }
    return 0.0;
}

void PlaylistComponent::addAutomationFor (int track, const AutoTarget& target)
{
    if (! juce::isPositiveAndBelow (track, (int) project.tracks.size()))
        return;

    // Put at the left of what is on screen, rounded down to a bar, because
    // that is the part of the arrangement the producer is looking at. Four
    // bars long, which is long enough to draw a gesture in and short enough to
    // trim rather than having to cut down.
    const double start  = std::max (0.0, std::floor (scrollBeats / 4.0) * 4.0);
    const double length = 16.0;

    // A curve already covering this stretch of the arrangement for the same
    // control is selected rather than buried under a second one. Another curve
    // for the same control somewhere else is fine and expected, which is why
    // this asks about the span and not just the target.
    if (auto* existing = project.automationClipFor (target, start, start + length))
    {
        project.selection = { existing->id };
        if (onStatus)
            onStatus (autoTargetName (target, project.channels) + " is already automated here, on "
                        + project.tracks[(size_t) existing->track].name);
        updateClipBar();
        repaint();
        return;
    }

    const int id = project.addAutomationClip (target, track, start, length, currentValueFor (target));
    project.selection = { id };

    if (onStatus)
        onStatus ("Automating " + autoTargetName (target, project.channels)
                    + ". Click the curve to add points, drag the circles to bend it.");

    updateClipBar();
    repaint();
}

void PlaylistComponent::showAutomationMenu (int track)
{
    automationMenu.clear();

    // Named parameters are capped: a synth with a thousand of them would make
    // a menu nobody can use, and the ones worth automating are near the front
    // in every plugin that bothers to order them.
    constexpr int maxParams = 48;

    const auto parameterSubMenu = [this, maxParams] (juce::AudioPluginInstance* plugin, AutoTarget base)
    {
        juce::PopupMenu sub;
        if (plugin == nullptr)
            return sub;

        const auto& params = plugin->getParameters();
        for (int i = 0; i < std::min (params.size(), maxParams); ++i)
        {
            if (! params[i]->isAutomatable())
                continue;
            base.paramIndex = i;
            base.paramName  = params[i]->getName (28);
            if (base.paramName.isEmpty())
                base.paramName = "Parameter " + juce::String (i + 1);

            automationMenu.push_back (base);
            sub.addItem (2000 + (int) automationMenu.size() - 1, base.paramName);
        }
        return sub;
    };

    const int insertIndex = juce::jlimit (0, kNumInserts - 1, project.tracks[(size_t) track].insert);

    juce::PopupMenu mixer;
    {
        AutoTarget volume;
        volume.kind   = AutoTargetKind::insertVolume;
        volume.insert = insertIndex;
        automationMenu.push_back (volume);
        mixer.addItem (2000 + (int) automationMenu.size() - 1, "Volume");

        AutoTarget pan;
        pan.kind   = AutoTargetKind::insertPan;
        pan.insert = insertIndex;
        automationMenu.push_back (pan);
        mixer.addItem (2000 + (int) automationMenu.size() - 1, "Pan");

        for (int slot = 0; slot < kNumFxSlots; ++slot)
        {
            auto* fx = engine.getFx (insertIndex, slot);
            if (fx == nullptr)
                continue;

            AutoTarget base;
            base.kind   = AutoTargetKind::insertFxParam;
            base.insert = insertIndex;
            base.fxSlot = slot;

            auto sub = parameterSubMenu (fx, base);
            if (sub.containsAnyActiveItems())
                mixer.addSubMenu (fx->getName(), sub);
        }
    }

    juce::PopupMenu instruments;
    for (int channel = 0; channel < kNumChannels; ++channel)
    {
        auto* plugin = engine.getChannelPlugin (channel);
        if (plugin == nullptr)
            continue;

        AutoTarget base;
        base.kind    = AutoTargetKind::channelParam;
        base.channel = channel;

        auto sub = parameterSubMenu (plugin, base);
        if (sub.containsAnyActiveItems())
            instruments.addSubMenu (juce::String (channel + 1) + "  " + project.channels[(size_t) channel].name, sub);
    }

    juce::PopupMenu menu;
    menu.addSectionHeader (insertIndex == 0 ? juce::String ("Master")
                                            : engine.insert (insertIndex).name);
    menu.addSubMenu ("Mixer", mixer, mixer.containsAnyActiveItems());
    menu.addSubMenu ("Instruments", instruments, instruments.containsAnyActiveItems());

    juce::Component::SafePointer<PlaylistComponent> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options(), [safeThis, track] (int result)
    {
        if (safeThis == nullptr || result < 2000)
            return;

        const size_t index = (size_t) (result - 2000);
        if (index < safeThis->automationMenu.size())
            safeThis->addAutomationFor (track, safeThis->automationMenu[index]);
    });
}

float PlaylistComponent::warpMarkerX (const Clip& c, int index) const
{
    if (! juce::isPositiveAndBelow (index, (int) c.warp.size()))
        return 0.0f;
    return beatToX (c.start + c.warp[(size_t) index].beat);
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

            // A marker takes precedence over the trim edges, because one sitting
            // near the end of a clip would otherwise be impossible to grab.
            if (warpVisible (*it))
                for (size_t i = 0; i < it->warp.size(); ++i)
                    if (std::abs (p.x - warpMarkerX (*it, (int) i)) < 5.0f)
                    {
                        h.warpIndex = (int) i;
                        break;
                    }

            // A curve's handles, likewise, take precedence over the trim
            // edges, and only on a selected clip. The name strip is left out
            // so that it stays a grab handle for moving the clip.
            if (curveVisible (*it) && p.y > r.getY() + 15.0f)
            {
                const auto body = curveBody (r);
                const auto& points = it->curve->points;

                for (size_t i = 0; i < points.size(); ++i)
                    if (std::abs (p.x - curvePointX (*it, (int) i)) < 5.0f
                        && std::abs (p.y - curveValueToY (body, points[i].value)) < 6.0f)
                    {
                        h.autoPoint = (int) i;
                        break;
                    }

                if (h.autoPoint < 0)
                    for (size_t i = 0; i + 1 < points.size(); ++i)
                    {
                        const auto handle = curveBendHandle (*it, body, (int) i);
                        if (p.getDistanceFrom (handle) < 6.0f)
                        {
                            h.autoBend = (int) i;
                            break;
                        }
                    }
            }

            if (h.warpIndex < 0 && h.autoPoint < 0 && h.autoBend < 0 && r.getWidth() > 18.0f)
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
        else if (h.warpIndex >= 0)           cursor = juce::MouseCursor::LeftRightResizeCursor;
        else if (h.autoPoint >= 0)           cursor = juce::MouseCursor::DraggingHandCursor;
        else if (h.autoBend >= 0)            cursor = juce::MouseCursor::UpDownResizeCursor;
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

    // A warp marker is grabbed before anything else, so dragging one never
    // moves or duplicates the clip by accident. Only a selected clip shows its
    // markers, so the clip is already selected here. Alt-click removes one,
    // which leaves double-click free for adding one.
    if (h.warpIndex >= 0)
    {
        if (e.mods.isAltDown())
        {
            project.removeWarpMarker (clip->id, h.warpIndex);
            updateClipBar();
            repaint();
            return;
        }

        drag.mode      = DragState::Mode::warp;
        drag.clipId    = clip->id;
        drag.warpIndex = h.warpIndex;
        repaint();
        return;
    }

    // A curve's handles, on the same principle: the clip is already selected
    // here, since nothing else shows them. Alt-click removes a point, which
    // leaves plain clicking free for drawing one.
    if (h.autoPoint >= 0)
    {
        if (e.mods.isAltDown())
        {
            project.removeCurvePoint (clip->id, h.autoPoint);
            repaint();
            return;
        }

        drag.mode       = DragState::Mode::autoPoint;
        drag.clipId     = clip->id;
        drag.pointIndex = h.autoPoint;
        repaint();
        return;
    }

    if (h.autoBend >= 0)
    {
        drag.mode       = DragState::Mode::autoBend;
        drag.clipId     = clip->id;
        drag.pointIndex = h.autoBend;
        drag.bendY0     = e.position.y;
        drag.bend0      = clip->curve != nullptr
                            ? clip->curve->points[(size_t) h.autoBend].bend : 0.0;
        repaint();
        return;
    }

    // Clicking the body of a selected curve draws a point there and starts
    // dragging it, so adding one and placing it are a single gesture rather
    // than a click followed by a drag. The name strip is excluded, which is
    // what keeps a selected curve movable.
    // Shift is left out so that shift+drag still duplicates a selection from
    // anywhere on the clip, rather than only from its name strip.
    if (clip->isAutomation() && curveVisible (*clip) && h.edge == Edge::none
        && tool == Tool::draw && ! e.mods.isPopupMenu() && ! e.mods.isShiftDown()
        && e.position.y > clipBounds (*clip).getY() + 15.0f)
    {
        const auto body = curveBody (clipBounds (*clip));
        const int index = project.setCurvePoint (clip->id,
                                                 clampToCurveWindow (*clip, snap (h.beat, e.mods)
                                                                              - (clip->start - clip->offset)),
                                                 curveYToValue (body, e.position.y),
                                                 0.0);
        if (index >= 0)
        {
            drag.mode       = DragState::Mode::autoPoint;
            drag.clipId     = clip->id;
            drag.pointIndex = index;
            drag.moved      = true;
        }
        repaint();
        return;
    }

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

/** Stretches a clip to a length in beats, from the state it had when the drag
    began.

    A warped clip has no single ratio to set, so the whole map is scaled: every
    marker's beat and the ratio move by the same factor, which stretches the
    performance without disturbing the feel that the markers describe. With no
    markers this is exactly the old one line calculation.
*/
void PlaylistComponent::stretchClipTo (Clip& c, const Clip& original, double wantedBeats)
{
    const double wasBeats = original.lengthBeats (project.bpm);
    if (wasBeats <= 1.0e-9 || wantedBeats <= 1.0e-9)
        return;

    const double wanted = juce::jlimit (0.1, 10.0, original.stretch * (wantedBeats / wasBeats));
    const double factor = wanted / std::max (1.0e-9, original.stretch);

    c.stretch = wanted;
    c.warp    = original.warp;
    for (auto& m : c.warp)
        m.beat *= factor;

    // Squeezing hard enough can bring two markers onto the same beat. They are
    // taken from the drag's starting state every time, so dragging back out
    // brings them back.
    c.tidyWarp();
}

void PlaylistComponent::mouseDrag (const juce::MouseEvent& e)
{
    const auto h = hitTest (e.position);
    hoverBeat = std::max (0.0, snap (h.beat, e.mods, true));

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

        case DragState::Mode::warp:
        {
            auto* c = project.find (drag.clipId);
            if (c == nullptr) return;
            project.moveWarpMarker (c->id, drag.warpIndex,
                                    snap (h.beat, e.mods) - c->start);
            drag.moved = true;
            break;
        }

        case DragState::Mode::autoPoint:
        {
            auto* c = project.find (drag.clipId);
            if (c == nullptr || c->curve == nullptr) return;

            // The index is re-read from what the move returns, so a point
            // dragged past its neighbour swaps with it and carries on being
            // dragged rather than being left behind.
            const auto body = curveBody (clipBounds (*c));
            drag.pointIndex = project.moveCurvePoint (c->id, drag.pointIndex,
                                                      clampToCurveWindow (*c, snap (h.beat, e.mods)
                                                                                - (c->start - c->offset)),
                                                      curveYToValue (body, e.position.y));
            drag.moved = true;
            break;
        }

        case DragState::Mode::autoBend:
        {
            auto* c = project.find (drag.clipId);
            if (c == nullptr || c->curve == nullptr) return;

            // Vertical travel, measured against where the handle was grabbed,
            // with the full range reached over about a track's height.
            //
            // The sign follows the segment's own direction, so the curve always
            // moves the way the mouse does. A positive bend holds the value
            // near the point it is leaving, which sags a rising segment
            // downwards but holds a falling one up, so taking the direction
            // into account is what stops the handle running away from the
            // mouse on half the segments in a curve.
            const auto& points = c->curve->points;
            if (! juce::isPositiveAndBelow (drag.pointIndex, (int) points.size())
                || (size_t) drag.pointIndex + 1 >= points.size())
                return;

            const double rise = points[(size_t) drag.pointIndex + 1].value
                                  - points[(size_t) drag.pointIndex].value;
            const double sign = rise >= 0.0 ? 1.0 : -1.0;

            const double travel = (double) (e.position.y - drag.bendY0) / (double) trackH;
            project.setCurveBend (c->id, drag.pointIndex, drag.bend0 + sign * travel * 2.0);
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

                // Through the warp map, so dragging the end of a warped clip
                // takes in exactly the audio that reaches that beat. With no
                // markers this is the same arithmetic as before.
                const double wanted = o->sourceAtBeat (project.bpm, end - o->start) - o->offset;
                c->length = juce::jlimit (0.005, c->sample->durationSeconds() - o->offset, wanted);
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
                // The beat at which the very start of the file would be read is
                // as far left as the edge can go.
                newStart = std::max ({ newStart, o->start + o->beatAtSource (project.bpm, 0.0), 0.0 });

                const double shiftBeats  = newStart - o->start;
                const double shiftSource = o->sourceAtBeat (project.bpm, shiftBeats) - o->offset;
                c->start  = newStart;
                c->offset = o->offset + shiftSource;
                c->length = o->length - shiftSource;

                if (o->isWarped())
                {
                    // The edge becomes the clip's new anchor, so the markers are
                    // measured from it. Those the edge has passed are gone, and
                    // if that leaves none at all the ratio has to take over the
                    // rate the clip was reading at there, or the trim would
                    // change its speed.
                    c->warp = o->warp;
                    rebaseWarpMarkers (c->warp, shiftBeats, c->offset);

                    if (c->warp.empty())
                        c->stretch = juce::jlimit (0.1, 10.0,
                                                   warpStretchForSlope (project.bpm,
                                                                        o->slopeAtBeat (project.bpm, shiftBeats)));
                }
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
            stretchClipTo (*c, *o, end - o->start);
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
            stretchClipTo (*c, *o, originalEnd - newStart);
            c->start = originalEnd - c->lengthBeats (project.bpm);
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

    // A finished drag is the point at which a warped clip's stretch ratio can
    // be put back to its own average rate, and so a fresh render started.
    // Doing it during the drag would re-render on every mouse move. This covers
    // dragging a marker, trimming and stretching alike, and does nothing to a
    // clip that is not warped.
    if (drag.moved && drag.clipId != 0)
        project.retuneWarpRatio (drag.clipId);

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
        auto* c = project.find (h.clipId);
        if (c == nullptr)
            return;

        if (c->isMidi())
        {
            if (onOpenPianoRoll)
                onOpenPianoRoll (c->id);
            return;
        }

        if (c->isAutomation())
        {
            // A curve is edited in place, so there is nothing to open. Double
            // clicking works on its handles instead: on a point it removes it,
            // on a bend handle it puts the segment back to a straight line,
            // which is the one thing a bend drag cannot reliably land on.
            if (project.selection.count (c->id) == 0)
                project.selection = { c->id };

            if (h.autoPoint >= 0)       project.removeCurvePoint (c->id, h.autoPoint);
            else if (h.autoBend >= 0)   project.setCurveBend (c->id, h.autoBend, 0.0);
            else                        return;

            updateClipBar();
            repaint();
            return;
        }

        // Double-clicking an audio clip works on its warp markers: on a marker
        // it removes it, anywhere else it adds one. Adding one does not change
        // the sound, it only pins what is already there, so this is safe to do
        // by accident.
        if (tool != Tool::draw)
            return;

        if (project.selection.count (c->id) == 0)
            project.selection = { c->id };

        if (h.warpIndex >= 0)
            project.removeWarpMarker (c->id, h.warpIndex);
        else
            project.addWarpMarker (c->id, snap (h.beat, e.mods) - c->start);

        updateClipBar();
        repaint();
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
    menu.addSeparator();
    menu.addItem (3, "Automate...");

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
        else if (result == 3)
        {
            // Shown from here rather than nested in the menu above, because
            // what is automatable depends on the plugins loaded and building
            // the whole tree every time the track menu opens would walk every
            // plugin's parameter list for a menu nobody had asked for.
            safeThis->showAutomationMenu (track);
            return;
        }
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

    // MIDI files come in through the same drop, because to a producer dragging
    // a part in is the same gesture as dragging a sample in. They are taken
    // first so that the rows they claim are the ones nearest the drop.
    juce::StringArray midiPaths;
    for (const auto& path : paths)
        if (MidiFileIO::isMidiFile (juce::File (path)))
            midiPaths.add (path);

    juce::String midiReport;
    if (! midiPaths.isEmpty())
        midiReport = importMidiFiles (midiPaths, beat, track);   // advances track

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

    if (midiReport.isNotEmpty() && onStatus)
        onStatus (midiReport);

    if (! problems.isEmpty())
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::WarningIcon)
                                          .withTitle ("Some files couldn't be added")
                                          .withMessage (problems.joinIntoString ("\n"))
                                          .withButton ("OK"),
                                      nullptr);
}

int PlaylistComponent::firstFreeChannel (const std::set<int>& alreadyClaimed) const
{
    for (int c = 0; c < kNumChannels; ++c)
    {
        if (alreadyClaimed.count (c) != 0)
            continue;
        if (project.channels[(size_t) c].name.isNotEmpty())
            continue;        // a plugin is loaded here

        bool inUse = false;
        for (const auto& clip : project.clips)
            if (clip.isMidi() && clip.channel == c)
            {
                inUse = true;
                break;
            }

        if (! inUse)
            return c;
    }

    return -1;
}

juce::String PlaylistComponent::importMidiFiles (const juce::StringArray& paths, double beat, int& track)
{
    juce::StringArray problems;
    std::set<int> claimed;

    int filesRead = 0, clipsMade = 0, notesRead = 0, droppedOverlaps = 0;
    bool ranOutOfChannels = false, ranOutOfTracks = false;
    bool sawTempoChanges = false, splitByChannel = false;
    double adoptedTempo = 0.0;

    for (const auto& path : paths)
    {
        const juce::File file (path);
        const auto read = MidiFileIO::read (file);

        if (! read.ok)
        {
            problems.add (read.error);
            continue;
        }

        ++filesRead;
        notesRead       += read.notesRead;
        droppedOverlaps += read.droppedOverlaps;
        sawTempoChanges  = sawTempoChanges || read.hasTempoChanges;
        splitByChannel   = splitByChannel  || read.splitByChannel;

        // The file's own tempo is adopted only while the project is still at
        // the tempo nobody chose. A producer who has set a tempo means it, and
        // a dropped file silently moving the whole arrangement underneath them
        // would be far worse than a part that needs stretching.
        if (read.bpm > 0.0 && adoptedTempo == 0.0
            && std::abs (project.bpm - kDefaultBpm) < 1.0e-9 && onSetTempo)
        {
            adoptedTempo = read.bpm;   // applied below, once the clips are in
        }

        for (const auto& imported : read.tracks)
        {
            if (track >= (int) project.tracks.size())
            {
                ranOutOfTracks = true;
                break;
            }

            const int channel = firstFreeChannel (claimed);
            if (channel < 0)
            {
                ranOutOfChannels = true;
                break;
            }
            claimed.insert (channel);

            auto pattern = std::make_shared<MidiPattern>();
            pattern->notes = imported.notes;

            // The clip has to be long enough to play its last note, and a
            // whole number of bars so it sits on the grid the way a part drawn
            // here would. Note positions keep the beats they had in the file,
            // which is the only thing that keeps the tracks of a multi-track
            // file lined up with each other.
            const double needed = pattern->lastBeat();
            const double bars   = std::max (1.0, std::ceil (needed / 4.0));

            Clip c;
            c.type    = ClipType::midi;
            c.pattern = pattern;
            c.channel = channel;
            c.track   = track;
            c.start   = beat;
            c.offset  = 0.0;
            c.length  = bars * 4.0;

            project.selection.insert (project.addClip (c));
            ++clipsMade;

            // The track's name is the right home for the name the file
            // carried: a channel's name means the plugin loaded in it, and
            // putting a part's name there would read as an instrument that
            // is not actually there.
            if (imported.name.isNotEmpty())
                project.tracks[(size_t) track].name = imported.name;

            ++track;
        }
    }

    // Setting the tempo re-stretches every tempo following clip and pushes a
    // new arrangement, so it happens once the clips are all in rather than
    // part way through adding them.
    if (adoptedTempo > 0.0 && clipsMade > 0 && onSetTempo)
        onSetTempo (adoptedTempo);

    if (! problems.isEmpty() && clipsMade == 0)
        return problems[0];

    if (clipsMade == 0)
        return "Nothing in that file could be imported.";

    juce::String report;
    report << "Imported " << clipsMade << (clipsMade == 1 ? " track, " : " tracks, ")
           << notesRead << (notesRead == 1 ? " note" : " notes");

    if (filesRead > 1)
        report << " from " << filesRead << " files";

    if (adoptedTempo > 0.0)
        report << ", at the file's tempo of " << juce::String (adoptedTempo, 2) << " bpm";

    report << ".";

    // Everything below is something the producer would otherwise find out by
    // noticing it was wrong.
    if (sawTempoChanges)
        report << "  The file changes tempo, which this studio can't follow yet: its opening tempo was used.";

    if (splitByChannel)
        report << "  A track carrying several MIDI channels was split, one clip each.";

    if (droppedOverlaps > 0)
        report << "  " << droppedOverlaps
               << (droppedOverlaps == 1 ? " note was" : " notes were")
               << " dropped: one channel can't sound two of the same pitch at once.";

    if (ranOutOfChannels)
        report << "  Ran out of free instrument channels, so the rest was left out.";

    if (ranOutOfTracks)
        report << "  Ran out of playlist tracks, so the rest was left out.";

    if (! problems.isEmpty())
        report << "  " << problems.joinIntoString ("  ");

    return report;
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
    {
        const juce::File file (f);
        if (cache.isAudioFile (file) || MidiFileIO::isMidiFile (file))
            return true;
    }
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
