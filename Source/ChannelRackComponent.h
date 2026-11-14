#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
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
            const int index = juce::jlimit (0, Groove::kNumOfferedSwings - 1,
                                            swingBox.getSelectedId() - 1);

            Groove g = project.groove;
            g.swing   = Groove::offeredSwing (index);
            g.enabled = index > 0;
            g.base    = project.rackStepsPerBar >= 32 ? Groove::Base::thirtySecond
                      : project.rackStepsPerBar >= 16 ? Groove::Base::sixteenth
                                                      : Groove::Base::eighth;
            project.setGroove (g);
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

            Groove g = project.groove;
            g.velocity = velocities[index];
            g.random   = randoms[index];
            project.setGroove (g);
            commit();
        };
        addAndMakeVisible (feelBox);

        addButton (feelFrom, "Feel from a part...",
                   "Take the timing out of a part somebody played and put it on the "
                   "steps here. Reads the part at the step resolution above, so a feel "
                   "read at sixteenths swings sixteenths. The donor's accents come too, "
                   "as far as the Feel control beside this is up: at Tight the timing "
                   "is taken and the accents are not.");
        feelFrom.onClick = [this] { showFeelMenu(); };

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

        const int swingIndex = project.groove.enabled
                             ? Groove::nearestOfferedSwing (project.groove.swing) : 0;
        swingBox.setSelectedId (swingIndex + 1, juce::dontSendNotification);

        const int feelIndex = project.groove.random >= 0.3 ? 2 : project.groove.velocity > 0.0 ? 1 : 0;
        feelBox.setSelectedId (feelIndex + 1, juce::dontSendNotification);

        // A loaded feel supplies the timing in place of the swing number, so
        // leaving the swing control live would offer a choice that does
        // nothing. It is switched off rather than hidden, so that what is
        // going on is visible rather than mysterious.
        const bool hasFeel = ! project.groove.feel.isEmpty();
        feelFrom.setButtonText (hasFeel ? "Feel: from a part" : "Feel from a part...");
        swingBox.setEnabled (! hasFeel);

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
        feelFrom.setBounds (top.removeFromRight (136));
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

    /** Every MIDI clip in the song, as somewhere a feel could be read from.
        Named by where it is rather than by an index, because an index means
        nothing to somebody looking at an arrangement. */
    void showFeelMenu()
    {
        // Out of the range the clip items can reach, and disabled anyway, but
        // the bounds check below is what actually makes it unreachable.
        static constexpr int kNothingToOffer = 1'000'000;

        juce::PopupMenu menu;

        if (! project.groove.feel.isEmpty())
        {
            menu.addItem (1, "No feel (go back to the swing)");
            menu.addSeparator();
        }

        menu.addSectionHeader ("Take the feel from");

        // The clips are named by id rather than by position in the vector.
        // The menu is asynchronous, so by the time the callback runs an index
        // can name a different clip than the one that was clicked, and
        // `Project::find` is what everything else here uses for that reason.
        std::vector<int> offered;

        for (const auto& c : project.clips)
        {
            if (! c.isMidi() || c.pattern == nullptr || c.pattern->notes.empty())
                continue;

            // Offered only if a feel really comes out of it. A clip can hold
            // a pattern with notes and play a stretch of it that has none,
            // after a trim or a split, and offering that is a menu item that
            // does nothing when it is picked with nothing to say why.
            if (feelOf (c).isEmpty())
                continue;

            juce::String what = "Bar " + juce::String ((int) project.meter.barBeatAt (c.start).bar);

            if (! project.channels.empty())
            {
                const int channel = juce::jlimit (0, (int) project.channels.size() - 1, c.channel);

                if (project.channels[(std::size_t) channel].name.isNotEmpty())
                    what += ", " + project.channels[(std::size_t) channel].name;
            }

            offered.push_back (c.id);
            menu.addItem ((int) offered.size() + 1, what);
        }

        if (offered.empty())
            menu.addItem (kNothingToOffer, "Nothing recorded yet", false, false);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (feelFrom),
                            [this, offered] (int choice)
        {
            if (choice == 0)
                return;

            Groove g = project.groove;

            if (choice == 1)
            {
                g.feel = {};

                // Back to the swing, and to a swing the control can actually
                // show. Leaving `enabled` alone left the groove switched on
                // with the humanise running while the swing box read
                // "Straight", and picking "Straight" sends no change because
                // the item is already current, so no control in the window
                // could turn it off again.
                //
                // Testing `swing != 0.5` is not enough, because the box reads
                // Straight for any swing it does not recognise and a file can
                // carry one: 0.6 would come back enabled, inaudibly swung and
                // stuck all over again. So the swing is snapped to the nearest
                // value the box offers and `enabled` follows that, which
                // leaves the groove in a state the controls can represent and
                // therefore undo. Snapping moves the swing by at most a
                // fortieth, and only for a value this build never writes.
                const int nearest = Groove::nearestOfferedSwing (g.swing);

                g.swing   = Groove::offeredSwing (nearest);
                g.enabled = nearest > 0;
            }
            else
            {
                const int which = choice - 2;

                if (which < 0 || which >= (int) offered.size())
                    return;

                // By id, because the menu is asynchronous and the clip that
                // was clicked may have been moved, deleted or replaced in the
                // vector since. An index would quietly read a different clip.
                const Clip* c = project.find (offered[(std::size_t) which]);

                if (c == nullptr || ! c->isMidi() || c->pattern == nullptr)
                    return;

                const auto read = feelOf (*c);

                if (read.isEmpty())
                    return;

                g.feel = read;

                // Reading a feel is asking for it, so it is switched on. The
                // velocity and random knobs are left where they were, which
                // means the donor's accents are passed on only as far as the
                // Feel control is up: at "tight" the timing is taken and the
                // accents are not. That is what the control is for and the
                // tooltip says so.
                g.enabled = true;
            }

            project.setGroove (g);
            commit();
        });
    }

    /** Reads a clip's feel at the resolution the rack is showing.

        Everything that decides what the feel is, including the anchor and
        the window, is in `GrooveTemplate::fromClip` where it is tested. This
        is only the marshalling: which notes, and at what spacing. */
    GrooveTemplate feelOf (const Clip& c) const
    {
        if (c.pattern == nullptr)
            return {};

        // The rack's own step, rather than a second table of the same three
        // numbers. They agree today and a fourth entry in the steps box would
        // make them disagree silently, which is a feel read at the wrong
        // resolution and nothing on screen to say so.
        const double slot = project.rackStepBeats();

        std::vector<GrooveTemplate::SourceNote> notes;
        notes.reserve (c.pattern->notes.size());

        for (const auto& n : c.pattern->notes)
            notes.push_back ({ n.start, n.velocity, n.note });

        return GrooveTemplate::fromClip (std::move (notes), c.offset, c.length, slot);
    }

    Project&     project;
    AudioEngine& engine;

    juce::TextButton clearAll, feelFrom, prevBar { "<" }, nextBar { ">" };
    juce::ComboBox   stepsBox, barsBox, swingBox, feelBox;
    juce::Label      positionLabel;

    bool painting = false, dragging = false, shading = false;
    int  playingStep = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelRackComponent)
};
