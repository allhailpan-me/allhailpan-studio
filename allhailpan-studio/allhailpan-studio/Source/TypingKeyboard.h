#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>

// Turns the computer keyboard into a two-octave piano, laid out like FL Studio:
//
//   upper octave:   2 3   5 6 7   9 0          (black keys)
//                  Q W E R T Y U I O P         (white keys)
//   lower octave:   S D   G H J   L ;          (black keys)
//                  Z X C V B N M , . /         (white keys)
//
// Up and Down arrows change the octave.
class TypingKeyboard : public juce::KeyListener
{
public:
    explicit TypingKeyboard (juce::MidiKeyboardState& s) : state (s) {}

    std::function<void()> onOctaveChanged;

    int  getBaseNote() const noexcept   { return 12 * (octave + 1); }   // C of the lower row
    int  getOctave() const noexcept     { return octave; }
    void setVelocity (float v) noexcept { velocity = juce::jlimit (0.05f, 1.0f, v); }

    void allNotesOff()
    {
        for (size_t i = 0; i < keys.size(); ++i)
            if (held[i] >= 0)
            {
                state.noteOff (1, held[i], 0.0f);
                held[i] = -1;
            }
    }

    bool keyPressed (const juce::KeyPress& key, juce::Component*) override
    {
        const auto mods = key.getModifiers();
        if (mods.isCommandDown() || mods.isAltDown())
            return false;

        const auto code = key.getKeyCode();
        if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
        {
            allNotesOff();
            octave = juce::jlimit (0, 8, octave + (code == juce::KeyPress::upKey ? 1 : -1));
            if (onOctaveChanged)
                onOctaveChanged();
            return true;
        }

        // Swallow mapped keys (including auto-repeat); note on/off happens in keyStateChanged
        return indexOf (code) >= 0;
    }

    bool keyStateChanged (bool, juce::Component*) override
    {
        bool used = false;
        for (size_t i = 0; i < keys.size(); ++i)
        {
            const bool down = juce::KeyPress::isKeyCurrentlyDown (keys[i].key);

            if (down && held[i] < 0)
            {
                const int note = juce::jlimit (0, 127, getBaseNote() + keys[i].offset);
                held[i] = note;
                state.noteOn (1, note, velocity);
                used = true;
            }
            else if (! down && held[i] >= 0)
            {
                state.noteOff (1, held[i], 0.0f);
                held[i] = -1;
                used = true;
            }
        }
        return used;
    }

private:
    struct Key { int key; int offset; };

    static constexpr size_t numKeys = 34;
    static inline const std::array<Key, numKeys> keys {{
        // lower octave
        { 'z', 0 }, { 's', 1 }, { 'x', 2 }, { 'd', 3 }, { 'c', 4 }, { 'v', 5 }, { 'g', 6 },
        { 'b', 7 }, { 'h', 8 }, { 'n', 9 }, { 'j', 10 }, { 'm', 11 }, { ',', 12 }, { 'l', 13 },
        { '.', 14 }, { ';', 15 }, { '/', 16 },
        // upper octave
        { 'q', 12 }, { '2', 13 }, { 'w', 14 }, { '3', 15 }, { 'e', 16 }, { 'r', 17 }, { '5', 18 },
        { 't', 19 }, { '6', 20 }, { 'y', 21 }, { '7', 22 }, { 'u', 23 }, { 'i', 24 }, { '9', 25 },
        { 'o', 26 }, { '0', 27 }, { 'p', 28 }
    }};

    int indexOf (int code) const
    {
        const auto lower = (int) juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) code);
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i].key == lower)
                return (int) i;
        return -1;
    }

    juce::MidiKeyboardState& state;
    int   octave   = 4;       // lower row starts at C4 (MIDI 60)
    float velocity = 0.8f;
    std::array<int, numKeys> held = [] { std::array<int, numKeys> a; a.fill (-1); return a; }();
};
