#pragma once
#include "WaveBank.h"
#include "LowPassGate.h"
#include "Slope.h"
#include "ModMatrix.h"

#include "../Envelope.h"
#include "../Fx/WaveFolder.h"

#include <cmath>
#include <cstdint>

//==============================================================================
/** One voice of the instrument, with everything wired through the matrix.

    Two wavetable oscillators, a sub, a noise source, a wavefolder and a low
    pass gate, with four envelopes and two cycling slopes driving any of it
    from anywhere. The parts are all built and tested separately; this is
    where they meet, and the only thing it adds of its own is the wiring.

    **Nothing in here is soldered.** The old voice had an amplitude envelope
    on the amplifier and a filter envelope on the filter, because somebody
    decided that is what they were for. Here both are just sources, and what
    they reach is whatever the patch says. A patch can put envelope one on the
    gate, envelope two on the fold, a slope on the wavetable position and
    nothing at all on the amplifier, which is a perfectly ordinary west coast
    patch and was not expressible before.

    The one exception is deliberate and is noted at `amplitudeFor`: something
    has to decide when the voice has finished, or a note never frees its
    voice and sixteen notes exhaust the instrument.

    **The signal path**, which is fixed even though the modulation is not:

        osc 1 ─┐
        osc 2 ─┼─> fold ──> low pass gate ──> out
        sub  ──┤
        noise ─┘

    Fixed because a folder after a gate is a folder being fed something that
    has already been made quiet, so it stops folding as the note decays and
    the patch loses its character exactly where it was interesting. Every
    hardware voice of this kind puts the shaping before the gate for the same
    reason.

    **Oscillator two can modulate oscillator one's phase.** Not its frequency:
    the offset is added before the table is read and is not accumulated, so
    oscillator one stays in tune however hard it is driven. That is the whole
    difference between an FM oscillator that is playable and one that wanders
    off key as the index rises.

    No JUCE, so the whole signal path of the instrument can be run and
    measured by a test rather than only heard.
*/
namespace Synth
{

class Voice
{
public:
    //==========================================================================
    struct SlopeSettings
    {
        double riseSeconds = 0.01;
        double fallSeconds = 0.30;
        double shape = 0.0;
        bool   cycling = false;
    };

    /** Everything one patch decides. No JUCE and no pointers, so it can be
        copied, compared, saved and handed between threads. */
    struct Patch
    {
        // Oscillators. The tables are indices into the bank; a patch stores
        // the bank's string id and resolves it on load, the same discipline
        // the matrix uses, so reordering the bank cannot repoint a patch.
        int    osc1Table = 0;
        int    osc2Table = 0;

        double osc1Position = 0.0;      /**< along the row of waveforms, 0 to 1 */
        double osc2Position = 0.0;

        double osc1Level = 0.5;
        double osc2Level = 0.0;
        double subLevel = 0.0;
        double noiseLevel = 0.0;

        double osc2Semitones = 0.0;     /**< against osc 1, for detune or an interval */
        double fmAmount = 0.0;          /**< osc 2 into osc 1's phase, in cycles */

        // Folder.
        double foldDrive = 1.0;
        double foldSymmetry = 0.0;

        // Gate.
        double gateRange = 7.0;         /**< octaves it opens */
        double gateResting = 60.0;      /**< hertz with it shut */
        double gateLag = 1.0;           /**< how much of the vactrol to use */
        LowPassGate::Mode gateMode = LowPassGate::Mode::both;

        Envelope::Settings envelopes[4];
        SlopeSettings slopes[2];

        ModMatrix::Patch matrix;
    };

    //==========================================================================
    void setSampleRate (double newRate)
    {
        sampleRate = newRate > 0.0 ? newRate : 44100.0;

        osc1.setSampleRate (sampleRate);
        osc2.setSampleRate (sampleRate);
        sub.setSampleRate (sampleRate);

        for (auto& e : envelopes)
            e.setSampleRate (sampleRate);

        for (auto& s : slopes)
            s.setSampleRate (sampleRate);

        gate.setSampleRate (sampleRate);
        folder.prepare (sampleRate, 1);
    }

    /** The bank is shared and const: every voice of every note reads the same
        mipmaps, which is the reason building them is affordable at all. */
    void setBank (const WaveBank* newBank) noexcept
    {
        bank = newBank;
        applyTables();
    }

    void setPatch (const Patch& p)
    {
        patch = p;
        applyTables();

        for (int i = 0; i < 4; ++i)
            envelopes[(std::size_t) i].setSettings (patch.envelopes[i]);

        for (int i = 0; i < 2; ++i)
        {
            slopes[(std::size_t) i].setTimes (patch.slopes[i].riseSeconds,
                                              patch.slopes[i].fallSeconds);
            slopes[(std::size_t) i].setShape (patch.slopes[i].shape);
            slopes[(std::size_t) i].setCycling (patch.slopes[i].cycling);
        }

        gate.setMode (patch.gateMode);
        gate.setRestingCutoff (patch.gateResting);
        gate.setLag (patch.gateLag);

        retune();
    }

    const Patch& getPatch() const noexcept { return patch; }

    //==========================================================================
    void start (int midiNote, float velocity) noexcept
    {
        note = midiNote;
        this->velocity = velocity < 0.0f ? 0.0f : (velocity > 1.0f ? 1.0f : velocity);

        held = true;
        retune();

        for (auto& e : envelopes)
            e.noteOn();

        for (auto& s : slopes)
            s.trigger();

        // One random value per note, held for the life of it. A random source
        // that moved during a note would be noise rather than a modulation
        // source: the point of it is that every note is a little different
        // from the last and consistent with itself.
        randomState = randomState * 6364136223846793005ull + 1442695040888963407ull;
        perNote = (double) ((randomState >> 33) & 0x7fffff) / (double) 0x7fffff * 2.0 - 1.0;
    }

    void stop() noexcept
    {
        held = false;

        for (auto& e : envelopes)
            e.noteOff();
    }

    /** Silences the voice at once, for when there is nothing left to steal
        and a click is better than a wrong note. */
    void kill() noexcept
    {
        for (auto& e : envelopes)
            e.reset();

        for (auto& s : slopes)
            s.reset();

        gate.reset();
        folder.reset();
        note = -1;
        held = false;
    }

    bool isActive() const noexcept { return envelopes[0].isActive(); }
    int  getNote() const noexcept  { return note; }

    //==========================================================================
    float nextSample() noexcept
    {
        if (! envelopes[0].isActive())
            return 0.0f;

        gatherSources();

        ModMatrix::evaluate (patch.matrix, sourceValues, usableSources(),
                             destinationSums, usableDestinations());

        // Pitch first, because everything downstream reads the frequency.
        //
        // Every one of these carries the range its parameter actually has.
        // The clamp in `modulated` defaults to nothing-to-one, which is right
        // for a level and wrong for everything that is not one, and getting
        // that wrong does not fail: it leaves the routing wired, the matrix
        // evaluated, the sum computed, and the result flattened to the same
        // number it started at. A routing to the fold read as doing nothing
        // at all, with four decimal places of agreement, which is what sent
        // somebody looking at the matrix rather than at this line.
        const double bend1 = modulated (destination.osc1Pitch, 0.0, -48.0, 48.0);
        const double bend2 = modulated (destination.osc2Pitch, 0.0, -48.0, 48.0);

        osc1.setFrequency (frequencyOf ((double) note + bend1));
        osc2.setFrequency (frequencyOf ((double) note + patch.osc2Semitones + bend2));
        sub.setFrequency (frequencyOf ((double) note + bend1) * 0.5);

        osc1.setPosition (modulated (destination.osc1Position, patch.osc1Position));
        osc2.setPosition (modulated (destination.osc2Position, patch.osc2Position));

        const double second = osc2.nextSample();

        // Phase modulation, not frequency modulation. See the class comment.
        const double index = modulated (destination.fm, patch.fmAmount, 0.0, 8.0);
        const double first = osc1.nextSample (second * index);

        double mixed = first * modulated (destination.osc1Level, patch.osc1Level)
                     + second * modulated (destination.osc2Level, patch.osc2Level)
                     + sub.nextSample() * modulated (destination.subLevel, patch.subLevel)
                     + nextNoise() * modulated (destination.noiseLevel, patch.noiseLevel);

        folder.setDrive (modulated (destination.fold, patch.foldDrive, 0.0, 64.0));
        folder.setSymmetry (modulated (destination.foldSymmetry, patch.foldSymmetry, -1.0));

        mixed = folder.processOneAtCurrentRate (mixed);

        gate.setRange (modulated (destination.gateRange, patch.gateRange, 0.0, 10.0));

        // What drives the gate. Nothing is soldered here either: if a patch
        // points something at it, that is what opens it, and if nothing does
        // then the amplitude envelope does, because a voice whose gate
        // nothing opens makes no sound and that is a confusing thing to hand
        // somebody who has not wired anything up yet.
        const double opening = patch.matrix.anyRouteTo (destination.gateOpen)
                             ? modulated (destination.gateOpen, 0.0)
                             : (double) envelopes[0].getLevel();

        mixed = gate.process (mixed, opening);

        return (float) (mixed * amplitudeFor());
    }

private:
    //==========================================================================
    /** The amplitude, which is the one thing that is not entirely up to the
        patch.

        A voice has to stop. Something has to tell the allocator the note has
        finished, and the only thing that knows is the envelope that was
        released when the key came up. So envelope one always scales the
        output, and a patch that wants the gate to do all the shaping leaves
        envelope one wide open rather than disconnecting it.

        That is a real constraint and it is written down rather than hidden:
        without it a patch could route nothing to the amplitude, the voice
        would never report itself finished, and sixteen notes would exhaust
        the instrument with nothing audible to show for it.
    */
    double amplitudeFor() noexcept
    {
        const double fromMatrix = modulated (destination.amp, 1.0);

        return (double) envelopes[0].getLevel() * velocity * fromMatrix;
    }

    void gatherSources() noexcept
    {
        for (int i = 0; i < usableSources(); ++i)
            sourceValues[i] = 0.0f;

        for (int i = 0; i < 4; ++i)
            sourceValues[source.envelope[i]] = envelopes[(std::size_t) i].nextSample();

        for (int i = 0; i < 2; ++i)
            sourceValues[source.slope[i]] = (float) slopes[(std::size_t) i].nextSample();

        sourceValues[source.velocity] = (float) velocity;

        // Zero at middle C and plus or minus one at the ends of the keyboard,
        // so a patch that tracks the key does the same thing on every
        // instrument rather than depending on where the notes happen to sit.
        sourceValues[source.keyTrack] = (float) ((note - 60) / 48.0);

        sourceValues[source.random] = (float) perNote;
        sourceValues[source.gate] = held ? 1.0f : 0.0f;
    }

    /** The patched value of a destination, with the matrix's sum applied. */
    double modulated (int which, double base, double lowest = 0.0, double highest = 1.0) noexcept
    {
        if (which < 0 || which >= tableRoom)
            return base;

        return (double) ModMatrix::apply (which, (float) base, destinationSums[which],
                                          (float) lowest, (float) highest);
    }

    /** Pink enough to be useful rather than white and brittle.

        One pole of lowpass on a white source, which is 3 dB per octave rather
        than the 3 dB per octave pink is, and is the difference between noise
        that sits under a sound and noise that sits on top of it. It is not
        pink and does not claim to be.
    */
    double nextNoise() noexcept
    {
        noiseState = noiseState * 6364136223846793005ull + 1442695040888963407ull;

        const double white = (double) ((noiseState >> 33) & 0xffffff)
                           / (double) 0x7fffff - 1.0;

        noiseFilter += 0.12 * (white - noiseFilter);

        return noiseFilter * 2.0;
    }

    void applyTables() noexcept
    {
        if (bank == nullptr)
            return;

        osc1.setTable (bank->table (patch.osc1Table));
        osc2.setTable (bank->table (patch.osc2Table));
        sub.setTable (bank->table (patch.osc1Table));
    }

    void retune() noexcept
    {
        if (note < 0)
            return;

        osc1.setFrequency (frequencyOf ((double) note));
        osc2.setFrequency (frequencyOf ((double) note + patch.osc2Semitones));
        sub.setFrequency (frequencyOf ((double) note) * 0.5);
    }

    static double frequencyOf (double midiNote) noexcept
    {
        return 440.0 * std::pow (2.0, (midiNote - 69.0) / 12.0);
    }

    /** The source and destination indices, looked up once by their stable ids
        rather than per sample, and never by a number written here. */
    struct SourceIndices
    {
        SourceIndices()
        {
            const char* envIds[4] = { "env1", "env2", "env3", "env4" };
            const char* slopeIds[2] = { "slope1", "slope2" };

            for (int i = 0; i < 4; ++i)
                envelope[i] = ModMatrix::sourceFromId (envIds[i]);

            for (int i = 0; i < 2; ++i)
                slope[i] = ModMatrix::sourceFromId (slopeIds[i]);

            velocity = ModMatrix::sourceFromId ("velocity");
            keyTrack = ModMatrix::sourceFromId ("keytrack");
            random   = ModMatrix::sourceFromId ("random");
            gate     = ModMatrix::sourceFromId ("gate");
        }

        int envelope[4] {}, slope[2] {};
        int velocity = -1, keyTrack = -1, random = -1, gate = -1;
    };

    struct DestinationIndices
    {
        DestinationIndices()
        {
            osc1Pitch    = ModMatrix::destinationFromId ("osc1pitch");
            osc2Pitch    = ModMatrix::destinationFromId ("osc2pitch");
            osc1Position = ModMatrix::destinationFromId ("osc1pos");
            osc2Position = ModMatrix::destinationFromId ("osc2pos");
            osc1Level    = ModMatrix::destinationFromId ("osc1level");
            osc2Level    = ModMatrix::destinationFromId ("osc2level");
            subLevel     = ModMatrix::destinationFromId ("sublevel");
            noiseLevel   = ModMatrix::destinationFromId ("noiselevel");
            fm           = ModMatrix::destinationFromId ("fm");
            fold         = ModMatrix::destinationFromId ("fold");
            foldSymmetry = ModMatrix::destinationFromId ("foldsymmetry");
            gateOpen     = ModMatrix::destinationFromId ("gateopen");
            gateRange    = ModMatrix::destinationFromId ("gaterange");
            amp          = ModMatrix::destinationFromId ("amp");
        }

        int osc1Pitch = -1, osc2Pitch = -1;
        int osc1Position = -1, osc2Position = -1;
        int osc1Level = -1, osc2Level = -1, subLevel = -1, noiseLevel = -1;
        int fm = -1, fold = -1, foldSymmetry = -1;
        int gateOpen = -1, gateRange = -1, amp = -1;
    };

    static const SourceIndices& sourceIndices()
    {
        static const SourceIndices all;
        return all;
    }

    static const DestinationIndices& destinationIndices()
    {
        static const DestinationIndices all;
        return all;
    }

    const SourceIndices& source = sourceIndices();
    const DestinationIndices& destination = destinationIndices();

    //==========================================================================
    const WaveBank* bank = nullptr;
    Patch patch;

    WaveTableOscillator osc1, osc2, sub;
    Fx::WaveFolder folder;
    LowPassGate gate;
    Envelope envelopes[4];
    Slope slopes[2];

    double sampleRate = 44100.0;
    double velocity = 1.0;
    double perNote = 0.0;
    double noiseFilter = 0.0;

    std::uint64_t randomState = 0x9e3779b97f4a7c15ull;
    std::uint64_t noiseState = 0x2545f4914f6cdd1dull;

    int  note = -1;
    bool held = false;

    // Sized generously and checked rather than sized exactly, because these
    // are indexed by whatever the tables hold and growing a table without
    // growing these would write past the end of a member. The check is at
    // compile time so adding a source can never be the thing that does it.
    static constexpr int tableRoom = 64;

    /** The tables, held to what will fit. If a later version adds sources
        past the room here they are simply not reached, which costs a routing
        and is survivable; writing past the end of a member is not.
        `VoiceTest` asserts there is room for every entry, so this is the belt
        rather than the braces. */
    static int usableSources() noexcept
    {
        const int n = ModMatrix::numSources();
        return n < tableRoom ? n : tableRoom;
    }

    static int usableDestinations() noexcept
    {
        const int n = ModMatrix::numDestinations();
        return n < tableRoom ? n : tableRoom;
    }

    float sourceValues[tableRoom] {};
    float destinationSums[tableRoom] {};
};

} // namespace Synth
