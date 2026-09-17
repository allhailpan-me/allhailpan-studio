#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Project.h"
#include "AudioEngine.h"

// The FL-style playlist: tracks, audio and MIDI clips, a ruler you can click
// to play from, snapping from bars to free placement, trimming or stretching,
// marquee selection, copy and paste, and zoom.
class PlaylistComponent : public juce::Component,
                          public juce::DragAndDropTarget,
                          public juce::FileDragAndDropTarget,
                          private juce::ScrollBar::Listener
{
public:
    PlaylistComponent (Project&, AudioEngine&, SampleCache&);
    ~PlaylistComponent() override;

    std::function<void()>              onInteraction;     // keep keyboard focus in the main window
    std::function<void(double)>        onPlayFrom;        // start playback at a beat
    std::function<void(double)>        onSetPosition;     // move the playhead / start marker
    std::function<void(int)>           onOpenPianoRoll;   // clip id
    std::function<bool(const Clip&)>   isRendering;       // stretch render in progress?
    std::function<void(int)>           onRouteTrack;      // track index changed its insert

    bool isEditing() const noexcept;   // true while the mouse or a slider is mid-drag

    void deleteSelection();
    void duplicateSelection();
    void copySelection();
    void cutSelection();
    void paste();
    void selectAll();

    void updatePlayhead();
    void refresh();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragMove (const juce::StringArray&, int x, int y) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;

private:
    enum class Tool { draw, slice, erase };
    enum class Zone { none, ruler, header, grid };
    enum class Edge { none, left, right };

    struct Hit
    {
        Zone   zone   = Zone::none;
        double beat   = 0.0;
        int    track  = -1;
        int    clipId = 0;
        Edge   edge   = Edge::none;
    };

    struct DragState
    {
        enum class Mode { none, move, trimLeft, trimRight, stretchLeft, stretchRight, erase, marquee } mode = Mode::none;
        int    clipId  = 0;
        double grab    = 0.0;
        int    track0  = 0;
        double anchor0 = 0.0;
        bool   moved   = false;
        bool   ctrlClick = false;
        double beat0 = 0.0, beat1 = 0.0;
        int    trackA = 0, trackB = 0;
        std::vector<Clip> originals;
    };

    Hit    hitTest (juce::Point<float>) const;
    double snapValue (const juce::ModifierKeys&) const;
    double snap (double beat, const juce::ModifierKeys&, bool roundDown = false) const;
    float  beatToX (double beat) const  { return (float) (grid.getX() + (beat - scrollBeats) * ppb); }
    double xToBeat (float x) const      { return scrollBeats + (x - (float) grid.getX()) / ppb; }
    float  trackToY (int t) const       { return (float) grid.getY() + (float) (t * trackH) - scrollY; }
    int    yToTrack (float y) const     { return (int) std::floor ((y - (float) grid.getY() + scrollY) / trackH); }
    juce::Rectangle<float> clipBounds (const Clip&) const;

    void zoom (double factor, float anchorX);
    void updateScrollBars();
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;
    void setTool (Tool);
    void addFiles (const juce::StringArray& paths, double beat, int track);
    void setDropHint (int x, int y);
    void paintClip (juce::Graphics&, const Clip&, juce::Rectangle<float>, bool selected);
    void showTrackMenu (int track);

    // clip settings bar
    void updateClipBar();
    void layoutClipBar (juce::Rectangle<int>);
    Clip* singleSelected();

    Project&     project;
    AudioEngine& engine;
    SampleCache& cache;
    juce::Image  watermark;

    juce::TextButton drawButton { "Draw" }, sliceButton { "Slice" }, eraseButton { "Delete" };
    juce::TextButton stretchButton { "Stretch" };
    juce::TextButton zoomOut { "-" }, zoomFit { "Fit" }, zoomIn { "+" };
    juce::ComboBox   snapBox;
    juce::Label      snapLabel { {}, "Snap" }, positionLabel;
    juce::ScrollBar  hbar { false }, vbar { true };

    // clip bar
    juce::Label      clipName, clipStatus, pitchLabel { {}, "Pitch" }, stretchLabel { {}, "Stretch" }, gainLabel { {}, "Gain" };
    juce::Slider     pitchSlider, stretchSlider, gainSlider;
    juce::TextButton resetButton { "Reset" }, openRollButton { "Open piano roll" };
    juce::ComboBox   channelBox;
    bool             sliderDragging = false;
    bool             updatingClipBar = false;

    Tool tool = Tool::draw;
    static constexpr int toolbarH = 38, headerW = 150, rulerH = 26, trackH = 50, barThickness = 12, clipBarH = 40;

    juce::Rectangle<int> toolbar, ruler, header, grid, clipBar;
    double ppb = 24.0;
    double scrollBeats = 0.0;
    float  scrollY = 0.0f;
    double lastPlayhead = -1.0;

    DragState drag;
    double    hoverBeat = 0.0;
    int       hoverTrack = -1;
    bool      mouseOverGrid = false;
    bool      showDrop = false;
    double    dropBeat = 0.0;
    int       dropTrack = 0;

    std::vector<Clip> clipboard;     // positions relative to the first clip
    int clipboardFirstTrack = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaylistComponent)
};
