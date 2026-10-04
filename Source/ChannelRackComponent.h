#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Project.h"
#include "AudioEngine.h"
#include "AhpLookAndFeel.h"

//==============================================================================
/** The channel rack: one row per instrument, one column per step.
*/
class ChannelRackComponent  : public juce::Component
{
public:
    ChannelRackComponent (Project& p, AudioEngine& e) : project (p), engine (e)
    {
        setWantsKeyboardFocus (false);

        auto addButton = [this] (juce::TextButton& b, const juce::String& text, const juce::String& tip)
        {
            b.setButtonText (text);
            b.setTooltip (tip);
            addAndMakeVisible (b);
        };

        swingBox.addItem ("Straight", 1);
        swingBox.addItem ("Swing 54", 2);
        swingBox.addItem ("Swing 58", 3);
        swingBox.addItem ("Swing 62", 4);
        swingBox.addItem ("Shuffle 66", 5);
        swingBox.setTooltip ("Where the offbeat sits inside its pair. 50 is straight, "
                             "66 lands it on the third triplet, which is a full shuffle. "
                             "Most records sit between 54 and 62.");
        swingBox.onChange = [this]
        {
            const double swings[] { 0.5, 0.54, 0.58, 0.62, 2.0 / 3.0 };
            const int index = juce::jlimit (0, 4, swingBox.getSelectedId() - 1);

            project.groove.swing   = swings[index];
            project.groove.enabled = index > 0;
            project.groove.base    = project.rackStepsPerBar >= 32 ? Groove::Base::thirtySecond
                                   : project.rackStepsPerBar >= 16 ? Groove::Base::sixteenth
                                                                   : Groove::Base::eighth;
            commit();
        };
        addAndMakeVisible (swingBox);

        feelBox.addItem ("Feel: tight", 1);
        feelBox.addItem ("Feel: played", 2);
        feelBox.addItem ("Feel: loose", 3);
        feelBox.setTooltip ("How much the offbeats soften and drift. Swing on its own still "
                            "sounds programmed; a played pattern is quieter and slightly "
                            "early or late on the offbeats.");
        feelBox.onChange = [this]
        {
            const double velocities[] { 0.0, 0.35, 0.6 };
            const double randoms[]    { 0.0, 0.15, 0.4 };
            const int index = juce::jlimit (0, 2, feelBox.getSelectedId() - 1);

            project.groove.velocity = velocities[index];
            project.groove.random   = randoms[index];
            commit();
        };
        addAndMakeVisible (feelBox);

        addButton (clearAll, "Clear", "Erase every step in this pattern");
        clearAll.onClick = [this]
        {
            for (int ch = 0; ch < kNumChannels; ++ch)
                project.clearRackRow (ch);
            commit();
        };

        for (auto* box : { &stepsBox, &barsBox })
            addAndMakeVisible (box);

        stepsBox.addItemList ({ "8 steps/bar", "16 steps/bar", "32 steps/bar" }, 1);
        stepsBox.setTooltip ("How finely the bar is divided");
        stepsBox.onChange = [this]
        {
            const int steps[] { 8, 16, 32 };
            project.rackStepsPerBar = steps[juce::jlimit (0, 2, stepsBox.getSelectedId() - 1)];
            commit();
        };

        barsBox.addItemList ({ "1 bar", "2 bars", "4 bars" }, 1);
        barsBox.setTooltip ("How long the pattern is");
        barsBox.onChange = [this]
        {
            const int bars[] { 1, 2, 4 };
            project.rackBars = bars[juce::jlimit (0, 2, barsBox.getSelectedId() - 1)];
            commit();
        };

        addButton (prevBar, "<", "Move the pattern a bar earlier");
        prevBar.onClick = [this]
        {
            project.rackStart = std::max (0.0, project.rackStart - 4.0);
            commit();
        };

        addButton (nextBar, ">", "Move the pattern a bar later");
        nextBar.onClick = [this]
        {
            project.rackStart += 4.0;
            commit();
        };

        addAndMakeVisible (positionLabel);
        positionLabel.setJustificationType (juce::Justification::centredLeft);
        positionLabel.setColour (juce::Label::textColourId, Ahp::muted);

        refresh();
    }

    /** True while a paint gesture is in progress, so dragging across a row
        becomes one undo step rather than one per cell. */
    bool isDragging() const noexcept { return dragging; }

    std::function<void()> onEdited;          // an edit worth an undo step
    std::function<void(int)> onSelectChannel; // row header clicked

    void refresh()
    {
        const int stepsIndex = project.rackStepsPerBar == 8 ? 1 : project.rackStepsPerBar == 32 ? 3 : 2;
        stepsBox.setSelectedId (stepsIndex, juce::dontSendNotification);

        const int barsIndex = project.rackBars == 2 ? 2 : project.rackBars == 4 ? 3 : 1;
        barsBox.setSelectedId (barsIndex, juce::dontSendNotification);

        positionLabel.setText ("Bar " + juce::String ((int) (project.rackStart / 4.0) + 1),
                               juce::dontSendNotification);

        const double swings[] { 0.5, 0.54, 0.58, 0.62, 2.0 / 3.0 };
        int swingIndex = 0;
        for (int i = 1; i < 5; ++i)
            if (project.groove.enabled && std::abs (project.groove.swing - swings[i]) < 0.005)
                swingIndex = i;
        swingBox.setSelectedId (swingIndex + 1, juce::dontSendNotification);

        const int feelIndex = project.groove.random >= 0.3 ? 2 : project.groove.velocity > 0.0 ? 1 : 0;
        feelBox.setSelectedId (feelIndex + 1, juce::dontSendNotification);
        repaint();
    }

    /** Highlights the step the transport is currently inside. */
    void updatePlayhead()
    {
        const double beat = engine.getBeatPosition();
        const double from = project.rackStart;
        const double to   = from + project.rackLengthBeats();

        const int step = (beat >= from && beat < to)
                       ? (int) ((beat - from) / project.rackStepBeats())
                       : -1;

        if (step != playingStep)
        {
            playingStep = step;
            repaint();
        }
    }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        g.fillAll (Ahp::black);

        const auto grid = gridArea();
        const int  rows = kNumChannels;
        const int  steps = project.rackStepCount();
        if (steps <= 0 || grid.isEmpty())
            return;

        const float rowH  = grid.getHeight() / (float) rows;
        const float stepW = grid.getWidth()  / (float) steps;
        const int   perBeat = std::max (1, project.rackStepsPerBar / 4);

        g.setFont (juce::Font (juce::FontOptions (11.0f)));

        for (int row = 0; row < rows; ++row)
        {
            const auto& info = project.channels[(size_t) row];
            const float y = grid.getY() + row * rowH;

            // ---- row header ----
            juce::Rectangle<float> header ((float) headerX(), y, (float) headerWidth, rowH);
            g.setColour (row % 2 ? Ahp::panel : Ahp::panel2);
            g.fillRect (header);

            const bool loaded = info.name.isNotEmpty();
            g.setColour (loaded ? Ahp::bone : Ahp::muted);
            g.drawText (juce::String (row + 1).paddedLeft ('0', 2) + "  "
                          + (loaded ? info.name : juce::String ("empty")),
                        header.reduced (8.0f, 0.0f).withTrimmedRight (46.0f),
                        juce::Justification::centredLeft, true);

            g.setColour (Ahp::muted);
            g.drawText (noteName (info.rackNote),
                        header.removeFromRight (44.0f).reduced (4.0f, 0.0f),
                        juce::Justification::centredRight, false);

            // ---- steps ----
            for (int s = 0; s < steps; ++s)
            {
                juce::Rectangle<float> cell (grid.getX() + s * stepW, y, stepW, rowH);
                auto inner = cell.reduced (1.5f);

                const bool onBeat = (s % perBeat) == 0;
                const bool onBar  = (s % std::max (1, project.rackStepsPerBar)) == 0;

                g.setColour (onBar ? Ahp::panel3 : onBeat ? Ahp::panel2 : Ahp::panel);
                g.fillRect (inner);

                if (s == playingStep)
                {
                    g.setColour (Ahp::bone.withAlpha (0.10f));
                    g.fillRect (inner);
                }

                if (auto* note = project.rackNoteAt (row, s))
                {
                    // Velocity reads as how full the block is, so a pattern's
                    // dynamics are visible without opening anything.
                    const float amount = juce::jlimit (0.15f, 1.0f, note->velocity);
                    auto lit = inner.withTrimmedTop (inner.getHeight() * (1.0f - amount));
                    g.setColour (loaded ? Ahp::bone : Ahp::muted);
                    g.fillRect (lit);
                }
            }
        }

        // grid outline
        g.setColour (Ahp::line);
        g.drawRect (grid.toFloat(), 1.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto top = r.removeFromTop (30).reduced (6, 4);

        clearAll.setBounds (top.removeFromRight (64));
        top.removeFromRight (6);
        feelBox.setBounds (top.removeFromRight (104));
        top.removeFromRight (6);
        swingBox.setBounds (top.removeFromRight (110));
        top.removeFromRight (6);
        barsBox.setBounds (top.removeFromRight (90));
        top.removeFromRight (6);
        stepsBox.setBounds (top.removeFromRight (120));

        prevBar.setBounds (top.removeFromLeft (28));
        top.removeFromLeft (4);
        nextBar.setBounds (top.removeFromLeft (28));
        top.removeFromLeft (8);
        positionLabel.setBounds (top.removeFromLeft (90));
    }

    //==========================================================================
    void mouseDown (const juce::MouseEvent& e) override
    {
        const int row = rowAt (e.position);
        if (row < 0)
            return;

        if (e.position.x < gridArea().getX())
        {
            if (e.mods.isPopupMenu())
                showRowMenu (row);
            else if (onSelectChannel)
                onSelectChannel (row);
            return;
        }

        const int step = stepAt (e.position);
        if (step < 0)
            return;

        // Holding shift on a lit step sets its velocity by how high in the
        // cell you drag, which is how hardware step sequencers have always
        // done accents. Without varied velocity a programmed beat sounds like
        // a machine, so this wants to be as quick as drawing the step itself.
        if (e.mods.isShiftDown() && project.rackStepOn (row, step))
        {
            shading = true;
            dragging = true;
            shadeAt (row, step, e.position);
            return;
        }

        // Painting: the first cell decides whether this gesture draws or
        // erases, so dragging across a row does one thing rather than
        // flipping every step it crosses.
        painting = ! project.rackStepOn (row, step) && ! e.mods.isRightButtonDown();
        shading = false;
        dragging = true;
        applyTo (row, step);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging)
            return;

        const int row  = rowAt (e.position);
        const int step = stepAt (e.position);
        if (row < 0 || step < 0)
            return;

        if (shading)
            shadeAt (row, step, e.position);
        else
            applyTo (row, step);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging)
        {
            dragging = false;
            shading = false;
            commit();
        }
    }

private:
    //==========================================================================
    int  headerX()     const noexcept { return 0; }
    static constexpr int headerWidth = 190;

    juce::Rectangle<int> gridArea() const
    {
        return getLocalBounds().withTrimmedTop (30).withTrimmedLeft (headerWidth).reduced (0, 4);
    }

    int rowAt (juce::Point<float> p) const
    {
        const auto grid = gridArea();
        if (p.y < grid.getY() || p.y >= grid.getBottom())
            return -1;
        const float rowH = grid.getHeight() / (float) kNumChannels;
        return juce::jlimit (0, kNumChannels - 1, (int) ((p.y - grid.getY()) / rowH));
    }

    int stepAt (juce::Point<float> p) const
    {
        const auto grid = gridArea();
        const int steps = project.rackStepCount();
        if (steps <= 0 || p.x < grid.getX() || p.x >= grid.getRight())
            return -1;
        const float stepW = grid.getWidth() / (float) steps;
        return juce::jlimit (0, steps - 1, (int) ((p.x - grid.getX()) / stepW));
    }

    /** Velocity from how high in the row the pointer is, so dragging up and
        down a lit step shapes the accent directly. */
    void shadeAt (int row, int step, juce::Point<float> p)
    {
        if (! project.rackStepOn (row, step))
            return;

        const auto grid = gridArea();
        const float rowH = grid.getHeight() / (float) kNumChannels;
        const float top  = grid.getY() + row * rowH;
        const float amount = juce::jlimit (0.05f, 1.0f, 1.0f - (p.y - top) / rowH);

        project.setRackStep (row, step, true, amount);
        repaint();
    }

    void applyTo (int row, int step)
    {
        if (project.rackStepOn (row, step) == painting)
            return;                       // already in the state this drag wants

        project.setRackStep (row, step, painting);
        repaint();
    }

    void commit()
    {
        refresh();
        if (onEdited)
            onEdited();
    }

    void showRowMenu (int row)
    {
        juce::PopupMenu menu;
        menu.addSectionHeader ("Channel " + juce::String (row + 1));
        menu.addItem (1, "Clear row");
        menu.addSeparator();
        menu.addSectionHeader ("Fill");
        menu.addItem (2, "Every beat");
        menu.addItem (3, "Every other step");
        menu.addItem (4, "Every 4th step");
        menu.addSeparator();
        menu.addSectionHeader ("Step note: " + noteName (project.channels[(size_t) row].rackNote));
        menu.addItem (5, "Up a semitone");
        menu.addItem (6, "Down a semitone");
        menu.addItem (7, "Up an octave");
        menu.addItem (8, "Down an octave");

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                            [this, row] (int result)
        {
            if (result == 0)
                return;

            const int steps   = project.rackStepCount();
            const int perBeat = std::max (1, project.rackStepsPerBar / 4);
            auto& info = project.channels[(size_t) row];

            switch (result)
            {
                case 1: project.clearRackRow (row); break;

                case 2:
                case 3:
                case 4:
                {
                    const int every = result == 2 ? perBeat : result == 3 ? 2 : 4;
                    project.clearRackRow (row);
                    for (int s = 0; s < steps; s += std::max (1, every))
                        project.setRackStep (row, s, true);
                    break;
                }

                case 5: project.setRackNote (row, info.rackNote + 1);  break;
                case 6: project.setRackNote (row, info.rackNote - 1);  break;
                case 7: project.setRackNote (row, info.rackNote + 12); break;
                case 8: project.setRackNote (row, info.rackNote - 12); break;
                default: break;
            }

            commit();
        });
    }

    static juce::String noteName (int note)
    {
        return juce::MidiMessage::getMidiNoteName (note, true, true, 4);
    }

    Project&     project;
    AudioEngine& engine;

    juce::TextButton clearAll, prevBar { "<" }, nextBar { ">" };
    juce::ComboBox   stepsBox, barsBox, swingBox, feelBox;
    juce::Label      positionLabel;

    bool painting = false, dragging = false, shading = false;
    int  playingStep = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelRackComponent)
};
