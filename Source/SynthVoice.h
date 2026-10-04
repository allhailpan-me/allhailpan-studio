#pragma once
#include "Oscillator.h"
#include "Envelope.h"
#include "Filter.h"

#include <cmath>

//==============================================================================
/** One voice of the built-in synth: two oscillators, a sub, a filter with its
    own envelope, and an amplitude envelope.

    Everything here is arithmetic over the three headers beside it, with no
    JUCE, so the whole signal path of the instrument can be run and measured
    by a test rather than only heard.

    Two decisions worth stating, because both look like bugs otherwise.

    The oscillators free run. Taking a voice for a new note retunes them and
    retriggers the envelopes, but does not reset their phase. Resetting it
    would put a step into the waveform on every note, which is a click, and
    it is most audible exactly when the synth is busy enough to be stealing
    voices. Analogue polysynths free run for the same reason. The cost is
    that a note does not sound bit identical twice, which for an instrument
    is a feature rather than a defect.

    The filter envelope opens the cutoff in octaves rather than in hertz. An
    envelope worth a fixed number of hertz is enormous on a bass note and
    inaudible two octaves up, so it has to be set again for every patch and
    every register. In octaves one setting means the same musical movement
    wherever it is played.
*/
class SynthVoice
{
public:
    struct Patch
    {
        Oscillator::Shape osc1Shape = Oscillator::Shape::saw;
        Oscillator::Shape osc2Shape = Oscillator::Shape::saw;

        double osc2Detune = 0.08;        // semitones, for the beating between them
        double pulseWidth = 0.5;

        double osc1Level  = 0.5;
        double osc2Level  = 0.4;
        double subLevel   = 0.25;        // square, one octave down

        double cutoff     = 900.0;       // where the filter sits with the envelope shut
        double resonance  = 1.4;
        double envOctaves = 3.0;         // how far the filter envelope opens it

        Envelope::Settings amp    { 0.004, 0.180, 0.70, 0.250 };
        Envelope::Settings filter { 0.002, 0.260, 0.35, 0.220 };
    };

    void setSampleRate (double newRate) noexcept
    {
        sampleRate = newRate > 0.0 ? newRate : 44100.0;

        osc1.setSampleRate (sampleRate);
        osc2.setSampleRate (sampleRate);
        sub.setSampleRate (sampleRate);
        lowpass.setSampleRate (sampleRate);
        ampEnv.setSampleRate (sampleRate);
        filterEnv.setSampleRate (sampleRate);

        applyPatch();
    }

    void setPatch (const Patch& p) noexcept
    {
        patch = p;
        applyPatch();
        retune();
    }

    const Patch& getPatch() const noexcept { return patch; }

    /** Starts, or takes over, this voice. The envelopes pick up from wherever
        they already were, so stealing fades across rather than cutting. */
    void start (int midiNote, float velocity) noexcept
    {
        note = midiNote;
        level = velocity < 0.0f ? 0.0f : (velocity > 1.0f ? 1.0f : velocity);
        retune();

        ampEnv.noteOn();
        filterEnv.noteOn();
    }

    void stop() noexcept
    {
        ampEnv.noteOff();
        filterEnv.noteOff();
    }

    /** Silences the voice at once, for when there is nothing left to steal
        and a click is better than a wrong note. */
    void kill() noexcept
    {
        ampEnv.reset();
        filterEnv.reset();
        lowpass.reset();
        note = -1;
    }

    /** False once the amplitude envelope has finished, which is the only
        thing that frees the voice. */
    bool isActive() const noexcept { return ampEnv.isActive(); }

    int getNote() const noexcept { return note; }

    float nextSample() noexcept
    {
        if (! ampEnv.isActive())
            return 0.0f;

        const double env = filterEnv.nextSample();

        // Octaves above the resting cutoff, so the same setting means the
        // same movement whatever note is being played.
        lowpass.setCutoff (patch.cutoff * std::pow (2.0, patch.envOctaves * env));

        double mixed = osc1.nextSample() * patch.osc1Level
                     + osc2.nextSample() * patch.osc2Level
                     + sub.nextSample()  * patch.subLevel;

        const double filtered = lowpass.nextSample ((float) mixed);
        const double amplitude = ampEnv.nextSample() * level;

        return (float) (filtered * amplitude);
    }

private:
    void applyPatch() noexcept
    {
        osc1.setShape (patch.osc1Shape);
        osc2.setShape (patch.osc2Shape);
        sub.setShape (Oscillator::Shape::square);

        osc1.setPulseWidth (patch.pulseWidth);
        osc2.setPulseWidth (patch.pulseWidth);

        lowpass.setMode (Filter::Mode::lowpass);
        lowpass.setResonance (patch.resonance);
        lowpass.setCutoff (patch.cutoff);

        ampEnv.setSettings (patch.amp);
        filterEnv.setSettings (patch.filter);
    }

    void retune() noexcept
    {
        if (note < 0)
            return;

        const double hz = frequencyOf ((double) note);

        osc1.setFrequency (hz);
        osc2.setFrequency (frequencyOf ((double) note + patch.osc2Detune));
        sub.setFrequency (hz * 0.5);
    }

    static double frequencyOf (double midiNote) noexcept
    {
        return 440.0 * std::pow (2.0, (midiNote - 69.0) / 12.0);
    }

    Patch patch;

    Oscillator osc1, osc2, sub;
    Filter     lowpass;
    Envelope   ampEnv, filterEnv;

    double sampleRate = 44100.0;
    double level = 1.0;
    int    note = -1;
};
