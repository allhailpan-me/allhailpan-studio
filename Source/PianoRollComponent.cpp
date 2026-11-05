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

    // The pattern's notes in the form NoteSelection.h works on. The two
    // structs carry the same four fields and are deliberately separate types,
    // so this is a copy rather than a cast: a reinterpret here would be the
    // kind of shortcut that keeps working until somebody adds a field to one
    // of them.
    std::vector<NoteSelection::Note> toNotes (const std::vector<MidiNote>& in)
    {
        std::vector<NoteSelection::Note> out;
        out.reserve (in.size());
        for (const auto& n : in)
            out.push_back ({ n.start, n.length, n.note, n.velocity });
        return out;
    }

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
    snapBox.onChange = [this] { describeQuantise(); repaint(); };
    snapBox.setTooltip ("Hold Alt to ignore the grid");

    quantiseButton.onClick = [this] { showQuantiseMenu(); };

    laneBox.onChange = [this] { clearLaneButton.setEnabled (laneBox.getSelectedId() >= 2); repaint(); };
    laneBox.setTooltip ("Velocity, or a parameter you recorded from the plugin");

    // Three tools rather than two. Draw and Delete are unchanged; Select is
    // Steinberg's Object Selection tool in spirit, where a drag on empty grid
    // is a rectangle and a drag on a note moves it.
    auto chooseTool = [this] (int which)
    {
        eraseTool  = (which == 1);
        selectTool = (which == 2);
        drawButton  .setToggleState (which == 0, juce::dontSendNotification);
        eraseButton .setToggleState (which == 1, juce::dontSendNotification);
        selectButton.setToggleState (which == 2, juce::dontSendNotification);
    };

    drawButton.onClick   = [chooseTool] { chooseTool (0); };
    eraseButton.onClick  = [chooseTool] { chooseTool (1); };
    selectButton.onClick = [chooseTool] { chooseTool (2); };
    drawButton.setToggleState (true, juce::dontSendNotification);

    selectButton.setTooltip ("Drag a rectangle to select notes. Hold Command to do it from the Draw tool");
    selectionButton.setTooltip ("What to do with the selected notes");
    selectionButton.onClick = [this] { showSelectionMenu(); };

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
                     static_cast<juce::Component*> (&eraseButton), static_cast<juce::Component*> (&selectButton),
                     static_cast<juce::Component*> (&clearLaneButton),
                     static_cast<juce::Component*> (&quantiseButton),
                     static_cast<juce::Component*> (&selectionButton) })
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

    describeQuantise();

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
    return (c != nullptr && c->isMidi() && c->pattern != nullptr) ? c : nullptr;
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
    if (newClipId != clipId)
    {
        // What the last pass did belongs to the clip it was done to. Carried
        // across, the button would report work on a pattern that is not open
        // any more, including after a different project is loaded.
        lastQuantise = {};
        hasQuantised = false;
    }

    clipId = newClipId;
    // A selection is a set of indices into one pattern's note list, so it
    // means nothing in another pattern. Carried across, the next group edit
    // would move whichever notes happened to sit at those indices.
    clearSelection();
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

    auto* pat = pattern();
    quantiseButton.setEnabled (pat != nullptr && ! pat->notes.empty());
    // An undo can replace the whole note list under us, so what was selected
    // is checked rather than assumed every time the view is refreshed.
    validateSelection();
    selectionButton.setEnabled (pat != nullptr && ! pat->notes.empty());
    describeQuantise();

    updateScrollBars();
    repaint();
}

// ---------------------------------------------------------------------------
// Selecting notes, and editing the selection
//
// The arithmetic is all in NoteSelection.h, where it is tested without JUCE,
// along with what is published about each behaviour and what is not. What is
// here is the gestures, the menu, and the conversion to and from the
// project's own note type.
// ---------------------------------------------------------------------------

void PianoRollComponent::clearSelection()
{
    if (! selection.empty())
    {
        selection.clear();
        repaint();
    }
}

void PianoRollComponent::selectOnly (int index)
{
    selection.clear();
    if (index >= 0)
        selection.push_back ((size_t) index);
    repaint();
}

void PianoRollComponent::validateSelection()
{
    auto* pat = pattern();
    const size_t count = pat != nullptr ? pat->notes.size() : 0;

    const auto before = selection.size();
    while (! selection.empty() && selection.back() >= count)
        selection.pop_back();                 // the set is sorted, so the tail is what can be stale

    if (selection.size() != before)
        repaint();
}

NoteSelection::Limits PianoRollComponent::selectionLimits() const
{
    NoteSelection::Limits limits;

    // The floor is the pattern's origin, which is what dragging one note here
    // has always clamped at. Deliberately not the clip's left edge: a clip is
    // a window onto its pattern and the view already shades what is outside
    // it, so notes do live out there and dragging into that region is a
    // legitimate thing to want. There is no ceiling for the same reason.
    limits.minStart = 0.0;
    return limits;
}

NoteSelection::Snap PianoRollComponent::snapModeFor (const juce::ModifierKeys& mods) const
{
    // Alt defeating the grid is what this roll already did and what the Snap
    // box's own tooltip says, and it is Ableton's and FL Studio's published
    // modifier for it.
    if (mods.isAltDown() || snapValue (mods) <= 0.0)
        return NoteSelection::Snap::off;
    return snapMode;
}

bool PianoRollComponent::marqueeWanted (const juce::MouseEvent& e, int noteUnderPointer) const
{
    // Only from empty grid. On a note, a press is a selection or a drag, which
    // is Steinberg's Object Selection tool: a rectangle comes from dragging
    // the background. Command reaching the same thing from the Draw tool is FL
    // Studio's documented shortcut, where Ctrl plus a drag is "a rectangle to
    // select all notes in enclosed area".
    if (noteUnderPointer >= 0)
        return false;
    return selectTool || e.mods.isCommandDown();
}

void PianoRollComponent::writeBackSelectionEdit (const std::vector<NoteSelection::Note>& notes)
{
    auto* pat = pattern();
    if (pat == nullptr)
        return;

    std::vector<MidiNote> out;
    out.reserve (notes.size());
    for (const auto& n : notes)
    {
        MidiNote m;
        m.start    = n.start;
        m.length   = n.length;
        m.note     = n.note;
        m.velocity = n.velocity;
        out.push_back (m);
    }
    pat->notes = std::move (out);
}

void PianoRollComponent::growClipToFitSelection()
{
    auto* c   = clip();
    auto* pat = pattern();
    if (c == nullptr || pat == nullptr)
        return;

    double furthest = c->offset + c->length;
    for (const auto index : selection)
        if (index < pat->notes.size())
            furthest = std::max (furthest, pat->notes[index].start + pat->notes[index].length);

    if (furthest > c->offset + c->length)
        c->length = std::ceil ((furthest - c->offset) / 4.0) * 4.0;
}

void PianoRollComponent::beginGroupDrag (const juce::MouseEvent& e, int grabbedIndex, bool resizing)
{
    auto* pat = pattern();
    if (pat == nullptr || ! juce::isPositiveAndBelow (grabbedIndex, (int) pat->notes.size()))
        return;

    const auto& n = pat->notes[(size_t) grabbedIndex];

    dragNote        = grabbedIndex;
    dragStartNotes  = pat->notes;
    dragStartBeat   = xToBeat (e.position.x);
    dragStartPitch  = yToNote (e.position.y);
    dragAnchorStart = n.start;
    dragAnchorEnd   = n.start + n.length;
    dragMode        = resizing ? DragMode::resizeGroup : DragMode::moveGroup;
}

void PianoRollComponent::dragGroup (const juce::MouseEvent& e)
{
    auto* pat = pattern();
    if (pat == nullptr || dragStartNotes.size() != pat->notes.size())
        return;

    // Always from the snapshot taken when the gesture began, never from the
    // last answer. Feeding each mouse move the result of the one before it
    // would snap an already snapped position several times a second, and the
    // group would drift away from the pointer by an amount that depends on
    // how fast the mouse was moved.
    auto notes = toNotes (dragStartNotes);

    if (dragMode == DragMode::resizeGroup)
    {
        const double wantedEnd = snap (xToBeat (e.position.x), e.mods);

        // The mode is read here rather than at the press, which is what makes
        // these modifiers usable at all: Shift at the press toggles the
        // selection, so Shift can only mean something else once a drag is
        // already under way. Ableton has the same idea for its copy drag,
        // where "Pressing the modifier after dragging starts also switches a
        // move to a copy".
        //
        // Relative with nothing held, which is Apple's default: "The relative
        // differences between the lengths of the selected notes are
        // maintained." Shift for one shared end position, which is Apple's
        // "Adjust ends of all selected notes to the same position". Command
        // for one shared length, which is Studio One's "[Ctrl]/[Cmd] + drag
        // will snap the length of all notes to match the length of the clicked
        // note".
        if (e.mods.isShiftDown())
            NoteSelection::applyLength (notes, selection, wantedEnd, NoteSelection::LengthMode::sameEnd);
        else if (e.mods.isCommandDown())
            NoteSelection::applyLength (notes, selection, wantedEnd - dragAnchorStart,
                                        NoteSelection::LengthMode::sameLength);
        else
            NoteSelection::applyLength (notes, selection, wantedEnd - dragAnchorEnd,
                                        NoteSelection::LengthMode::group);

        writeBackSelectionEdit (notes);

        // Resizing already grew the clip to fit before there were selections,
        // and still does. Moving did not, and still does not: a note dragged
        // past the clip's end is hidden by the window rather than stretching
        // it, which is what the window is.
        growClipToFitSelection();
    }
    else
    {
        NoteSelection::Request request;
        request.beats       = xToBeat (e.position.x) - dragStartBeat;
        request.semitones   = yToNote (e.position.y) - dragStartPitch;
        request.grid        = snapValue (e.mods);
        request.snap        = snapModeFor (e.mods);
        request.anchorStart = dragAnchorStart;

        const auto move = NoteSelection::planMove (notes, selection, request, selectionLimits());
        NoteSelection::applyMove (notes, selection, move);
        writeBackSelectionEdit (notes);

        if (juce::isPositiveAndBelow (dragNote, (int) pat->notes.size()))
            preview (pat->notes[(size_t) dragNote].note);
    }

    changed (false);
}

void PianoRollComponent::commitSelectionEdit()
{
    auto* pat = pattern();
    if (pat == nullptr)
        return;

    auto notes = toNotes (pat->notes);
    const auto report = NoteSelection::resolveOverlaps (notes, selection);

    if (report.deleted > 0 || report.trimmed > 0)
        writeBackSelectionEdit (notes);
}

void PianoRollComponent::showSelectionMenu()
{
    auto* pat = pattern();
    if (pat == nullptr)
        return;

    validateSelection();

    const int count = (int) selection.size();
    const bool any  = count > 0;

    juce::PopupMenu m;
    m.addSectionHeader (count == 0 ? "Nothing selected"
                      : count == 1 ? "1 note selected"
                                   : juce::String (count) + " notes selected");

    m.addItem (10, "Select all", ! pat->notes.empty());
    m.addItem (11, "Select none", any);
    // Published by Steinberg ("Inverts the selection"), FL Studio and Ardour,
    // all three with the same meaning.
    m.addItem (12, "Invert selection", ! pat->notes.empty());

    m.addSeparator();

    // A semitone on the arrows and an octave with Shift is the single
    // strongest agreement in the research behind this: Steinberg, Bitwig,
    // Ableton and Ardour all publish it and all agree. The keys themselves
    // have to wait for the window to route them here.
    m.addItem (20, "Transpose up a semitone", any);
    m.addItem (21, "Transpose down a semitone", any);
    m.addItem (22, "Transpose up an octave", any);
    m.addItem (23, "Transpose down an octave", any);

    m.addSeparator();

    // Ten of 127 is Ableton's published step for a velocity nudge.
    m.addItem (30, "Velocity up", any);
    m.addItem (31, "Velocity down", any);

    m.addSeparator();

    m.addItem (40, "Duplicate", any);
    m.addItem (41, "Delete", any);

    m.addSeparator();

    juce::PopupMenu snapModes;
    snapModes.addItem (50, "Grid", true, snapMode == NoteSelection::Snap::absolute);
    snapModes.addItem (51, "Grid relative", true, snapMode == NoteSelection::Snap::relative);
    m.addSubMenu ("Dragging snaps to", snapModes);

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&selectionButton),
                     [this] (int result)
    {
        if (result == 0)
            return;

        auto* p = pattern();
        if (p == nullptr)
            return;

        if (result == 50) { snapMode = NoteSelection::Snap::absolute; return; }
        if (result == 51) { snapMode = NoteSelection::Snap::relative; return; }

        if (result == 10) { selection = NoteSelection::all (p->notes.size()); repaint(); return; }
        if (result == 11) { clearSelection(); return; }
        if (result == 12)
        {
            auto inverted = NoteSelection::all (p->notes.size());
            for (const auto index : selection)
                NoteSelection::remove (inverted, index);
            selection = std::move (inverted);
            repaint();
            return;
        }

        if (selection.empty())
            return;

        auto notes = toNotes (p->notes);

        switch (result)
        {
            case 20: case 21: case 22: case 23:
            {
                const int semitones = result == 20 ? 1 : result == 21 ? -1 : result == 22 ? 12 : -12;
                const auto move = NoteSelection::transpose (notes, selection, semitones, selectionLimits());
                if (move.semitones == 0)
                    return;                   // the group is against a limit, so there is nothing to undo
                NoteSelection::applyMove (notes, selection, move);
                writeBackSelectionEdit (notes);
                commitSelectionEdit();
                break;
            }

            case 30: case 31:
            {
                const double step = 10.0 / 127.0;
                NoteSelection::applyVelocity (notes, selection, result == 30 ? step : -step,
                                              NoteSelection::VelocityMode::group);
                writeBackSelectionEdit (notes);
                break;
            }

            case 40:
            {
                selection = NoteSelection::duplicate (notes, selection);
                writeBackSelectionEdit (notes);
                // A copy landing past the clip's end would be invisible, so
                // the clip grows to show it, the way drawing a note past the
                // end already does. Moving notes still does not grow it.
                growClipToFitSelection();
                commitSelectionEdit();
                break;
            }

            case 41:
                NoteSelection::eraseSelected (notes, selection);
                writeBackSelectionEdit (notes);
                break;

            default:
                return;
        }

        changed (true);
        refresh();
    });
}

// ---------------------------------------------------------------------------
// Quantise
//
// The arithmetic is all in Quantise.h, where it is tested without JUCE. What
// is here is the menu, the conversion to and from the project's own note type,
// and one undo step per pass.
//
// The grid is the Snap box's rather than a second control of its own, so there
// is one grid on screen and the lines the notes land on are the lines that are
// drawn. Ableton's quantise does the same thing, offering the current grid as
// well as a value chosen in the dialog.

double PianoRollComponent::quantiseGrid() const
{
    const double fromSnap = rollSnaps[(size_t) std::max (0, snapBox.getSelectedId() - 1)].second;

    // "None" is a choice about drawing freely, and a quantise has to have a
    // grid, so it falls back to a step rather than doing nothing silently.
    return fromSnap > 0.0 ? fromSnap : 0.25;
}

juce::String PianoRollComponent::quantiseGridName() const
{
    const double fromSnap = rollSnaps[(size_t) std::max (0, snapBox.getSelectedId() - 1)].second;
    if (fromSnap > 0.0)
        return rollSnaps[(size_t) std::max (0, snapBox.getSelectedId() - 1)].first;
    return "Step";
}

void PianoRollComponent::describeQuantise()
{
    const int strength = (int) std::lround (quantiseOptions.strength * 100.0);

    // Two decimals only where they say something, so the exact triplet reads
    // back as the 66.67% the menu offered rather than being rounded to 67%
    // and looking like a setting nothing in the menu can reach.
    const double swingPercent = quantiseOptions.swing * 100.0;
    const bool wholePercent = std::abs (swingPercent - std::lround (swingPercent)) < 1.0e-9;
    const juce::String swing = wholePercent ? juce::String ((int) std::lround (swingPercent))
                                            : juce::String (swingPercent, 2);

    juce::String what = "Move the notes onto the " + quantiseGridName().toLowerCase() + " grid: "
                      + juce::String (strength) + "% strength";

    if (std::abs (quantiseOptions.swing - 0.5) > 1.0e-9)
        what += ", swing " + swing + "%";

    what += quantiseOptions.target == Quantise::Target::starts ? ", note starts"
          : quantiseOptions.target == Quantise::Target::ends   ? ", note ends"
                                                               : ", both ends";

    if (quantiseOptions.gate == Quantise::Gate::include)
        what += ", only the notes already close";
    else if (quantiseOptions.gate == Quantise::Gate::exclude)
        what += ", leaving the close ones alone";

    if (hasQuantised)
    {
        what += ".  Last pass moved " + juce::String (lastQuantise.moved) + " note(s)";

        // The shortened ones are a subset of the moved ones, so they are
        // written as one rather than as a second group: three numbers in a row
        // read as three separate sets of notes.
        if (lastQuantise.trimmed > 0)
            what += ", " + juce::String (lastQuantise.trimmed)
                  + " of them cut back to where the next note of the same pitch starts";

        // A note merged into a neighbour is gone, and calling that a merge
        // sounds like nothing was lost. It is the one outcome somebody would
        // want to know about and the easiest to miss on a crowded pattern.
        if (lastQuantise.dropped > 0)
            what += ", and lost " + juce::String (lastQuantise.dropped)
                  + (lastQuantise.dropped == 1 ? " to another note of the same pitch landing on it"
                                               : " to other notes of the same pitch landing on them");

        // Held back is not a failure and does need saying, because the note
        // visibly did not move and the reason is not on screen.
        if (lastQuantise.held > 0)
            what += ".  " + juce::String (lastQuantise.held)
                  + " left where they were played, because the grid would have put them outside this clip";
    }

    quantiseButton.setTooltip (what);
}

void PianoRollComponent::showQuantiseMenu()
{
    const bool lengthsApply = Quantise::preserveLengthApplies (quantiseOptions);

    juce::PopupMenu strengths;
    {
        const int percents[] = { 100, 90, 80, 70, 60, 50, 25 };
        for (int i = 0; i < 7; ++i)
        {
            const double value = percents[i] / 100.0;
            // Avid recommends 70 to 80 per cent for keeping the original feel,
            // which is worth saying rather than leaving somebody to find.
            juce::String name = juce::String (percents[i]) + "%";
            if (percents[i] == 100) name += "  (fully on the grid)";
            if (percents[i] == 80)  name += "  (keeps the feel)";
            strengths.addItem (100 + i, name, true,
                               std::abs (quantiseOptions.strength - value) < 1.0e-9);
        }
    }

    juce::PopupMenu swings;
    {
        // Apple's own table of named swing presets, plus the exact triplet,
        // which Apple rounds to 66.
        const double values[] = { 0.5, 0.54, 0.58, 0.62, 0.66, 2.0 / 3.0, 0.71 };
        const char* names[] = { "Straight (50%)", "54%", "58%", "62%", "66%",
                                "66.67%  (triplet shuffle)", "71%" };
        for (int i = 0; i < 7; ++i)
            swings.addItem (200 + i, names[i], true,
                            std::abs (quantiseOptions.swing - values[i]) < 1.0e-9);
    }

    juce::PopupMenu targets;
    {
        targets.addItem (300, "Note starts", true, quantiseOptions.target == Quantise::Target::starts);
        targets.addItem (301, "Note ends", true, quantiseOptions.target == Quantise::Target::ends);
        targets.addItem (302, "Both ends", true, quantiseOptions.target == Quantise::Target::both);
        targets.addSeparator();

        // Greyed out rather than ignored wherever it would decide nothing,
        // which is both of the other two targets. With both ends going to the
        // grid a note cannot keep its length and have both ends on the grid,
        // and Avid makes the same option unavailable in that case rather than
        // quietly picking one of the two. With ends only the start is held
        // still anyway, so the length is whatever the end lands on.
        targets.addItem (303, "Keep note lengths", lengthsApply,
                         lengthsApply && quantiseOptions.preserveLength);
    }

    juce::PopupMenu gates;
    {
        gates.addItem (400, "Everything", true, quantiseOptions.gate == Quantise::Gate::off);
        gates.addItem (401, "Only notes within a quarter step of the grid", true,
                       quantiseOptions.gate == Quantise::Gate::include);
        gates.addItem (402, "Leave notes within an eighth step alone", true,
                       quantiseOptions.gate == Quantise::Gate::exclude
                           && std::abs (quantiseOptions.range - 0.25) < 1.0e-9);
        gates.addItem (403, "Leave notes within a quarter step alone", true,
                       quantiseOptions.gate == Quantise::Gate::exclude
                           && std::abs (quantiseOptions.range - 0.5) < 1.0e-9);
    }

    juce::PopupMenu menu;
    menu.addSectionHeader ("Quantise");
    menu.addItem (1, "Quantise to the " + quantiseGridName().toLowerCase() + " grid");
    menu.addSeparator();
    menu.addSubMenu ("Strength", strengths);
    menu.addSubMenu ("Swing", swings);
    menu.addSubMenu ("What moves", targets);
    menu.addSubMenu ("Which notes", gates);

    juce::Component::SafePointer<PianoRollComponent> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&quantiseButton),
                        [safeThis] (int result)
    {
        if (safeThis == nullptr || result == 0)
            return;

        auto& o = safeThis->quantiseOptions;

        if (result == 1)
        {
            safeThis->applyQuantise();
            return;
        }

        if (result >= 100 && result < 107)
        {
            const int percents[] = { 100, 90, 80, 70, 60, 50, 25 };
            o.strength = percents[result - 100] / 100.0;
        }
        else if (result >= 200 && result < 207)
        {
            const double values[] = { 0.5, 0.54, 0.58, 0.62, 0.66, 2.0 / 3.0, 0.71 };
            o.swing = values[result - 200];
        }
        else if (result == 300) o.target = Quantise::Target::starts;
        else if (result == 301) o.target = Quantise::Target::ends;
        else if (result == 302) o.target = Quantise::Target::both;
        else if (result == 303) o.preserveLength = ! o.preserveLength;
        else if (result == 400) o.gate = Quantise::Gate::off;
        else if (result == 401) { o.gate = Quantise::Gate::include; o.range = 0.5; }
        else if (result == 402) { o.gate = Quantise::Gate::exclude; o.range = 0.25; }
        else if (result == 403) { o.gate = Quantise::Gate::exclude; o.range = 0.5; }

        safeThis->describeQuantise();

        // Changing a setting opens the menu again, because nobody sets three
        // of these and then has to find the button once more for each.
        safeThis->showQuantiseMenu();
    });
}

void PianoRollComponent::applyQuantise()
{
    auto* c = clip();
    auto* pat = pattern();
    if (c == nullptr || pat == nullptr || pat->notes.empty())
        return;

    auto options = quantiseOptions;
    options.grid = quantiseGrid();

    // Which part of the pattern this clip actually plays. A note quantised
    // past either edge of that window is still in the pattern and still drawn
    // in the roll, and silent, so the quantiser holds it where it was played
    // instead. Growing the clip to cover it was tried and is worse: a clip is
    // a window onto its pattern and is often deliberately shorter, which is
    // how one bar of a longer pattern gets looped, so growing it un-hides
    // everything the trim was hiding and moves what is next on the track.
    options.windowStart = c->offset;
    options.windowEnd = c->offset + c->length;

    std::vector<TimebaseNote> notes;
    notes.reserve (pat->notes.size());
    for (const auto& n : pat->notes)
    {
        TimebaseNote t;
        t.start    = n.start;
        t.length   = n.length;
        t.note     = n.note;
        t.velocity = n.velocity;
        notes.push_back (t);
    }

    lastQuantise = Quantise::apply (notes, options);
    hasQuantised = true;
    describeQuantise();

    // Nothing moved, so there is nothing to undo either. An undo step that
    // undoes nothing is a step somebody has to press through twice.
    if (lastQuantise.moved == 0 && lastQuantise.dropped == 0 && lastQuantise.trimmed == 0)
        return;    // held notes change nothing, so they are not an edit either

    std::vector<MidiNote> quantised;
    quantised.reserve (notes.size());
    for (const auto& n : notes)
    {
        MidiNote m;
        m.start    = n.start;
        m.length   = n.length;
        m.note     = n.note;
        m.velocity = n.velocity;
        quantised.push_back (m);
    }

    pat->notes = std::move (quantised);
    // A pass can merge notes, so the indices a selection holds do not survive
    // it. Dropping the selection is honest; remapping it would mean guessing
    // which of two merged notes the user had picked.
    clearSelection();
    changed (true);
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
    drawButton  .setBounds (tb.removeFromLeft (52));
    eraseButton .setBounds (tb.removeFromLeft (58));
    selectButton.setBounds (tb.removeFromLeft (58));  tb.removeFromLeft (14);
    snapLabel  .setBounds (tb.removeFromLeft (42));
    snapBox    .setBounds (tb.removeFromLeft (100)); tb.removeFromLeft (14);
    laneLabel  .setBounds (tb.removeFromLeft (40));
    laneBox    .setBounds (tb.removeFromLeft (240)); tb.removeFromLeft (6);
    clearLaneButton.setBounds (tb.removeFromLeft (84)); tb.removeFromLeft (14);
    quantiseButton.setBounds (tb.removeFromLeft (78)); tb.removeFromLeft (6);
    selectionButton.setBounds (tb.removeFromLeft (86)); tb.removeFromLeft (14);
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

            for (size_t ni = 0; ni < pat->notes.size(); ++ni)
            {
                const auto& n = pat->notes[ni];
                juce::Rectangle<float> r (beatToX (n.start), noteToY (n.note) + 1.0f,
                                          std::max (3.0f, (float) (n.length * ppb)), (float) rowH - 2.0f);
                if (r.getRight() < grid.getX() || r.getX() > grid.getRight())
                    continue;
                const bool isSelected = NoteSelection::contains (selection, ni);
                g.setColour (Ahp::bone.withAlpha (isSelected ? 1.0f : 0.45f + 0.55f * n.velocity));
                g.fillRoundedRectangle (r, 2.0f);
                g.setColour (Ahp::black);
                g.fillRect (r.withLeft (r.getRight() - 3.0f).reduced (0.0f, 3.0f));
                if (r.getWidth() > 34.0f)
                {
                    g.setFont (juce::FontOptions (10.0f));
                    g.drawText (juce::MidiMessage::getMidiNoteName (n.note, true, true, 4), r.withTrimmedLeft (3.0f), juce::Justification::centredLeft);
                }

                // Selected notes are outlined rather than tinted. The theme
                // here is one colour, so a selection cannot be shown by hue,
                // and a note's own brightness is already carrying its
                // velocity: changing that to show selection would make the
                // two readings fight.
                if (isSelected)
                {
                    g.setColour (Ahp::black.withAlpha (0.85f));
                    g.drawRoundedRectangle (r.reduced (1.0f), 2.0f, 1.0f);
                    g.setColour (Ahp::bone);
                    g.drawRoundedRectangle (r.reduced (0.5f), 2.5f, 1.5f);
                }
            }

            if (dragMode == DragMode::marquee && ! marqueeOnScreen.isEmpty())
            {
                g.setColour (Ahp::bone.withAlpha (0.10f));
                g.fillRect (marqueeOnScreen);
                g.setColour (Ahp::bone.withAlpha (0.65f));
                g.drawRect (marqueeOnScreen, 1.0f);
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
            for (size_t ni = 0; ni < pat->notes.size(); ++ni)
            {
                const auto& n = pat->notes[ni];
                const float x = beatToX (n.start);
                const float y = top + (1.0f - n.velocity) * h;
                const bool isSelected = NoteSelection::contains (selection, ni);
                g.setColour (Ahp::bone.withAlpha (isSelected ? 1.0f : 0.5f));
                g.fillRect (juce::Rectangle<float> (x, y, 2.0f, top + h - y));
                if (isSelected)
                    g.fillEllipse (x - 3.5f, y - 3.5f, 9.0f, 9.0f);
                else
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
        else if (marqueeWanted (e, idx))
            cursor = juce::MouseCursor::CrosshairCursor;   // so the gesture is discoverable without reading anything
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
            // Pressing on a selected note's marker drags the whole selection:
            // one offset for all of them, the differences kept, and the group
            // stopping together at the rail. That is what Apple publishes.
            // Pressing anywhere else is the painting gesture this lane has
            // always had, which is still the quickest way to flatten a run of
            // velocities and is kept for that.
            int hit = -1;
            for (size_t i = 0; i < pat->notes.size(); ++i)
                if (std::abs (beatToX (pat->notes[i].start) - e.position.x) < 6.0f)
                    hit = (int) i;

            if (hit >= 0 && selection.size() > 1 && NoteSelection::contains (selection, (size_t) hit))
            {
                dragMode           = DragMode::velocityGroup;
                dragStartNotes     = pat->notes;
                dragStartVelocityY = e.position.y;
                return;
            }

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
            // Erasing a note moves every index after it, so a selection left
            // alone here would begin pointing at the wrong notes and the next
            // group edit would move something nobody picked.
            selection = NoteSelection::remapAfter (selection, NoteSelection::Set { (size_t) idx });
            changed (true);
        }
        return;
    }

    if (marqueeWanted (e, idx))
    {
        dragMode        = DragMode::marquee;
        marqueeFrom     = e.position;
        marqueeOnScreen = {};
        // Shift over a marquee reverses what it catches and leaves everything
        // outside it alone, which is Logic 9's documented rule: "the active
        // selection status of the elements is reversed".
        marqueeReverses = e.mods.isShiftDown();
        marqueeBefore   = marqueeReverses ? selection : NoteSelection::Set {};
        selection       = marqueeBefore;
        repaint();
        return;
    }

    if (idx >= 0)
    {
        // Shift on a note adds it or removes it. One modifier doing both is
        // the only documented convention: Ableton publishes both halves on
        // the same gesture and nobody publishes a second modifier for it.
        if (e.mods.isShiftDown())
        {
            NoteSelection::toggle (selection, (size_t) idx);
            if (NoteSelection::contains (selection, (size_t) idx))
                preview (pat->notes[(size_t) idx].note);
            repaint();
            return;
        }

        // A press on a note that is already selected keeps the selection, so
        // the drag that follows moves the group. No manual states this and a
        // group drag is impossible without it: NoteSelection.h has the
        // reasoning and the procedures that force it.
        if (! NoteSelection::contains (selection, (size_t) idx))
            selectOnly (idx);

        beginGroupDrag (e, idx, edge);
        if (! edge)
            preview (pat->notes[(size_t) idx].note);
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
    // A new note becomes the selection, so the gesture that drew it is also
    // the gesture that picked it, and whatever was selected before does not
    // quietly come along to the next group edit.
    selectOnly (dragNote);
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
                selection = NoteSelection::remapAfter (selection, NoteSelection::Set { (size_t) idx });
                changed (false);
            }
            return;
        }

        case DragMode::marquee:
        {
            marqueeOnScreen = juce::Rectangle<float>::leftTopRightBottom (
                std::min (marqueeFrom.x, e.position.x), std::min (marqueeFrom.y, e.position.y),
                std::max (marqueeFrom.x, e.position.x), std::max (marqueeFrom.y, e.position.y));

            const auto r = NoteSelection::Rect::between (xToBeat (marqueeFrom.x), xToBeat (e.position.x),
                                                         yToNote (marqueeFrom.y), yToNote (e.position.y));
            const auto notes = toNotes (pat->notes);
            selection = marqueeReverses ? NoteSelection::reverseInRect (marqueeBefore, notes, r)
                                        : NoteSelection::inRect (notes, r);
            // Choosing notes is not editing them, so this deliberately does
            // not reach changed(): a marquee must not land in the undo
            // history, and an undo history full of selections is one nobody
            // can use.
            repaint();
            return;
        }

        case DragMode::moveGroup:
        case DragMode::resizeGroup:
            dragGroup (e);
            return;

        case DragMode::velocityGroup:
        {
            const float h = std::max (1.0f, (float) laneArea.getHeight() - 10.0f);
            const double delta = (double) ((dragStartVelocityY - e.position.y) / h);
            auto notes = toNotes (dragStartNotes);
            NoteSelection::applyVelocity (notes, selection, delta, NoteSelection::VelocityMode::group);
            writeBackSelectionEdit (notes);
            break;
        }

        case DragMode::createNote:
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
    if (dragMode == DragMode::createNote || dragMode == DragMode::resizeGroup)
        if (auto* pat = pattern())
            if (juce::isPositiveAndBelow (dragNote, (int) pat->notes.size()))
                lastLength = pat->notes[(size_t) dragNote].length;

    // What a moved note does to what it lands on is resolved once, here, and
    // never while the pointer is moving. Resolving as the drag proceeds would
    // destroy every note the group passed over on its way, and an undo of the
    // move would not bring them back, because they were already gone when it
    // finished.
    if (dragMode == DragMode::moveGroup || dragMode == DragMode::resizeGroup)
        commitSelectionEdit();

    preview (-1);

    // A marquee is not an edit, so it must not push an undo step. Everything
    // else that got this far changed the pattern.
    const bool wasEditing = dragMode != DragMode::none && dragMode != DragMode::keys
                            && dragMode != DragMode::marquee;
    const bool wasMarquee = dragMode == DragMode::marquee;
    dragMode = DragMode::none;
    dragNote = -1;
    dragStartNotes.clear();
    marqueeOnScreen = {};
    if (wasEditing)
        changed (true);
    if (wasMarquee)
        repaint();
    updateScrollBars();
}
