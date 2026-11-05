#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Project.h"
#include "AudioEngine.h"
#include "Quantise.h"
#include "NoteSelection.h"

// Edits the notes and recorded parameter automation of one MIDI clip.
class PianoRollComponent : public juce::Component,
                           private juce::ScrollBar::Listener
{
public:
    PianoRollComponent (Project&, AudioEngine&);
    ~PianoRollComponent() override;

    void setClip (int clipId);
    int  getClipId() const noexcept { return clipId; }

    std::function<void()>       onInteraction;
    std::function<void(double)> onSetPosition;       // ruler clicks (song beats)

    void refresh();
    void updatePlayhead();
    bool isDragging() const noexcept { return dragMode != DragMode::none; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    // moveNote and resizeNote are gone rather than kept beside the group
    // modes. A single note is now a selection of one and goes down the same
    // path, which is the only way the two can be guaranteed to agree: two
    // code paths for the same gesture is how a single note and a group end up
    // snapping differently, and nobody would know which was right.
    enum class DragMode { none, createNote, erase, velocity, autoPoint, keys,
                          marquee, moveGroup, resizeGroup, velocityGroup };

    Clip*        clip();
    MidiPattern* pattern();
    AutoLane*    currentLane();
    void  rebuildLaneBox();
    void  preview (int note);
    double snapValue (const juce::ModifierKeys&) const;
    double snap (double beat, const juce::ModifierKeys&, bool roundDown = false) const;
    float  beatToX (double b) const { return (float) (grid.getX() + (b - scrollBeats) * ppb); }
    double xToBeat (float x) const  { return scrollBeats + (x - (float) grid.getX()) / ppb; }
    float  noteToY (int note) const { return (float) grid.getY() + (float) ((127 - note) * rowH) - scrollY; }
    int    yToNote (float y) const  { return 127 - (int) std::floor ((y - (float) grid.getY() + scrollY) / rowH); }
    int    noteAt (juce::Point<float>, bool& onRightEdge) const;
    void   updateScrollBars();
    void   scrollBarMoved (juce::ScrollBar*, double) override;
    void   zoom (double factor, float anchorX);
    void   changed (bool final);

    void   showSelectionMenu();
    void   selectOnly (int index);
    void   clearSelection();
    void   validateSelection();
    NoteSelection::Limits selectionLimits() const;
    NoteSelection::Snap   snapModeFor (const juce::ModifierKeys&) const;
    void   beginGroupDrag (const juce::MouseEvent&, int grabbedIndex, bool resizing);
    void   dragGroup (const juce::MouseEvent&);
    void   commitSelectionEdit();
    void   rememberSelection();
    bool   marqueeWanted (const juce::MouseEvent&, int noteUnderPointer) const;
    void   writeBackSelectionEdit (const std::vector<NoteSelection::Note>&);
    void   growClipToFitSelection();

    void   showQuantiseMenu();
    void   applyQuantise();
    double quantiseGrid() const;
    juce::String quantiseGridName() const;
    void   describeQuantise();

    Project&     project;
    AudioEngine& engine;
    int clipId = 0;

    juce::Label      title;
    juce::ComboBox   laneBox, snapBox;
    juce::Label      laneLabel { {}, "Lane" }, snapLabel { {}, "Snap" };
    juce::TextButton drawButton { "Draw" }, eraseButton { "Delete" }, selectButton { "Select" },
                     clearLaneButton { "Clear lane" }, quantiseButton { "Quantise" },
                     selectionButton { "Selection" };
    juce::ScrollBar  hbar { false }, vbar { true };

    static constexpr int toolbarH = 38, keysW = 64, rulerH = 22, rowH = 14, laneH = 120, barThickness = 12;
    juce::Rectangle<int> toolbar, ruler, keys, grid, laneArea;

    double ppb = 64.0, scrollBeats = 0.0;
    float  scrollY = 0.0f;
    bool   eraseTool = false;
    bool   selectTool = false;
    bool   initialScrollDone = false;
    bool   wasPlaying = false;

    DragMode dragMode = DragMode::none;
    int      dragNote = -1;
    double   lastLength = 0.25;
    int      lastPreviewNote = -1;
    int      heldPreview = -1;
    float    lastVelocityX = 0.0f;
    AutoPoint draggedPoint;

    // Which notes a group edit will act on, and the state the pattern was in
    // when the gesture began.
    //
    // The snapshot is not an optimisation. A group drag computes its delta
    // from where the pointer started, against the notes as they were then,
    // and writes the answer over the top. Applying each mouse move to the
    // result of the last one instead would snap an already snapped position
    // every few milliseconds, and the group would creep away from the
    // pointer by an amount that depends on how fast the mouse was moved.
    NoteSelection::Set    selection;

    // The selected notes themselves, remembered so that an index can be
    // checked rather than trusted. See validateSelection: an undo rebuilds
    // the note list from a deep copy, so every index can still be in range
    // and every one of them point at a different note.
    std::vector<MidiNote> selectionNotes;

    std::vector<MidiNote> dragStartNotes;
    bool   groupDragMoved = false;
    double dragStartBeat  = 0.0;
    int    dragStartPitch = 0;
    double dragAnchorStart = 0.0;
    double dragAnchorEnd   = 0.0;
    float  dragStartVelocityY = 0.0f;

    juce::Point<float>   marqueeFrom;
    juce::Rectangle<float> marqueeOnScreen;
    NoteSelection::Set   marqueeBefore;
    bool                 marqueeReverses = false;

    // Absolute to match what dragging one note has always done here, and what
    // Apple and Steinberg both default to. Relative is the other published
    // mode and is on the Selection menu. Sticky across clips, the way the
    // quantise options above are, and for the same reason: one answer on
    // screen rather than two that can disagree. It belongs in Preferences
    // eventually, next to the rest of the editing settings.
    NoteSelection::Snap snapMode = NoteSelection::Snap::absolute;

    // Sticky across clips, the way a quantise dialog's settings are in every
    // studio: the grid comes from the Snap box, so there is one grid on screen
    // rather than two that can disagree.
    Quantise::Options quantiseOptions;

    // What the last pass did, so the button can say so. A note merged into
    // another is the one outcome somebody would want to know about and the
    // easiest to miss on a crowded pattern.
    Quantise::Result lastQuantise;
    bool hasQuantised = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollComponent)
};
