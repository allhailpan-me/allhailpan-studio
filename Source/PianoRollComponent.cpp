#include "PianoRollComponent.h"
#include "AhpLookAndFeel.h"
#include <cmath>

namespace
{
    const std::vector<std::pair<juce::String, double>> rollSnaps {
        { "Bar", 4.0 }, { "Beat", 1.0 }, { "1/2 beat", 0.5 }, { "1/3 beat", 1.0 / 3.0 },
        { "Step", 0.25 }, { "1/2 step", 0.125 }, { "1/3 step", 1.0 / 12.0 },
        { "1/4 step", 0.0625 }, { "1/6 step", 1.0 / 24.0 }, { "None", 0.0 }
    };

    bool isBlackKey (int note)
    {
        const int n = note % 12;
        return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
    }
}

PianoRollComponent::PianoRollComponent (Project& p, AudioEngine& e)
    : project (p), engine (e)
{
    title.setColour (juce::Label::textColourId, Ahp::bone);
    title.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    laneLabel.setColour (juce::Label::textColourId, Ahp::muted);
    snapLabel.setColour (juce::Label::textColourId, Ahp::muted);

    for (int i = 0; i < (int) rollSnaps.size(); ++i)
        snapBox.addItem (rollSnaps[(size_t) i].first, i + 1);
    snapBox.setSelectedId (5, juce::dontSendNotification);
    snapBox.onChange = [this] { repaint(); };
    snapBox.setTooltip ("Hold Alt to ignore the grid");

    laneBox.onChange = [this] { clearLaneButton.setEnabled (laneBox.getSelectedId() >= 2); repaint(); };
    laneBox.setTooltip ("Velocity, or a parameter you recorded from the plugin");

    drawButton.onClick  = [this] { eraseTool = false; drawButton.setToggleState (true, juce::dontSendNotification); eraseButton.setToggleState (false, juce::dontSendNotification); };
    eraseButton.onClick = [this] { eraseTool = true;  drawButton.setToggleState (false, juce::dontSendNotification); eraseButton.setToggleState (true, juce::dontSendNotification); };
    drawButton.setToggleState (true, juce::dontSendNotification);

    clearLaneButton.onClick = [this]
    {
        auto* pat = pattern();
        const int id = laneBox.getSelectedId();
        if (pat == nullptr || id < 2)
            return;
        const int index = id - 2;
        if (juce::isPositiveAndBelow (index, (int) pat->lanes.size()))
            pat->lanes.erase (pat->lanes.begin() + index);
        rebuildLaneBox();
        changed (true);
    };

    for (auto* c : { static_cast<juce::Component*> (&title), static_cast<juce::Component*> (&laneBox),
                     static_cast<juce::Component*> (&snapBox), static_cast<juce::Component*> (&laneLabel),
                     static_cast<juce::Component*> (&snapLabel), static_cast<juce::Component*> (&drawButton),
                     static_cast<juce::Component*> (&eraseButton), static_cast<juce::Component*> (&clearLaneButton) })
    {
        c->setWantsKeyboardFocus (false);
        c->setMouseClickGrabsKeyboardFocus (false);
        addAndMakeVisible (c);
    }

    for (auto* sb : { &hbar, &vbar })
    {
        sb->addListener (this);
        sb->setAutoHide (false);
        sb->setColour (juce::ScrollBar::thumbColourId, Ahp::panel3.brighter (0.15f));
        addAndMakeVisible (sb);
    }

    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
}

PianoRollComponent::~PianoRollComponent()
{
    hbar.removeListener (this);
    vbar.removeListener (this);
    if (heldPreview >= 0)
        engine.keyboard().noteOff (1, heldPreview, 0.0f);
}

Clip* PianoRollComponent::clip()
{
    auto* c = project.find (clipId);
    return (c != nullptr && ! c->isAudio() && c->pattern != nullptr) ? c : nullptr;
}

MidiPattern* PianoRollComponent::pattern()
{
    auto* c = clip();
    return c != nullptr ? c->pattern.get() : nullptr;
}

AutoLane* PianoRollComponent::currentLane()
{
    auto* pat = pattern();
    const int index = laneBox.getSelectedId() - 2;
    if (pat == nullptr || ! juce::isPositiveAndBelow (index, (int) pat->lanes.size()))
        return nullptr;
    return &pat->lanes[(size_t) index];
}

void PianoRollComponent::setClip (int newClipId)
{
    clipId = newClipId;
    rebuildLaneBox();

    if (auto* c = clip())
    {
        engine.setSelectedChannel (c->channel);
        scrollBeats = std::max (0.0, c->offset);

        // Centre the view on the notes
        if (auto* pat = c->pattern.get(); pat != nullptr && ! pat->notes.empty())
        {
            int lo = 127, hi = 0;
            for (auto& n : pat->notes) { lo = std::min (lo, n.note); hi = std::max (hi, n.note); }
            scrollY = (float) ((127 - (lo + hi) / 2) * rowH) - grid.getHeight() / 2.0f;
        }
        else
        {
            scrollY = (float) ((127 - 66) * rowH);
        }
        initialScrollDone = true;
    }
    refresh();
}

void PianoRollComponent::rebuildLaneBox()
{
    const int previous = laneBox.getSelectedId();
    laneBox.clear (juce::dontSendNotification);
    laneBox.addItem ("Velocity", 1);

    if (auto* pat = pattern())
        for (int i = 0; i < (int) pat->lanes.size(); ++i)
            laneBox.addItem ("Automation: " + pat->lanes[(size_t) i].name, i + 2);

    laneBox.setSelectedId (laneBox.indexOfItemId (previous) >= 0 ? previous : 1, juce::dontSendNotification);
}

void PianoRollComponent::refresh()
{
    if (auto* c = clip())
    {
        title.setText (c->displayName (project.channels) + "   |   " + project.tracks[(size_t) c->track].name,
                       juce::dontSendNotification);
        if (auto* pat = c->pattern.get(); pat != nullptr && laneBox.getNumItems() != (int) pat->lanes.size() + 1)
            rebuildLaneBox();
    }
    else
    {
        title.setText ("Double-click a MIDI clip in the playlist, or record one, to edit it here.", juce::dontSendNotification);
    }
    clearLaneButton.setEnabled (laneBox.getSelectedId() >= 2);
    updateScrollBars();
    repaint();
}

void PianoRollComponent::changed (bool final)
{
    juce::ignoreUnused (final);
    project.changed();
    repaint();
}

// ---------------------------------------------------------------------------
// Layout and scrolling

void PianoRollComponent::resized()
{
    auto r = getLocalBounds();
    toolbar = r.removeFromTop (toolbarH);

    auto tb = toolbar.reduced (10, 6);
    drawButton .setBounds (tb.removeFromLeft (56));
    eraseButton.setBounds (tb.removeFromLeft (62));  tb.removeFromLeft (14);
    snapLabel  .setBounds (tb.removeFromLeft (42));
    snapBox    .setBounds (tb.removeFromLeft (100)); tb.removeFromLeft (14);
    laneLabel  .setBounds (tb.removeFromLeft (40));
    laneBox    .setBounds (tb.removeFromLeft (240)); tb.removeFromLeft (6);
    clearLaneButton.setBounds (tb.removeFromLeft (84)); tb.removeFromLeft (14);
    title      .setBounds (tb);

    hbar.setBounds (r.removeFromBottom (barThickness).withTrimmedLeft (keysW).withTrimmedRight (barThickness));
    laneArea = r.removeFromBottom (laneH).withTrimmedLeft (keysW).withTrimmedRight (barThickness);
    vbar.setBounds (r.removeFromRight (barThickness).withTrimmedTop (rulerH));
    ruler = r.removeFromTop (rulerH).withTrimmedLeft (keysW);
    keys  = r.removeFromLeft (keysW);
    grid  = r;

    if (! initialScrollDone)
        scrollY = (float) ((127 - 72) * rowH);

    updateScrollBars();
}

void PianoRollComponent::updateScrollBars()
{
    double patternEnd = 16.0;
    if (auto* c = clip())
        patternEnd = std::max ({ patternEnd, c->offset + c->length, c->pattern->lastBeat() });

    const double visible = grid.getWidth() / ppb;
    hbar.setRangeLimits (0.0, std::max (patternEnd + 8.0, scrollBeats + visible), juce::dontSendNotification);
    hbar.setCurrentRange (scrollBeats, visible, juce::dontSendNotification);

    const double total = 128.0 * rowH;
    scrollY = (float) juce::jlimit (0.0, std::max (0.0, total - grid.getHeight()), (double) scrollY);
    vbar.setRangeLimits (0.0, total, juce::dontSendNotification);
    vbar.setCurrentRange (scrollY, grid.getHeight(), juce::dontSendNotification);
}

void PianoRollComponent::scrollBarMoved (juce::ScrollBar* bar, double start)
{
    if (bar == &hbar) scrollBeats = std::max (0.0, start);
    else              scrollY = (float) start;
    repaint();
}

void PianoRollComponent::zoom (double factor, float anchorX)
{
    const double anchor = xToBeat (anchorX);
    ppb = juce::jlimit (8.0, 800.0, ppb * factor);
    scrollBeats = std::max (0.0, anchor - (anchorX - grid.getX()) / ppb);
    updateScrollBars();
    repaint();
}

void PianoRollComponent::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
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

void PianoRollComponent::updatePlayhead()
{
    if (engine.isPlaying() || wasPlaying)
        repaint (grid);
    wasPlaying = engine.isPlaying();
}

// ---------------------------------------------------------------------------
// Drawing

void PianoRollComponent::paint (juce::Graphics& g)
{
    g.fillAll (Ahp::panel);
    g.setColour (Ahp::line);
    g.fillRect (toolbar.withTop (toolbar.getBottom() - 1));

    auto* c   = clip();
    auto* pat = pattern();
    const double b0 = scrollBeats, b1 = xToBeat ((float) grid.getRight());

    // ---- note grid ----
    {
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (grid);

        for (int note = 0; note < 128; ++note)
        {
            const float y = noteToY (note);
            if (y > grid.getBottom() || y + rowH < grid.getY())
                continue;
            g.setColour (isBlackKey (note) ? Ahp::panel : Ahp::panel2);
            g.fillRect (juce::Rectangle<float> ((float) grid.getX(), y, (float) grid.getWidth(), (float) rowH));
            if (note % 12 == 0)
            {
                g.setColour (Ahp::line);
                g.fillRect (juce::Rectangle<float> ((float) grid.getX(), y + rowH - 1.0f, (float) grid.getWidth(), 1.0f));
            }
        }

        const double snapSize = snapValue ({});
        const double minor = (snapSize > 0.0 && snapSize * ppb >= 6.0) ? snapSize : 0.25;
        for (double b = std::floor (b0 / minor) * minor; b <= b1; b += minor)
        {
            const bool isBar  = std::abs (b / 4.0 - std::round (b / 4.0)) < 1e-6;
            const bool isBeat = std::abs (b - std::round (b)) < 1e-6;
            g.setColour (isBar ? Ahp::muted : isBeat ? Ahp::line : Ahp::line.withAlpha (0.4f));
            g.fillRect (juce::Rectangle<float> (std::round (beatToX (b)), (float) grid.getY(), 1.0f, (float) grid.getHeight()));
        }

        if (c != nullptr)
        {
            // shade outside the clip's visible range
            g.setColour (Ahp::black.withAlpha (0.45f));
            const float cx0 = beatToX (c->offset), cx1 = beatToX (c->offset + c->length);
            if (cx0 > grid.getX())     g.fillRect (juce::Rectangle<float> ((float) grid.getX(), (float) grid.getY(), cx0 - grid.getX(), (float) grid.getHeight()));
            if (cx1 < grid.getRight()) g.fillRect (juce::Rectangle<float> (cx1, (float) grid.getY(), grid.getRight() - cx1, (float) grid.getHeight()));

            for (auto& n : pat->notes)
            {
                juce::Rectangle<float> r (beatToX (n.start), noteToY (n.note) + 1.0f,
                                          std::max (3.0f, (float) (n.length * ppb)), (float) rowH - 2.0f);
                if (r.getRight() < grid.getX() || r.getX() > grid.getRight())
                    continue;
                g.setColour (Ahp::bone.withAlpha (0.45f + 0.55f * n.velocity));
                g.fillRoundedRectangle (r, 2.0f);
                g.setColour (Ahp::black);
                g.fillRect (r.withLeft (r.getRight() - 3.0f).reduced (0.0f, 3.0f));
                if (r.getWidth() > 34.0f)
                {
                    g.setFont (juce::FontOptions (10.0f));
                    g.drawText (juce::MidiMessage::getMidiNoteName (n.note, true, true, 4), r.withTrimmedLeft (3.0f), juce::Justification::centredLeft);
                }
            }

            // playhead
            const double songBeat = engine.getBeatPosition();
            const double local    = songBeat - c->start + c->offset;
            if (engine.isPlaying() && local >= c->offset && local <= c->offset + c->length)
            {
                g.setColour (Ahp::bone.withAlpha (0.9f));
                g.fillRect (juce::Rectangle<float> (std::round (beatToX (local)), (float) grid.getY(), 2.0f, (float) grid.getHeight()));
            }
        }
    }

    // ---- ruler ----
    g.setColour (Ahp::panel3);
    g.fillRect (ruler);
    {
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (ruler);
        g.setFont (juce::FontOptions (11.0f));
        for (int bar = (int) (b0 / 4.0); bar * 4.0 <= b1; ++bar)
        {
            const float x = beatToX (bar * 4.0);
            g.setColour (Ahp::bone);
            g.drawText (juce::String (bar + 1), juce::Rectangle<float> (x + 3.0f, (float) ruler.getY(), 40.0f, (float) rulerH),
                        juce::Justification::centredLeft);
            g.fillRect (juce::Rectangle<float> (x, (float) ruler.getY() + 12.0f, 1.0f, 10.0f));
        }
    }

    // ---- keyboard ----
    {
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (keys);
        for (int note = 0; note < 128; ++note)
        {
            const float y = noteToY (note);
            if (y > keys.getBottom() || y + rowH < keys.getY())
                continue;
            const bool black = isBlackKey (note);
            juce::Rectangle<float> r ((float) keys.getX(), y, black ? keysW * 0.62f : (float) keysW, (float) rowH);
            g.setColour (black ? Ahp::panel2 : Ahp::bone);
            g.fillRect (r.reduced (0.0f, 0.5f));
            if (note % 12 == 0)
            {
                g.setColour (Ahp::black);
                g.setFont (juce::FontOptions (10.0f));
                g.drawText (juce::MidiMessage::getMidiNoteName (note, true, true, 4),
                            r.withTrimmedRight (4.0f), juce::Justification::centredRight);
            }
        }
    }
    g.setColour (Ahp::panel3);
    g.fillRect (juce::Rectangle<int> (keys.getX(), ruler.getY(), keysW, rulerH));

    // ---- lane ----
    g.setColour (Ahp::panel2);
    g.fillRect (laneArea);
    g.setColour (Ahp::line);
    g.fillRect (laneArea.withHeight (1));
    g.setColour (Ahp::muted);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText (laneBox.getText(), juce::Rectangle<int> (0, laneArea.getY(), keysW - 4, 18), juce::Justification::centredRight);

    if (pat != nullptr)
    {
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (laneArea);
        const float top = (float) laneArea.getY() + 6.0f, h = (float) laneArea.getHeight() - 10.0f;

        if (auto* lane = currentLane())
        {
            juce::Path path;
            bool started = false;
            for (auto& p : lane->points)
            {
                const float x = beatToX (p.beat), y = top + (1.0f - p.value) * h;
                if (! started) { path.startNewSubPath ((float) laneArea.getX(), y); path.lineTo (x, y); started = true; }
                else           path.lineTo (x, y);
            }
            if (started)
            {
                path.lineTo ((float) laneArea.getRight(), path.getCurrentPosition().y);
                g.setColour (Ahp::bone.withAlpha (0.8f));
                g.strokePath (path, juce::PathStrokeType (1.5f));
                for (auto& p : lane->points)
                    g.fillEllipse (beatToX (p.beat) - 3.0f, top + (1.0f - p.value) * h - 3.0f, 6.0f, 6.0f);
            }
            else
            {
                g.drawText ("Click to add points", laneArea, juce::Justification::centred);
            }
        }
        else
        {
            for (auto& n : pat->notes)
            {
                const float x = beatToX (n.start);
                const float y = top + (1.0f - n.velocity) * h;
                g.setColour (Ahp::bone.withAlpha (0.85f));
                g.fillRect (juce::Rectangle<float> (x, y, 2.0f, top + h - y));
                g.fillEllipse (x - 2.5f, y - 2.5f, 7.0f, 7.0f);
            }
        }
    }

    if (c == nullptr)
    {
        g.setColour (Ahp::muted);
        g.setFont (juce::FontOptions (15.0f));
        g.drawText ("No MIDI clip open", grid, juce::Justification::centred);
    }
}

// ---------------------------------------------------------------------------
// Editing

double PianoRollComponent::snapValue (const juce::ModifierKeys& mods) const
{
    if (mods.isAltDown())
        return 0.0;
    return rollSnaps[(size_t) std::max (0, snapBox.getSelectedId() - 1)].second;
}

double PianoRollComponent::snap (double beat, const juce::ModifierKeys& mods, bool roundDown) const
{
    const double s = snapValue (mods);
    if (s <= 0.0)
        return beat;
    return (roundDown ? std::floor (beat / s + 1e-9) : std::round (beat / s)) * s;
}

int PianoRollComponent::noteAt (juce::Point<float> p, bool& onRightEdge) const
{
    onRightEdge = false;
    auto* self = const_cast<PianoRollComponent*> (this);
    auto* pat  = self->pattern();
    if (pat == nullptr)
        return -1;

    const int pitch = yToNote (p.y);
    for (int i = (int) pat->notes.size(); --i >= 0;)
    {
        const auto& n = pat->notes[(size_t) i];
        if (n.note != pitch)
            continue;
        const float x0 = beatToX (n.start), x1 = x0 + std::max (3.0f, (float) (n.length * ppb));
        if (p.x >= x0 && p.x < x1)
        {
            onRightEdge = (x1 - p.x) < 6.0f && (x1 - x0) > 10.0f;
            return i;
        }
    }
    return -1;
}

void PianoRollComponent::preview (int note)
{
    if (note == heldPreview)
        return;
    if (heldPreview >= 0)
        engine.keyboard().noteOff (1, heldPreview, 0.0f);
    heldPreview = note;
    if (note >= 0)
        engine.keyboard().noteOn (1, note, 0.8f);
}

void PianoRollComponent::mouseMove (const juce::MouseEvent& e)
{
    bool edge = false;
    auto cursor = juce::MouseCursor::NormalCursor;
    if (grid.contains (e.getPosition()))
    {
        const int idx = noteAt (e.position, edge);
        if (idx >= 0)
            cursor = edge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::DraggingHandCursor;
    }
    else if (keys.contains (e.getPosition()))
        cursor = juce::MouseCursor::PointingHandCursor;
    setMouseCursor (cursor);
}

void PianoRollComponent::mouseDown (const juce::MouseEvent& e)
{
    if (onInteraction)
        onInteraction();

    auto* c   = clip();
    auto* pat = pattern();
    dragMode  = DragMode::none;
    if (c == nullptr)
        return;

    const auto pos = e.getPosition();

    if (ruler.contains (pos))
    {
        if (onSetPosition)
            onSetPosition (c->start + (snap (xToBeat (e.position.x), e.mods, true) - c->offset));
        return;
    }

    if (keys.contains (pos))
    {
        dragMode = DragMode::keys;
        preview (juce::jlimit (0, 127, yToNote (e.position.y)));
        return;
    }

    if (laneArea.contains (pos))
    {
        const float top = (float) laneArea.getY() + 6.0f, h = (float) laneArea.getHeight() - 10.0f;
        const float value = juce::jlimit (0.0f, 1.0f, 1.0f - (e.position.y - top) / h);

        if (auto* lane = currentLane())
        {
            // find a nearby point
            int nearIndex = -1;
            for (int i = 0; i < (int) lane->points.size(); ++i)
            {
                const auto& p = lane->points[(size_t) i];
                if (std::abs (beatToX (p.beat) - e.position.x) < 7.0f
                    && std::abs (top + (1.0f - p.value) * h - e.position.y) < 9.0f)
                    nearIndex = i;
            }

            if (e.mods.isPopupMenu() || eraseTool)
            {
                if (nearIndex >= 0)
                {
                    lane->points.erase (lane->points.begin() + nearIndex);
                    changed (true);
                }
                return;
            }

            if (nearIndex < 0)
            {
                lane->points.push_back ({ snap (xToBeat (e.position.x), e.mods), value });
                lane->sort();
                nearIndex = -1;
                for (int i = 0; i < (int) lane->points.size(); ++i)
                    if (std::abs (lane->points[(size_t) i].value - value) < 1e-6f)
                        nearIndex = i;
            }
            if (nearIndex >= 0)
            {
                draggedPoint = lane->points[(size_t) nearIndex];
                dragMode = DragMode::autoPoint;
            }
            changed (false);
        }
        else
        {
            dragMode = DragMode::velocity;
            lastVelocityX = e.position.x;
            for (auto& n : pat->notes)
                if (std::abs (beatToX (n.start) - e.position.x) < 6.0f)
                    n.velocity = std::max (0.02f, value);
            changed (false);
        }
        return;
    }

    if (! grid.contains (pos))
        return;

    bool edge = false;
    const int idx = noteAt (e.position, edge);

    if (e.mods.isPopupMenu() || eraseTool)
    {
        dragMode = DragMode::erase;
        if (idx >= 0)
        {
            pat->notes.erase (pat->notes.begin() + idx);
            changed (true);
        }
        return;
    }

    if (idx >= 0)
    {
        auto& n = pat->notes[(size_t) idx];
        dragNote   = idx;
        grabOffset = xToBeat (e.position.x) - n.start;
        grabPitch  = n.note;
        dragMode   = edge ? DragMode::resizeNote : DragMode::moveNote;
        if (! edge)
            preview (n.note);
        return;
    }

    // create a note
    MidiNote n;
    n.start    = std::max (0.0, snap (xToBeat (e.position.x), e.mods, true));
    n.note     = juce::jlimit (0, 127, yToNote (e.position.y));
    n.length   = lastLength;
    n.velocity = 0.8f;
    pat->notes.push_back (n);
    dragNote = (int) pat->notes.size() - 1;
    dragMode = DragMode::createNote;
    preview (n.note);

    // grow the clip if the note lands past its end
    if (n.start + n.length > c->offset + c->length)
        c->length = std::ceil ((n.start + n.length - c->offset) / 4.0) * 4.0;
    changed (false);
}

void PianoRollComponent::mouseDrag (const juce::MouseEvent& e)
{
    auto* c   = clip();
    auto* pat = pattern();
    if (c == nullptr || dragMode == DragMode::none)
        return;

    switch (dragMode)
    {
        case DragMode::keys:
            preview (juce::jlimit (0, 127, yToNote (e.position.y)));
            return;

        case DragMode::erase:
        {
            bool edge = false;
            const int idx = noteAt (e.position, edge);
            if (idx >= 0)
            {
                pat->notes.erase (pat->notes.begin() + idx);
                changed (false);
            }
            return;
        }

        case DragMode::createNote:
        case DragMode::resizeNote:
        {
            if (! juce::isPositiveAndBelow (dragNote, (int) pat->notes.size())) return;
            auto& n = pat->notes[(size_t) dragNote];
            const double minLen = std::max (snapValue (e.mods), 1.0 / 64.0);
            const double end    = std::max (n.start + minLen, snap (xToBeat (e.position.x), e.mods));
            n.length = end - n.start;
            if (end > c->offset + c->length)
                c->length = std::ceil ((end - c->offset) / 4.0) * 4.0;
            break;
        }

        case DragMode::moveNote:
        {
            if (! juce::isPositiveAndBelow (dragNote, (int) pat->notes.size())) return;
            auto& n = pat->notes[(size_t) dragNote];
            n.start = std::max (0.0, snap (xToBeat (e.position.x) - grabOffset, e.mods));
            n.note  = juce::jlimit (0, 127, yToNote (e.position.y));
            preview (n.note);
            break;
        }

        case DragMode::velocity:
        {
            const float top = (float) laneArea.getY() + 6.0f, h = (float) laneArea.getHeight() - 10.0f;
            const float value = juce::jlimit (0.02f, 1.0f, 1.0f - (e.position.y - top) / h);
            const float xa = std::min (lastVelocityX, e.position.x) - 6.0f;
            const float xb = std::max (lastVelocityX, e.position.x) + 6.0f;
            for (auto& n : pat->notes)
            {
                const float x = beatToX (n.start);
                if (x >= xa && x <= xb)
                    n.velocity = value;
            }
            lastVelocityX = e.position.x;
            break;
        }

        case DragMode::autoPoint:
        {
            auto* lane = currentLane();
            if (lane == nullptr) return;
            const float top = (float) laneArea.getY() + 6.0f, h = (float) laneArea.getHeight() - 10.0f;

            // find the dragged point again (sorting can move it)
            int index = -1;
            for (int i = 0; i < (int) lane->points.size(); ++i)
                if (lane->points[(size_t) i].beat == draggedPoint.beat && lane->points[(size_t) i].value == draggedPoint.value)
                    index = i;
            if (index < 0) return;

            auto& p = lane->points[(size_t) index];
            p.beat  = std::max (0.0, snap (xToBeat (e.position.x), e.mods));
            p.value = juce::jlimit (0.0f, 1.0f, 1.0f - (e.position.y - top) / h);
            draggedPoint = p;
            lane->sort();
            break;
        }

        case DragMode::none:
            return;
    }

    changed (false);
}

void PianoRollComponent::mouseUp (const juce::MouseEvent&)
{
    if (dragMode == DragMode::createNote || dragMode == DragMode::resizeNote)
        if (auto* pat = pattern())
            if (juce::isPositiveAndBelow (dragNote, (int) pat->notes.size()))
                lastLength = pat->notes[(size_t) dragNote].length;

    preview (-1);
    const bool wasEditing = dragMode != DragMode::none && dragMode != DragMode::keys;
    dragMode = DragMode::none;
    dragNote = -1;
    if (wasEditing)
        changed (true);
    updateScrollBars();
}
