#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Project.h"
#include "AudioEngine.h"
#include "AhpLookAndFeel.h"

//==============================================================================
/** The modulators panel.

    Each modulator is a shape running in time, wired to any number of plugin
    parameters. Because it drives ordinary parameters it works on anything the
    studio can host, not only on built in devices.

    Wiring is done by learning: arm a modulator, then grab the knob you want it
    on inside the plugin's own window. That avoids asking anyone to hunt for a
    parameter by number in a list of several hundred.
*/
class ModulatorPanel  : public juce::Component,
                        private juce::Timer
{
public:
    ModulatorPanel (Project& p, AudioEngine& e) : project (p), engine (e)
    {
        addAndMakeVisible (title);
        title.setText ("Modulators", juce::dontSendNotification);
        title.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        title.setColour (juce::Label::textColourId, Ahp::bone);

        addAndMakeVisible (hint);
        hint.setColour (juce::Label::textColourId, Ahp::muted);
        hint.setFont (juce::FontOptions (11.5f));
        hint.setJustificationType (juce::Justification::topLeft);

        addAndMakeVisible (addButton);
        addButton.setButtonText ("+  add modulator");
        addButton.onClick = [this]
        {
            selected = project.addModulator();
            rebuild();
        };

        addAndMakeVisible (viewport);
        viewport.setViewedComponent (&list, false);
        viewport.setScrollBarsShown (true, false);

        startTimerHz (20);
        rebuild();
    }

    ~ModulatorPanel() override { viewport.setViewedComponent (nullptr, false); }

    std::function<void()>    onEdited;
    std::function<int()>     getSelectedChannel;   // which plugin learning listens to

    void refresh() { rebuild(); }

    /** Called when the user grabs a knob in a plugin window. If a modulator is
        waiting to learn, that parameter becomes its target. */
    void parameterTouched (int channel, int paramIndex, const juce::String& paramName)
    {
        if (learning < 0)
            return;

        project.assignModulation (learning, channel, paramIndex, paramName, 0.5f);
        learning = -1;
        rebuild();

        if (onEdited)
            onEdited();
    }

    bool isLearning() const noexcept { return learning >= 0; }

    void paint (juce::Graphics& g) override { g.fillAll (Ahp::black); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        title.setBounds (r.removeFromTop (22));
        hint.setBounds (r.removeFromTop (34));
        r.removeFromTop (4);
        addButton.setBounds (r.removeFromTop (26).removeFromLeft (150));
        r.removeFromTop (8);
        viewport.setBounds (r);
        layoutList();
    }

private:
    //==========================================================================
    struct Row : public juce::Component
    {
        Row (ModulatorPanel& ownerIn, int indexIn) : owner (ownerIn), index (indexIn)
        {
            auto* mod = owner.project.modulator (index);

            name.setText (mod != nullptr ? mod->name : juce::String(), juce::dontSendNotification);
            name.setColour (juce::Label::textColourId, Ahp::bone);
            name.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            name.setEditable (false, true, false);
            name.onTextChange = [this]
            {
                if (auto* m = owner.project.modulator (index))
                {
                    m->name = name.getText();
                    owner.project.changed();
                }
            };
            addAndMakeVisible (name);

            shape.addItem ("Sine", 1);
            shape.addItem ("Triangle", 2);
            shape.addItem ("Saw down", 3);
            shape.addItem ("Ramp up", 4);
            shape.addItem ("Square", 5);
            shape.addItem ("Random", 6);
            shape.addItem ("Sample and hold", 7);
            shape.setSelectedId (mod != nullptr ? (int) mod->shape + 1 : 1, juce::dontSendNotification);
            shape.onChange = [this]
            {
                if (auto* m = owner.project.modulator (index))
                {
                    m->shape = (ModShape) (shape.getSelectedId() - 1);
                    owner.project.changed();
                }
            };
            addAndMakeVisible (shape);

            // Rates are musical divisions rather than hertz, so everything
            // stays locked to the tempo.
            const double beats[] { 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0 };
            const char*  names[] { "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars", "8 bars" };
            for (int i = 0; i < 8; ++i)
            {
                rate.addItem (names[i], i + 1);
                rateBeats[i] = beats[i];
            }
            rate.setSelectedId (nearestRate (mod != nullptr ? mod->rateBeats : 4.0), juce::dontSendNotification);
            rate.onChange = [this]
            {
                if (auto* m = owner.project.modulator (index))
                {
                    m->rateBeats = rateBeats[juce::jlimit (0, 7, rate.getSelectedId() - 1)];
                    owner.project.changed();
                }
            };
            addAndMakeVisible (rate);

            polarity.setButtonText ("bipolar");
            polarity.setClickingTogglesState (true);
            polarity.setToggleState (mod != nullptr && mod->bipolar, juce::dontSendNotification);
            polarity.setTooltip ("Bipolar swings either side of the knob's own value; "
                                 "unipolar only moves it upward.");
            polarity.onClick = [this]
            {
                if (auto* m = owner.project.modulator (index))
                {
                    m->bipolar = polarity.getToggleState();
                    polarity.setButtonText (m->bipolar ? "bipolar" : "unipolar");
                    owner.project.changed();
                }
            };
            addAndMakeVisible (polarity);

            learn.setButtonText ("Learn");
            learn.setTooltip ("Click, then grab a knob in the plugin's window to wire it up");
            learn.onClick = [this] { owner.beginLearning (index); };
            addAndMakeVisible (learn);

            remove.setButtonText ("x");
            remove.setTooltip ("Delete this modulator");
            remove.onClick = [this]
            {
                owner.project.removeModulator (index);
                owner.rebuild();
                if (owner.onEdited) owner.onEdited();
            };
            addAndMakeVisible (remove);

            rebuildTargets();
        }

        int nearestRate (double beats) const
        {
            int best = 5;
            double bestDistance = 1.0e9;
            for (int i = 0; i < 8; ++i)
            {
                const double d = std::abs (std::log (rateBeats[i] / std::max (1.0e-6, beats)));
                if (d < bestDistance) { bestDistance = d; best = i + 1; }
            }
            return best;
        }

        void rebuildTargets()
        {
            depths.clear();
            unassign.clear();
            targetNames.clear();

            auto* mod = owner.project.modulator (index);
            if (mod == nullptr)
                return;

            for (int t = 0; t < (int) mod->targets.size(); ++t)
            {
                const auto& target = mod->targets[(size_t) t];

                auto* label = targetNames.add (new juce::Label());
                label->setText ("ch " + juce::String (target.channel + 1) + "   " + target.paramName,
                                juce::dontSendNotification);
                label->setColour (juce::Label::textColourId, Ahp::muted);
                label->setFont (juce::FontOptions (11.5f));
                addAndMakeVisible (label);

                auto* depth = depths.add (new juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::NoTextBox));
                depth->setRange (-1.0, 1.0, 0.001);
                depth->setValue (target.depth, juce::dontSendNotification);
                depth->setDoubleClickReturnValue (true, 0.0);
                depth->setColour (juce::Slider::trackColourId, Ahp::bone);
                depth->setColour (juce::Slider::backgroundColourId, Ahp::panel3);
                depth->onValueChange = [this, t]
                {
                    if (auto* m = owner.project.modulator (index))
                        if (juce::isPositiveAndBelow (t, (int) m->targets.size()))
                        {
                            m->targets[(size_t) t].depth = (float) depths[t]->getValue();
                            owner.project.changed();
                        }
                };
                addAndMakeVisible (depth);

                auto* drop = unassign.add (new juce::TextButton ("x"));
                drop->onClick = [this, t]
                {
                    owner.project.removeModulation (index, t);
                    owner.rebuild();
                    if (owner.onEdited) owner.onEdited();
                };
                addAndMakeVisible (drop);
            }
        }

        int wantedHeight() const
        {
            auto* mod = owner.project.modulator (index);
            const int targets = mod != nullptr ? (int) mod->targets.size() : 0;
            return 70 + targets * 26 + 8;
        }

        void paint (juce::Graphics& g) override
        {
            g.setColour (Ahp::panel);
            g.fillRect (getLocalBounds());
            g.setColour (owner.learning == index ? Ahp::rec : Ahp::line);
            g.drawRect (getLocalBounds(), 1);

            // A little picture of the shape, so the list reads at a glance.
            auto* mod = owner.project.modulator (index);
            if (mod == nullptr)
                return;

            auto box = juce::Rectangle<float> (getWidth() - 96.0f, 8.0f, 84.0f, 26.0f);
            g.setColour (Ahp::panel3);
            g.fillRect (box);

            juce::Path path;
            for (int x = 0; x <= (int) box.getWidth(); ++x)
            {
                const double beat = (x / (double) box.getWidth()) * mod->rateBeats;
                const float v = mod->valueAt (beat);
                const float norm = mod->bipolar ? (v + 1.0f) * 0.5f : v;
                const float y = box.getBottom() - 3.0f - norm * (box.getHeight() - 6.0f);
                if (x == 0) path.startNewSubPath (box.getX() + x, y);
                else        path.lineTo (box.getX() + x, y);
            }
            g.setColour (mod->enabled ? Ahp::bone : Ahp::muted);
            g.strokePath (path, juce::PathStrokeType (1.2f));
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (8, 6);

            auto top = r.removeFromTop (24);
            name.setBounds (top.removeFromLeft (120));
            top.removeFromLeft (100);           // room for the shape drawing
            remove.setBounds (top.removeFromRight (24));

            r.removeFromTop (4);
            auto controls = r.removeFromTop (24);
            shape.setBounds (controls.removeFromLeft (130));
            controls.removeFromLeft (6);
            rate.setBounds (controls.removeFromLeft (80));
            controls.removeFromLeft (6);
            polarity.setBounds (controls.removeFromLeft (76));
            controls.removeFromLeft (6);
            learn.setBounds (controls.removeFromLeft (64));

            r.removeFromTop (6);
            for (int t = 0; t < depths.size(); ++t)
            {
                auto row = r.removeFromTop (22);
                targetNames[t]->setBounds (row.removeFromLeft (190));
                unassign[t]->setBounds (row.removeFromRight (24));
                row.removeFromRight (6);
                depths[t]->setBounds (row);
                r.removeFromTop (4);
            }
        }

        ModulatorPanel& owner;
        int index = 0;

        juce::Label      name;
        juce::ComboBox   shape, rate;
        juce::TextButton polarity, learn, remove;
        juce::OwnedArray<juce::Slider>     depths;
        juce::OwnedArray<juce::TextButton> unassign;
        juce::OwnedArray<juce::Label>      targetNames;
        double rateBeats[8] {};
    };

    //==========================================================================
    void beginLearning (int index)
    {
        learning = (learning == index) ? -1 : index;
        updateHint();
        for (auto* r : rows)
            r->repaint();
    }

    void updateHint()
    {
        if (learning >= 0)
            hint.setText ("Waiting: open the plugin for the selected channel and move the knob "
                          "you want this modulator on. Click Learn again to cancel.",
                          juce::dontSendNotification);
        else
            hint.setText ("A modulator moves any plugin parameter in time, locked to the tempo. "
                          "Click Learn, then grab a knob in the plugin's own window.",
                          juce::dontSendNotification);
    }

    void rebuild()
    {
        rows.clear();
        for (int i = 0; i < (int) project.modulators.size(); ++i)
            list.addAndMakeVisible (rows.add (new Row (*this, i)));

        updateHint();
        layoutList();
        repaint();
    }

    void layoutList()
    {
        int y = 0;
        const int w = std::max (240, viewport.getWidth() - viewport.getScrollBarThickness() - 4);

        for (auto* r : rows)
        {
            const int h = r->wantedHeight();
            r->setBounds (0, y, w, h);
            y += h + 8;
        }

        list.setSize (w, std::max (y, viewport.getHeight()));
    }

    void timerCallback() override
    {
        // The shape drawings animate only while something is running, so an
        // idle window costs nothing.
        if (! rows.isEmpty() && engine.isPlaying())
            for (auto* r : rows)
                r->repaint();
    }

    Project&     project;
    AudioEngine& engine;

    juce::Label      title, hint;
    juce::TextButton addButton;
    juce::Viewport   viewport;
    juce::Component  list;
    juce::OwnedArray<Row> rows;

    int learning = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModulatorPanel)
};
