#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

//==============================================================================
/** Decides which voice plays which note, and which one gets taken away when
    they have all run out.

    This is bookkeeping rather than audio, and it is kept apart from the audio
    for one reason: voice stealing is where polyphonic synths go wrong, and
    the failures are not ones you can hear your way to. A voice handed out
    twice sounds like a note that will not stop. A voice never handed back is
    a synth that goes quiet after a few bars of playing and comes back after
    a reload. A voice stolen without being told sounds like a click, and then
    like a note cut off. None of that is visible in a waveform, but all of it
    is visible in the bookkeeping, so the bookkeeping lives here where a test
    can reach it with no JUCE and no audio at all.

    The policy is the usual one, in order of preference:

      1. a voice that is not sounding at all
      2. the one that has been releasing longest, since it is already fading
      3. the one that has been held longest, which is the least recent thing
         the player asked for

    Age is counted in notes started rather than in samples, which is enough to
    order them and cannot drift or overflow in any session anyone will play.
*/
template <int NumVoices>
class VoiceAllocator
{
public:
    static constexpr int voiceCount = NumVoices;
    static constexpr int none = -1;

    struct Voice
    {
        int      note     = -1;       // the note it is playing, or -1
        bool     held     = false;    // key still down
        bool     sounding = false;    // still making noise, release included
        uint64_t startedAt = 0;       // which note start this was
    };

    void reset() noexcept
    {
        for (auto& v : voices)
            v = {};

        counter = 0;
    }

    /** Picks a voice for a note and marks it held. Returns the index, and
        sets stolenFrom to the note that was taken away, or -1 if none was.

        The caller is told what it stole so it can fade that voice out rather
        than cut it, which is the difference between a note ending and a
        click. */
    int noteOn (int note, int& stolenFrom) noexcept
    {
        stolenFrom = -1;

        // Retriggering a note that is already held reuses its own voice
        // rather than stacking a second one on top, which is what a keyboard
        // sending a repeated note on without a note off would otherwise do.
        for (int i = 0; i < NumVoices; ++i)
            if (voices[(std::size_t) i].held && voices[(std::size_t) i].note == note)
            {
                voices[(std::size_t) i].startedAt = ++counter;
                return i;
            }

        int chosen = findSilent();

        if (chosen == none)
            chosen = findOldest (true);       // longest in release

        if (chosen == none)
            chosen = findOldest (false);      // longest held

        if (chosen == none)
            return none;                      // only when there are no voices

        auto& v = voices[(std::size_t) chosen];

        if (v.sounding)
            stolenFrom = v.note;

        v.note      = note;
        v.held      = true;
        v.sounding  = true;
        v.startedAt = ++counter;

        return chosen;
    }

    /** Releases every voice holding this note. Returns how many, since a
        note can legitimately be on more than one voice after a steal. */
    int noteOff (int note) noexcept
    {
        int released = 0;

        for (auto& v : voices)
        {
            if (v.held && v.note == note)
            {
                v.held = false;               // still sounding, now releasing
                ++released;
            }
        }

        return released;
    }

    void allNotesOff() noexcept
    {
        for (auto& v : voices)
            v.held = false;
    }

    /** Called when a voice's envelope has finished, which is the only thing
        that puts a voice back in the pool. */
    void voiceFinished (int index) noexcept
    {
        if (index < 0 || index >= NumVoices)
            return;

        auto& v = voices[(std::size_t) index];
        v.note     = -1;
        v.held     = false;
        v.sounding = false;
    }

    const Voice& voice (int index) const noexcept { return voices[(std::size_t) index]; }

    int countSounding() const noexcept
    {
        int n = 0;
        for (const auto& v : voices)
            if (v.sounding)
                ++n;
        return n;
    }

    int countHeld() const noexcept
    {
        int n = 0;
        for (const auto& v : voices)
            if (v.held)
                ++n;
        return n;
    }

private:
    int findSilent() const noexcept
    {
        for (int i = 0; i < NumVoices; ++i)
            if (! voices[(std::size_t) i].sounding)
                return i;

        return none;
    }

    /** Oldest sounding voice, either among those released or among those
        still held. */
    int findOldest (bool releasingOnly) const noexcept
    {
        int best = none;
        uint64_t oldest = 0;

        for (int i = 0; i < NumVoices; ++i)
        {
            const auto& v = voices[(std::size_t) i];

            if (! v.sounding)
                continue;

            if (releasingOnly && v.held)
                continue;

            if (best == none || v.startedAt < oldest)
            {
                best = i;
                oldest = v.startedAt;
            }
        }

        return best;
    }

    std::array<Voice, (std::size_t) NumVoices> voices {};
    uint64_t counter = 0;
};
