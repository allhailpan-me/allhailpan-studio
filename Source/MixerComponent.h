#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "AudioEngine.h"
#include "AhpLookAndFeel.h"

// FL-style mixer: the master on the left, the 16 inserts in a compact grid that
// wraps instead of stretching across the screen, and the effect rack for the
// selected insert on the right.
class MixerComponent : public juce::Component
{
public:
    std::function<void (int insert, int slot)> onSlotClicked;   // add / open / replace / remove
    std::function<void()> onEdited;
    std::function<void()> onInteraction;

    MixerComponent (AudioEngine& e, Project& p) : engine (e), project (p)
    {
        master.reset (new Strip (*this, 0));
        addAndMakeVisible (master.get());

        for (int i = 1; i < kNumInserts; ++i)
            grid.addAndMakeVisible (strips.add (new Strip (*this, i)));

        viewport.setViewedComponent (&grid, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        rackTitle.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        rackTitle.setColour (juce::Label::textColourId, Ahp::bone);
        addAndMakeVisible (rackTitle);

        for (int k = 0; k < kNumFxSlots; ++k)
        {
            auto* row = slots.add (new juce::TextButton());
            row->setWantsKeyboardFocus (false);
            row->setMouseClickGrabsKeyboardFocus (false);
            row->setColour (juce::TextButton::buttonColourId, Ahp::panel3);
            row->onClick = [this, k] { if (onSlotClicked) onSlotClicked (selected, k); };
            addAndMakeVisible (row);

            auto* power = powers.add (new juce::TextButton ("on"));
            power->setWantsKeyboardFocus (false);
            power->setMouseClickGrabsKeyboardFocus (false);
            power->setTooltip ("Bypass this effect");
            power->onClick = [this, k]
            {
                auto& b = engine.insert (selected).bypass[(size_t) k];
                engine.setFxBypass (selected, k, ! b.load());
                refresh();
                if (onEdited) onEdited();
            };
            addAndMakeVisible (power);
        }

        sendsTitle.setText ("sends", juce::dontSendNotification);
        sendsTitle.setColour (juce::Label::textColourId, Ahp::muted);
        sendsTitle.setFont (juce::FontOptions (11.5f));
        addAndMakeVisible (sendsTitle);

        for (int k = 0; k < kNumSends; ++k)
        {
            auto* dest = sendDest.add (new juce::ComboBox());
            dest->setWantsKeyboardFocus (false);
            dest->setTooltip ("Feed a portion of this insert into another one. "
                              "Only later inserts can be chosen, which is what keeps the mixer free of loops.");
            dest->onChange = [this, k] { pushSend (k); };
            addAndMakeVisible (dest);

            auto* amount = sendAmount.add (new juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::NoTextBox));
            amount->setRange (0.0, 1.0, 0.001);
            amount->setWantsKeyboardFocus (false);
            amount->setColour (juce::Slider::trackColourId, Ahp::bone);
            amount->setColour (juce::Slider::backgroundColourId, Ahp::panel3);
            amount->onValueChange = [this, k] { pushSend (k); };
            addAndMakeVisible (amount);
        }

        routing.setColour (juce::Label::textColourId, Ahp::muted);
        routing.setFont (juce::FontOptions (11.5f));
        routing.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (routing);

        setWantsKeyboardFocus (false);
        refresh();
    }

    ~MixerComponent() override { viewport.setViewedComponent (nullptr, false); }

    void selectInsert (int index)
    {
        selected = juce::jlimit (0, kNumInserts - 1, index);
        refresh();
    }

    int getSelectedInsert() const noexcept { return selected; }

    /** Rebuilds every strip's input menu from the device that is open now.
        Called when the device changes, since an interface with eight inputs
        and one with two do not offer the same list. */
    void refreshInputs()
    {
        const auto names = inputChannelNames();

        // A studio with an interface attached and nothing wired to it cannot
        // record, and says so only once somebody presses the button. So the
        // first input gets the first strip, once, which is what somebody with
        // one instrument in one socket wanted anyway.
        //
        // Once per session and only when nothing at all is wired, so that
        // clearing every input stays cleared and a project that wires its own
        // strips is never second guessed.
        if (! names.isEmpty() && ! offeredADefaultInput && engine.countWiredInputs() == 0)
        {
            offeredADefaultInput = true;
            engine.setInsertInput (1, { InputSource::defaultChoice(), true });
            engine.setInsertArmed (1, true);
        }

        for (auto* s : strips)
            s->refreshInputs (names);

        refresh();
    }

    /** The device's own names for its active inputs, falling back to numbers
        when it gives none. */
    juce::StringArray inputChannelNames() const
    {
        auto names = engine.getActiveInputChannelNames();
        const int channels = engine.getInputChannelCount();

        // A device that names nothing still has to be selectable.
        while (names.size() < channels)
            names.add ("In " + juce::String (names.size() + 1));

        names.removeRange (channels, names.size() - channels);
        return names;
    }

    /** Gives every input on the device a strip of its own, from `firstInsert`
        upward, or clears them all. */
    void mapInputsFrom (int firstInsert, bool asPairs, bool clearInstead)
    {
        if (clearInstead)
        {
            for (int i = 1; i < kNumInserts; ++i)
            {
                engine.setInsertInput (i, {});
                engine.setInsertArmed (i, false);
            }
        }
        else
        {
            const auto mapped = InputSource::autoMap (firstInsert, kNumInserts,
                                                      engine.getInputChannelCount(), asPairs);

            for (int i = 1; i < (int) mapped.size() && i < kNumInserts; ++i)
            {
                // Only the strips the map reached. Clearing the rest would
                // throw away an input somebody wired by hand further along.
                if (! mapped[(size_t) i].assigned)
                    continue;

                engine.setInsertInput (i, mapped[(size_t) i]);
                engine.setInsertArmed (i, true);
            }
        }

        refreshInputs();
        refresh();
        edited();
    }

    void refresh()
    {
        auto& ctl = engine.insert (selected);
        rackTitle.setText (ctl.name + "   effects", juce::dontSendNotification);

        for (int k = 0; k < kNumFxSlots; ++k)
        {
            auto* row   = slots[k];
            auto* power = powers[k];
            const bool bypassed = ctl.bypass[(size_t) k].load();

            if (auto* plugin = engine.getFx (selected, k))
            {
                row->setButtonText (plugin->getName());
                row->setColour (juce::TextButton::textColourOffId, bypassed ? Ahp::muted : Ahp::bone);
                power->setVisible (true);
                power->setButtonText (bypassed ? "off" : "on");
                power->setToggleState (! bypassed, juce::dontSendNotification);
            }
            else if (auto it = project.missingFx.find (selected * kNumFxSlots + k); it != project.missingFx.end())
            {
                juce::String name = "plugin";
                if (auto* d = it->second->getFirstChildElement())
                    name = d->getStringAttribute ("name", name);
                row->setButtonText ("missing: " + name);
                row->setColour (juce::TextButton::textColourOffId, Ahp::rec);
                power->setVisible (false);
            }
            else
            {
                row->setButtonText ("+  add effect");
                row->setColour (juce::TextButton::textColourOffId, Ahp::muted);
                power->setVisible (false);
            }
        }

        refreshSends();

        juce::StringArray sources;
        for (int c = 0; c < kNumChannels; ++c)
            if (project.channels[(size_t) c].insert == selected && project.channels[(size_t) c].name.isNotEmpty())
                sources.add ("Channel " + juce::String (c + 1) + ": " + project.channels[(size_t) c].name);
        for (int t = 0; t < (int) project.tracks.size(); ++t)
            if (project.tracks[(size_t) t].insert == selected)
                for (const auto& clip : project.clips)
                    if (clip.isAudio() && clip.track == t)
                    {
                        sources.addIfNotAlreadyThere ("Audio on " + project.tracks[(size_t) t].name);
                        break;
                    }
        if (selected == 0)
            sources.add ("Every insert ends up here.");

        routing.setText (sources.isEmpty() ? juce::String ("Nothing plays through this insert yet.")
                                           : "Playing through here:\n" + sources.joinIntoString ("\n"),
                         juce::dontSendNotification);

        master->refresh();
        for (auto* s : strips)
            s->refresh();
        repaint();
    }

    /** Only inserts after this one can receive a send, so the list offers
        exactly those. */
    void refreshSends()
    {
        auto& ctl = engine.insert (selected);

        for (int k = 0; k < kNumSends; ++k)
        {
            auto* dest = sendDest[k];
            const int current = ctl.sendTo[(size_t) k].load();

            dest->clear (juce::dontSendNotification);
            dest->addItem ("no send", 1);
            for (int to = selected + 1; to < kNumInserts; ++to)
                dest->addItem (engine.insert (to).name, to + 1);

            const bool valid = current > selected && current < kNumInserts;
            dest->setSelectedId (valid ? current + 1 : 1, juce::dontSendNotification);
            dest->setEnabled (selected > 0 && selected + 1 < kNumInserts);

            sendAmount[k]->setValue (ctl.sendLevel[(size_t) k].load(), juce::dontSendNotification);
            sendAmount[k]->setEnabled (valid);
        }

        sendsTitle.setText (selected == 0 ? "sends   (the master has nowhere to send to)"
                                          : "sends",
                            juce::dontSendNotification);
    }

    void pushSend (int k)
    {
        const int id = sendDest[k]->getSelectedId();
        const int to = id > 1 ? id - 1 : 0;

        engine.setSend (selected, k, to, (float) sendAmount[k]->getValue());
        sendAmount[k]->setEnabled (to > selected);

        if (onEdited)
            onEdited();
    }

    void updateMeters()
    {
        master->repaintMeter();
        for (auto* s : strips)
            s->repaintMeter();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);

        auto rack = r.removeFromRight (juce::jlimit (240, 320, r.getWidth() / 4));
        r.removeFromRight (10);
        rackArea = rack;

        rack.reduce (12, 12);
        rackTitle.setBounds (rack.removeFromTop (24));
        rack.removeFromTop (8);
        for (int k = 0; k < kNumFxSlots; ++k)
        {
            auto row = rack.removeFromTop (30);
            powers[k]->setBounds (row.removeFromRight (34).reduced (1, 2));
            row.removeFromRight (4);
            slots[k]->setBounds (row);
            rack.removeFromTop (5);
        }
        rack.removeFromTop (10);
        sendsTitle.setBounds (rack.removeFromTop (18));
        rack.removeFromTop (2);
        for (int k = 0; k < kNumSends; ++k)
        {
            auto row = rack.removeFromTop (26);
            sendDest[k]->setBounds (row.removeFromLeft (118).reduced (0, 2));
            row.removeFromLeft (6);
            sendAmount[k]->setBounds (row);
            rack.removeFromTop (4);
        }

        rack.removeFromTop (8);
        routing.setBounds (rack);

        masterArea = r.removeFromLeft (stripW + 16);
        master->setBounds (masterArea.withTrimmedRight (16));
        r.removeFromLeft (6);

        viewport.setBounds (r);
        const int usable  = r.getWidth() - viewport.getScrollBarThickness() - 4;
        const int columns = std::max (2, (usable + gap) / (stripW + gap));
        const int rows    = ((kNumInserts - 1) + columns - 1) / columns;
        grid.setSize (usable, std::max (r.getHeight(), rows * (stripH + gap) - gap));

        for (int i = 0; i < strips.size(); ++i)
            strips[i]->setBounds ((i % columns) * (stripW + gap), (i / columns) * (stripH + gap), stripW, stripH);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Ahp::panel);
        g.setColour (Ahp::panel2);
        g.fillRoundedRectangle (rackArea.toFloat(), 4.0f);
        g.setColour (Ahp::line);
        g.drawRoundedRectangle (rackArea.toFloat().reduced (0.5f), 4.0f, 1.0f);
        g.fillRect (masterArea.getRight(), masterArea.getY(), 1, masterArea.getHeight());
    }

private:
    class Strip : public juce::Component
    {
    public:
        Strip (MixerComponent& o, int i) : owner (o), index (i)
        {
            auto& ctl = owner.engine.insert (index);

            fader.setSliderStyle (juce::Slider::LinearVertical);
            fader.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
            fader.setRange (0.0, 1.25, 0.001);
            fader.setValue (ctl.volume.load(), juce::dontSendNotification);
            fader.setDoubleClickReturnValue (true, 0.8);
            fader.setColour (juce::Slider::trackColourId, Ahp::panel3);
            fader.setColour (juce::Slider::thumbColourId, Ahp::bone);
            fader.onValueChange = [this]
            {
                owner.engine.insert (index).volume.store ((float) fader.getValue());
                repaint();
                owner.edited();
            };

            pan.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            pan.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
            pan.setRange (-1.0, 1.0, 0.01);
            pan.setValue (ctl.pan.load(), juce::dontSendNotification);
            pan.setDoubleClickReturnValue (true, 0.0);
            pan.setColour (juce::Slider::rotarySliderFillColourId, Ahp::bone);
            pan.setColour (juce::Slider::rotarySliderOutlineColourId, Ahp::panel3);
            pan.onValueChange = [this]
            {
                owner.engine.insert (index).pan.store ((float) pan.getValue());
                owner.edited();
            };

            mute.setClickingTogglesState (false);
            mute.onClick = [this]
            {
                // Through the engine, because what this strip is multiplied by
                // depends on every other strip's solo and on the send matrix.
                owner.engine.setMute (index, ! owner.engine.isMuted (index));
                owner.refresh();
                owner.edited();
            };

            solo.setClickingTogglesState (false);

            if (index > 0)
            {
                solo.setTooltip ("Hear this strip on its own.\n"
                                 "Anything it sends to stays audible, so soloing a vocal keeps its "
                                 "reverb, and soloing a reverb keeps what feeds it.\n"
                                 "Alt click to solo only this one. Alt click a lit solo to clear "
                                 "every solo.\n"
                                 "Right click the strip for Solo isolate, which keeps a strip "
                                 "audible when something else is soloed.");
                solo.onClick = [this]
                {
                    // juce::Button does not filter on which mouse button, and
                    // the strip is a mouse listener on this one, so a right
                    // click would otherwise solo the strip and open the menu at
                    // the same time. The tooltip above sends people here to
                    // right click for Solo isolate, so this is on a path the
                    // feature advertises.
                    const auto mods = juce::ModifierKeys::getCurrentModifiers();

                    if (mods.isPopupMenu())
                        return;

                    // Logic's modifier for both of its solo gestures, and the
                    // preference and the modifier go through one function so
                    // the two cannot drift apart. See SoloGraph::gestureFor.
                    owner.engine.applySoloGesture (index, mods.isAltDown());

                    // Every strip, not this one: a solo here changes what is
                    // audible on all of them, and the whole point of the three
                    // state button is that the other strips can say so.
                    owner.refresh();
                    owner.edited();
                };
            }
            else
            {
                // On the master, the same slot is the indicator that says
                // something is soloed and the button that clears it. Ardour
                // has one, internally called "rude solo", and its tooltip is
                // the whole argument for it: "When active, something is
                // soloed. Click to de-solo everything". Leaving a solo up and
                // then wondering where the mix went is the commonest mistake
                // anybody makes with solo.
                solo.setTooltip ("Lights when something is soloed. Click to clear every solo.");
                solo.onClick = [this]
                {
                    owner.engine.clearAllSolos();
                    owner.refresh();
                    owner.edited();
                };
            }

            // The master has nothing to record: it is where every insert is
            // summed, so a live input there would run through the whole
            // master chain and pick up its lookahead.
            if (index > 0)
            {
                inputBox.setTextWhenNothingSelected ("in");
                inputBox.setTooltip ("Which input on your audio interface feeds this strip.\n"
                                     "One instrument in one socket is a mono input and is heard in "
                                     "the middle. Pairs are for something genuinely stereo.\n"
                                     "Picking an input arms the strip, so pressing record captures it.\n"
                                     "Right click the strip for Map inputs from here, which gives every "
                                     "input on the interface its own strip in one go.");
                inputBox.onChange = [this]
                {
                    const int id = inputBox.getSelectedId();

                    if (id <= 0)
                        return;

                    const auto assignment = InputSource::assignmentFromStored (id - 1);
                    owner.engine.setInsertInput (index, assignment);

                    // Image-Line's manual: "Selecting an input will auto-arm
                    // the track for recording." Wiring a socket to a strip
                    // and then not recording it is almost never what somebody
                    // meant, and the alternative is a take that silently did
                    // not happen.
                    owner.engine.setInsertArmed (index, assignment.assigned);

                    refresh();
                    owner.edited();
                };

                arm.setClickingTogglesState (false);
                arm.setTooltip ("Record this strip's input when the record button is pressed.\n"
                                "Any number of strips can be armed at once, which is how several "
                                "players are recorded at the same time.");
                arm.onClick = [this]
                {
                    owner.engine.setInsertArmed (index, ! owner.engine.isInsertArmed (index));
                    refresh();
                    owner.edited();
                };

                for (auto* c : { static_cast<juce::Component*> (&inputBox),
                                 static_cast<juce::Component*> (&arm) })
                {
                    c->setWantsKeyboardFocus (false);
                    c->setMouseClickGrabsKeyboardFocus (false);
                    c->addMouseListener (this, false);
                    addAndMakeVisible (c);
                }
            }

            for (auto* c : { static_cast<juce::Component*> (&fader), static_cast<juce::Component*> (&pan),
                             static_cast<juce::Component*> (&mute), static_cast<juce::Component*> (&solo) })
            {
                c->setWantsKeyboardFocus (false);
                c->setMouseClickGrabsKeyboardFocus (false);
                c->addMouseListener (this, false);
                addAndMakeVisible (c);
            }
            refresh();
        }

        void refresh()
        {
            auto& ctl = owner.engine.insert (index);
            fader.setValue (ctl.volume.load(), juce::dontSendNotification);
            pan.setValue (ctl.pan.load(), juce::dontSendNotification);

            // A pan control and a balance control do different things to the
            // level, and a strip set the wrong way is silent about it, so the
            // knob says which it is: a brighter ring in pan mode, and the
            // tooltip names the mode and the law it is running under.
            const auto mode = ctl.panMode.load();

            pan.setColour (juce::Slider::rotarySliderOutlineColourId,
                           mode == PanLaw::Mode::pan ? Ahp::muted : Ahp::panel3);

            pan.setTooltip (juce::String (PanLaw::name (mode))
                                + (mode == PanLaw::Mode::pan
                                       ? ", law " + juce::String (PanLaw::name (owner.engine.getPanLaw()))
                                       : juce::String())
                                + ". Right click the strip to change it.");

            mute.setButtonText (ctl.mute.load() ? "muted" : "on");
            mute.setToggleState (! ctl.mute.load(), juce::dontSendNotification);

            // Three states, not two, which is Ardour's RouteUI::solo_active_state
            // and is not cosmetic: soloing a vocal lights nothing on the reverb
            // bus unless the bus can say it is soloed by propagation, and there
            // is then no way to tell why the reverb is still being heard.
            //
            // On the master the same button is the "something is soloed"
            // indicator, so it lights from the whole mixer rather than from
            // this strip.
            if (index > 0)
            {
                const bool selfSoloed = owner.engine.isSoloed (index);
                const bool byOthers   = owner.engine.isSoloedByOthers (index);
                const bool isolated   = owner.engine.isSoloIsolated (index);

                solo.setButtonText (isolated ? "iso" : "solo");
                solo.setToggleState (selfSoloed, juce::dontSendNotification);
                solo.setColour (juce::TextButton::textColourOnId, Ahp::rec);
                solo.setColour (juce::TextButton::textColourOffId,
                                byOthers ? Ahp::rec.withAlpha (0.55f)
                                         : (isolated ? Ahp::bone : Ahp::muted));
            }
            else
            {
                solo.setButtonText ("solo");
                solo.setToggleState (owner.engine.isAnySoloed(), juce::dontSendNotification);
                solo.setColour (juce::TextButton::textColourOnId, Ahp::rec);
                solo.setColour (juce::TextButton::textColourOffId, Ahp::muted.withAlpha (0.5f));
            }

            // Ardour draws the mute button outlined on a strip muted because
            // something else is soloed, with the preference that does it on by
            // default, and Logic flashes it instead. Same purpose either way:
            // a strip that has gone quiet has to be able to say why. The
            // outline is in paint(), which is the only place it can be: this
            // button's toggle state is inverted, true meaning not muted, so
            // setting its off colour from that state lights the wrong one of
            // the two. An audit found exactly that here.

            fxCount = 0;
            for (int k = 0; k < kNumFxSlots; ++k)
                if (owner.engine.getFx (index, k) != nullptr)
                    ++fxCount;

            if (index > 0)
            {
                const auto assignment = owner.engine.getInsertInput (index);
                const int  id = InputSource::assignmentToStored (assignment) + 1;

                if (inputBox.getSelectedId() != id)
                    inputBox.setSelectedId (inputBox.indexOfItemId (id) >= 0 ? id : 1,
                                            juce::dontSendNotification);

                const bool armed = owner.engine.isInsertArmed (index);
                arm.setButtonText (armed ? "rec" : "arm");
                arm.setToggleState (armed, juce::dontSendNotification);
                arm.setColour (juce::TextButton::textColourOffId,
                               assignment.assigned ? Ahp::bone : Ahp::muted);
                arm.setColour (juce::TextButton::textColourOnId, Ahp::rec);
            }

            repaint();
        }

        /** Rebuilds the input menu from the device that is open now.

            Laid out the way Image-Line document FL Studio's, "an upper stereo
            list and lower mono list", because a musician should not have to
            learn a second set of rules for the same job. Nothing comes first,
            since most strips carry an instrument rather than a socket. */
        void refreshInputs (const juce::StringArray& channelNames)
        {
            if (index == 0)
                return;

            const int channels = channelNames.size();
            const auto wanted = owner.engine.getInsertInput (index);

            inputBox.clear (juce::dontSendNotification);
            inputBox.addItem ("No input", 1);

            const auto label = [&channelNames] (const InputSource::Choice& choice, int channels_)
            {
                const auto resolved = InputSource::resolve (choice, channels_);

                if (! resolved.valid)
                    return juce::String (InputSource::describe (choice));

                // The device's own names where it gives them, because "In 1"
                // is the first input the device hands over, which is the first
                // socket only while every channel is enabled.
                auto name = channelNames[resolved.left];

                if (! choice.stereo)
                    return name;

                return name + " + " + channelNames[resolved.right];
            };

            const auto all = InputSource::options (channels);

            if (channels >= 2)
            {
                inputBox.addSectionHeading ("Stereo");

                for (const auto& choice : all)
                    if (choice.stereo)
                        inputBox.addItem (label (choice, channels),
                                          InputSource::assignmentToStored ({ choice, true }) + 1);
            }

            if (channels >= 1)
            {
                inputBox.addSectionHeading ("Mono");

                for (const auto& choice : all)
                    if (! choice.stereo)
                        inputBox.addItem (label (choice, channels),
                                          InputSource::assignmentToStored ({ choice, true }) + 1);
            }

            // What was wired stays wired when the device still has it.
            const int wantedId = InputSource::assignmentToStored (wanted) + 1;
            const bool stillThere = inputBox.indexOfItemId (wantedId) >= 0;

            inputBox.setSelectedId (stillThere ? wantedId : 1, juce::dontSendNotification);

            // And when it does not, the engine is told as well, not just the
            // menu. Showing "No input" while the engine kept the old
            // assignment meant that swapping an eight input interface for a
            // two input one left a strip reading "No input", still armed, and
            // still recording: InputSource::resolve clamps, so it captured
            // input 2 under the name of insert 5.
            if (! stillThere && wanted.assigned)
            {
                owner.engine.setInsertInput (index, {});
                owner.engine.setInsertArmed (index, false);
            }
        }

        void repaintMeter() { repaint (meterArea.expanded (0, 1)); }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (owner.onInteraction)
                owner.onInteraction();
            owner.selectInsert (index);

            if (e.mods.isPopupMenu())
            {
                showMenu();
                return;
            }
            if (e.eventComponent == this && e.getNumberOfClicks() == 2 && e.y < 22)
                rename();
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (5, 6);
            r.removeFromTop (20);                                  // name
            r.removeFromTop (12);                                  // fx count

            if (index > 0)
            {
                inputBox.setBounds (r.removeFromTop (19));
                r.removeFromTop (2);
                arm.setBounds (r.removeFromTop (16));
                r.removeFromTop (3);
            }

            pan.setBounds (r.removeFromTop (30).withSizeKeepingCentre (30, 30));
            r.removeFromTop (4);
            // Stacked rather than side by side, and the strip is taller by
            // the difference. A strip is 66 pixels wide and 56 after its
            // margins, so splitting that row in two leaves 27 pixels a button,
            // and JUCE's own button text indent is half the corner size at
            // each end: 13 pixels of text for a 12 pixel font, which is a
            // smear. An audit measured it. Full width each, one above the
            // other, keeps both words legible.
            solo.setBounds (r.removeFromBottom (18));
            r.removeFromBottom (2);
            mute.setBounds (r.removeFromBottom (18));
            r.removeFromBottom (14);                               // dB readout
            meterArea = r.removeFromRight (12).reduced (1, 2);
            r.removeFromRight (2);
            fader.setBounds (r);
        }

        void paint (juce::Graphics& g) override
        {
            const bool isSelected = owner.selected == index;
            const bool isMaster   = index == 0;
            auto& ctl = owner.engine.insert (index);
            auto r = getLocalBounds().toFloat();

            g.setColour (isMaster ? Ahp::panel3 : Ahp::panel2);
            g.fillRoundedRectangle (r, 4.0f);
            if (isSelected)
            {
                g.setColour (Ahp::bone.withAlpha (0.08f));
                g.fillRoundedRectangle (r, 4.0f);
            }
            g.setColour (isSelected ? Ahp::bone : Ahp::line);
            g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, isSelected ? 1.6f : 1.0f);

            g.setColour (ctl.mute.load() ? Ahp::muted : Ahp::bone);
            g.setFont (juce::FontOptions (11.5f, isMaster ? juce::Font::bold : juce::Font::plain));
            g.drawFittedText (ctl.name, getLocalBounds().reduced (4, 5).removeFromTop (18), juce::Justification::centred, 1);

            g.setColour (fxCount > 0 ? Ahp::bone.withAlpha (0.65f) : Ahp::muted.withAlpha (0.5f));
            g.setFont (juce::FontOptions (9.5f));
            g.drawText (fxCount > 0 ? juce::String (fxCount) + (fxCount == 1 ? " effect" : " effects") : juce::String ("no effects"),
                        getLocalBounds().reduced (4).withTop (23).withHeight (12), juce::Justification::centred);

            const float v  = ctl.volume.load();
            const float db = juce::Decibels::gainToDecibels ((v / 0.8f) * (v / 0.8f), -100.0f);
            g.setColour (Ahp::muted);
            g.setFont (juce::FontOptions (10.0f));
            g.drawText (db <= -99.0f ? juce::String ("-inf") : juce::String (db, 1),
                        getLocalBounds().withTrimmedBottom (26).removeFromBottom (14), juce::Justification::centred);

            // The outline Ardour draws on a strip that is quiet only because
            // something else is soloed, round the mute button, which is where
            // Ardour and Logic both put it.
            // An explicit mute wins the display, the way Ardour's
            // update_mute_display lets it: mutedByOthersSoloing is true on a
            // hand muted strip too, because it is about what the solo system
            // is doing and takes no account of mute, so without this a strip
            // the user muted would also be outlined and the outline would stop
            // meaning anything.
            if (owner.engine.isMutedByOthersSoloing (index) && ! owner.engine.isMuted (index))
            {
                g.setColour (Ahp::rec.withAlpha (0.55f));
                g.drawRoundedRectangle (mute.getBounds().toFloat().reduced (0.5f), 3.0f, 1.0f);
            }

            g.setColour (Ahp::black);
            g.fillRect (meterArea);
            auto bar = [&] (juce::Rectangle<int> m, float level)
            {
                const float norm = juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (level, -60.0f) + 60.0f) / 66.0f);
                g.setColour (level >= 1.0f ? Ahp::rec : Ahp::bone.withAlpha (0.85f));
                g.fillRect (m.withTop (m.getBottom() - (int) (m.getHeight() * norm)));
            };
            bar (meterArea.withWidth (meterArea.getWidth() / 2).reduced (1, 0), ctl.peakL.load());
            bar (meterArea.withLeft (meterArea.getCentreX()).reduced (1, 0), ctl.peakR.load());
        }

    private:
        void rename()
        {
            auto* w = new juce::AlertWindow ("Rename " + owner.engine.insert (index).name, {}, juce::MessageBoxIconType::NoIcon);
            w->addTextEditor ("name", owner.engine.insert (index).name);
            w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

            juce::Component::SafePointer<Strip> safe (this);
            w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w] (int result)
            {
                if (result != 1 || safe == nullptr)
                    return;
                const auto name = w->getTextEditorContents ("name").trim().substring (0, 18);
                if (name.isNotEmpty())
                {
                    safe->owner.engine.insert (safe->index).name = name;
                    safe->owner.refresh();
                    safe->owner.edited();
                }
            }), true);
        }

        void showMenu()
        {
            juce::PopupMenu m;
            m.addSectionHeader (owner.engine.insert (index).name);
            m.addItem (1, "Rename...");
            // Named for everything it does. It used to say "Reset volume and
            // pan" while also clearing mute, and it now clears solo and solo
            // isolate as well. Solo isolate is the one setting here whose loss
            // is silent and deferred: reset a reverb return's fader, and the
            // next time somebody solos a vocal the return is cut and the vocal
            // is dry, with nothing anywhere to say the solo isolate went.
            m.addItem (2, "Reset this strip");
            m.addItem (3, "Remove all effects", fxCount > 0);

            // Ardour, Cubase and Reaper all let the panner be chosen rather
            // than inferred from the audio, and all three put the choice on
            // the pan control's own context menu.
            //
            // Inferring it would be wrong for most of what reaches an insert.
            // A strip fed from a socket does know, because `InputSource.h`
            // knows whether that choice was mono, and Logic and Cubase would
            // use it. But an insert also carries playlist audio, an instrument
            // channel's output and the sum of several of those, and for all of
            // those the buffer is two channels whatever is on it, so the
            // studio cannot tell a centred mono take from a stereo recording
            // that happens to be quiet on one side. A control that changed the
            // level depending on the material would be worse than one that
            // needs telling.
            //
            // Not offered on insert 0. The master is what every other insert
            // is summed into after each has been lined up, so there is no
            // source on it to place: under an uncompensated law it would drop
            // the whole mix by the law's own figure, and a compensated one
            // would raise the ends of a bus that has no headroom to give.
            // Insert 0 is carved out of input routing for the same shape of
            // reason.
            const auto mode = owner.engine.insert (index).panMode.load();

            if (index > 0)
            {
                m.addSeparator();

                // Ardour calls it Solo Isolate, Pro Tools calls it Solo Safe
                // and Logic calls it solo-safe, and all three manuals describe
                // the same behaviour. Avid's own caution is worth passing on
                // in the tooltip: "Don't apply Solo Safe to every track, since
                // that defeats the purpose of soloing."
                m.addItem (4, "Solo isolate", true, owner.engine.isSoloIsolated (index));

                m.addSeparator();
                m.addSectionHeader ("Pan control");
                m.addItem (20, PanLaw::name (PanLaw::Mode::balance), true, mode == PanLaw::Mode::balance);
                m.addItem (21, PanLaw::name (PanLaw::Mode::pan),     true, mode == PanLaw::Mode::pan);
            }

            // One law for the project, which is where Logic and Reaper keep
            // it. Said so on the item rather than left to be discovered after
            // somebody has set it on eight strips in turn.
            juce::PopupMenu laws;
            const auto current = owner.engine.getPanLaw();

            if (index == 0)
                m.addSeparator();

            for (int i = 0; i < PanLaw::numLaws; ++i)
            {
                const auto law = PanLaw::lawAt (i);
                laws.addItem (30 + i, PanLaw::name (law), true, law == current);
            }

            m.addSubMenu ("Pan law, for the whole project", laws);

            // Image-Line's manual calls this Auto-map: "This will
            // automatically map each input on your audio device to a unique
            // Mixer Track Input, starting on the Mixer track where the
            // Auto-map was initiated and working to the right." It is the
            // difference between wiring an eight input interface in one
            // action and in eight.
            const int channels = owner.engine.getInputChannelCount();

            if (index > 0 && channels > 0)
            {
                m.addSeparator();
                m.addItem (10, "Map inputs from here");
                m.addItem (11, "Map inputs from here, in pairs", channels >= 2);
                m.addItem (12, "Clear every input");
            }

            juce::Component::SafePointer<Strip> safe (this);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [safe] (int result)
            {
                if (safe == nullptr || result == 0)
                    return;
                auto& ctl = safe->owner.engine.insert (safe->index);

                if (result == 1) { safe->rename(); return; }

                if (result == 4 && safe->index > 0)
                {
                    safe->owner.engine.setSoloIsolate (safe->index,
                                                       ! safe->owner.engine.isSoloIsolated (safe->index));
                    safe->owner.refresh();
                    safe->owner.edited();
                    return;
                }

                if (result == 10 || result == 11 || result == 12)
                {
                    safe->owner.mapInputsFrom (safe->index, result == 11, result == 12);
                    return;
                }

                if ((result == 20 || result == 21) && safe->index > 0)
                {
                    ctl.panMode.store (result == 21 ? PanLaw::Mode::pan : PanLaw::Mode::balance);
                }
                else if (result >= 30 && result < 30 + PanLaw::numLaws)
                {
                    // Every strip's tooltip names the law, and a law reaches
                    // every strip set to pan, so the whole mixer is refreshed
                    // rather than this one.
                    safe->owner.engine.setPanLaw (PanLaw::lawAt (result - 30));
                }

                if (result == 2)
                {
                    ctl.volume.store (0.8f);
                    ctl.pan.store (0.0f);

                    // Through the engine, and taking the solo state with it: a
                    // strip reset to its defaults and left solo isolated is a
                    // strip that behaves differently from every other one with
                    // nothing on it to say so.
                    safe->owner.engine.setMute (safe->index, false);

                    if (safe->index > 0)
                    {
                        safe->owner.engine.setSolo (safe->index, false);
                        safe->owner.engine.setSoloIsolate (safe->index, false);
                    }
                }
                else if (result == 3)
                {
                    for (int k = 0; k < kNumFxSlots; ++k)
                        if (safe->owner.engine.getFx (safe->index, k) != nullptr)
                            safe->owner.removeFx (safe->index, k);
                }
                safe->owner.refresh();
                safe->owner.edited();
            });
        }

        MixerComponent& owner;
        const int index;
        int fxCount = 0;
        juce::Slider fader, pan;
        juce::TextButton mute, solo;
        juce::ComboBox   inputBox;
        juce::TextButton arm { "arm" };
        juce::Rectangle<int> meterArea;
    };

    void edited() { if (onEdited) onEdited(); }

    // Set by the main window so the strip menu can clear a rack
    friend class MainComponent;
    std::function<void (int insert, int slot)> removeFxCallback;
    void removeFx (int insert, int slot) { if (removeFxCallback) removeFxCallback (insert, slot); }

    // Taller than it was by the input menu and the arm button together.
    bool offeredADefaultInput = false;

    // Taller than it was by the input menu and the arm button together.
    // Taller than it was by the height of the solo button and its gap, so
    // that adding solo did not come out of the fader's travel.
    static constexpr int stripW = 66, stripH = 250, gap = 6;

    AudioEngine& engine;
    Project&     project;
    int selected = 0;

    std::unique_ptr<Strip>  master;
    juce::Viewport          viewport;
    juce::Component         grid;
    juce::OwnedArray<Strip> strips;
    juce::Rectangle<int>    rackArea, masterArea;

    juce::Label rackTitle, routing, sendsTitle;
    juce::OwnedArray<juce::TextButton> slots, powers;
    juce::OwnedArray<juce::ComboBox>   sendDest;
    juce::OwnedArray<juce::Slider>     sendAmount;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerComponent)
};

// The mixer lives in its own window, so it can sit over the playlist.
class MixerWindow : public juce::DocumentWindow
{
public:
    MixerWindow (MixerComponent& mixer, std::function<void()> onClosed)
        : DocumentWindow ("Mixer  -  ALLHAILPAN Studio", Ahp::black, DocumentWindow::closeButton | DocumentWindow::maximiseButton),
          closed (std::move (onClosed))
    {
        setUsingNativeTitleBar (true);
        setContentNonOwned (&mixer, false);
        setResizable (true, false);
        setResizeLimits (700, 420, 4000, 2400);
        centreWithSize (1080, 560);
        setVisible (true);
    }

    void closeButtonPressed() override { if (closed) closed(); }

private:
    std::function<void()> closed;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerWindow)
};
