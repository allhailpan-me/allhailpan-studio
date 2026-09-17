#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Project.h"
#include "AudioEngine.h"

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
    enum class DragMode { none, createNote, moveNote, resizeNote, erase, velocity, autoPoint, keys };

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

    Project&     project;
    AudioEngine& engine;
    int clipId = 0;

    juce::Label      title;
    juce::ComboBox   laneBox, snapBox;
    juce::Label      laneLabel { {}, "Lane" }, snapLabel { {}, "Snap" };
    juce::TextButton drawButton { "Draw" }, eraseButton { "Delete" }, clearLaneButton { "Clear lane" };
    juce::ScrollBar  hbar { false }, vbar { true };

    static constexpr int toolbarH = 38, keysW = 64, rulerH = 22, rowH = 14, laneH = 120, barThickness = 12;
    juce::Rectangle<int> toolbar, ruler, keys, grid, laneArea;

    double ppb = 64.0, scrollBeats = 0.0;
    float  scrollY = 0.0f;
    bool   eraseTool = false;
    bool   initialScrollDone = false;
    bool   wasPlaying = false;

    DragMode dragMode = DragMode::none;
    int      dragNote = -1;
    double   grabOffset = 0.0;
    int      grabPitch = 0;
    double   lastLength = 0.25;
    int      lastPreviewNote = -1;
    int      heldPreview = -1;
    float    lastVelocityX = 0.0f;
    AutoPoint draggedPoint;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollComponent)
};
