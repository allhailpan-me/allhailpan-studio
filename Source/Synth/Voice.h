#pragma once
#include "WaveBank.h"
#include "LowPassGate.h"
#include "Slope.h"
#include "ModMatrix.h"

#include "../Envelope.h"
#include "../Fx/Oversampler.h"
#include "../Fx/Saturator.h"
#include "../Fx/Svf.h"
#include "../Fx/WaveFolder.h"

#include <cmath>
#include <cstdint>
#include <limits>

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

        osc 1 ─┐    ┌─────── four times oversampled ───────┐
        osc 2 ─┼───>│ fold, mixed in ─> drive ─> filter    │──> gate ──> out
        sub  ──┤    └─────────────────────────────────────┘
        noise ─┘

    Fixed because a folder after a gate is a folder being fed something that
    has already been made quiet, so it stops folding as the note decays and
    the patch loses its character exactly where it was interesting. Every
    hardware voice of this kind puts the shaping before the gate for the same
    reason. The filter goes between the two so that one voice can be either
    tradition: a west coast patch folds and lets the gate do the shaping, an
    east coast one leaves the fold mixed out and sweeps the filter, and
    neither is a different code path.

    **The fold is mixed in rather than switched on**, and its mix sits at
    nothing by default. A wavefolder is a colour, not something a voice
    should be wearing before anybody has asked: at a drive of one the
    characteristic is already a soft saturation with its own gain, so a
    folder permanently in circuit means the instrument has no clean sound at
    all. The crossfade is also how the hardware presents it, where the Timbre
    control on a Buchla 259 sweeps into the folds rather than switching them
    in.

    **The nonlinear section runs at four times the sample rate.** That is
    where the folder's and the saturator's own measurements were taken, and
    running them at the base rate instead would be shipping two modules
    outside the conditions their documented figures are about. It is one
    oversampled section rather than one each: two would filter twice, delay
    twice and cost twice, for a signal that is going through both of them in
    series anyway. The filter is inside it too, which costs almost nothing
    once the section exists and means a driven filter is driven into a filter
    rather than beside one.

    The section delays the voice by the oversampler's filters, which is about
    44 samples at a 48 kHz rate, and `latencySamples` reports it.

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

        // Folder. The mix rests at nothing, so an untouched patch is clean:
        // see the class comment.
        double foldDrive = 1.0;
        double foldSymmetry = 0.0;
        double foldMix = 0.0;

        // Drive, into the filter. Zero is transparent rather than silent:
        // the saturator divides by its own curve at the drive, so a drive of
        // nothing passes the signal through at unity.
        Fx::Saturator::Curve driveCurve = Fx::Saturator::Curve::soft;
        double driveAmount = 0.0;
        double driveBias = 0.0;

        // Filter. Open by default, for the same reason the fold is mixed
        // out: an instrument nobody has touched should sound like its
        // oscillators and not like somebody else's idea of a patch.
        Fx::Svf::Type filterType = Fx::Svf::Type::lowpass;
        double cutoff = 18000.0;
        double resonance = 0.70710678118654752;

        /** How much the cutoff follows the note. One means it tracks exactly,
            so every note is filtered the same number of octaves above its own
            fundamental and the timbre stays put across the keyboard, which is
            what a pluck or a bass wants. Nothing means the filter stays where
            it is in hertz and high notes come out darker relative to
            themselves, which is what an acoustic instrument does. Neither is
            the right default for everything, so it is a control. */
        double filterKeyTrack = 0.0;

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

        // One oversampler for the whole nonlinear section, prepared for a
        // single sample at a time. The stages are plain finite impulse
        // response filters carrying their own history, so feeding them one
        // sample per call gives exactly what feeding them a block would:
        // there is no block boundary in them to get wrong.
        oversampler.prepare (sampleRate, oversamplingFactor, 1);

        const double high = oversampler.getOversampledRate();

        folder.prepareAtRate (high);
        saturator.prepareAtRate (high);
        filter.setSampleRate (high);
    }

    /** What the oversampling costs, in samples of the base rate.

        A voice is not lined up against a dry path, so this is not a
        correctness problem the way it is for an effect, but it is the delay
        between a note arriving and the first sample of it leaving, and the
        instrument has to report it so the studio's compensation can take it
        off.
    */
    /** The highest note that folds without audible aliasing at the patch's
        current fold drive, in hertz.

        Nothing is clamped to it. It is here because folding is a bass and
        midrange instrument and the edge is a real one: above this the folded
        harmonics run past where even four times oversampling can hold them,
        which is a property of folding rather than of this implementation.
        A patch, a test or the interface can use it to keep off that edge, or
        to say so, rather than finding it by ear and reporting it as a bug.

        It is the folder's own figure, and it is four times what the same
        folder would report at the base rate, because the folder inside this
        voice really is running at four times. That is the only thing in the
        voice that reads the rate the folder was prepared at, which is why it
        is worth exposing: told the wrong rate, the folder would carry on
        sounding exactly right and quietly report a quarter of the range it
        has.
    */
    double foldsCleanlyBelow() const noexcept
    {
        return folder.foldsCleanlyBelow();
    }

    double latencySamples() const noexcept
    {
        // The second order antialiasing in the folder costs a sample at the
        // oversampled rate on top of the filters themselves.
        return oversampler.latencySamples() + 1.0 / (double) oversamplingFactor;
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

        saturator.setCurve (patch.driveCurve);

        // The folder's drive is set again every sample, because it is a
        // modulation destination, so this looks redundant and is not: it is
        // what makes `foldsCleanlyBelow` mean something before a note has
        // been played. Without it the figure is whatever drive the last
        // sample happened to use, or the folder's own default on a voice
        // that has never sounded, which is the state an interface asks in.
        folder.setDrive (patch.foldDrive);

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
        saturator.reset();
        filter.reset();
        oversampler.reset();
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

        // Only when it has actually moved. Retuning is three powers of two
        // and three logarithms, because picking the right level of a mipmap
        // is a logarithm, and on a patch with nothing pointed at either
        // pitch those six answers are the same every sample for the whole
        // length of the note. Done unconditionally it measured as a fifth of
        // a processor core across sixteen voices, spent arriving at numbers
        // that were already there.
        //
        // The comparison is against a value seeded as a quiet NaN, so the
        // first sample of every note always retunes: NaN compares unequal to
        // everything including itself, which is exactly the "no idea, do it"
        // this wants and is why it is not a separate flag.
        if (bend1 != lastBend1 || bend2 != lastBend2)
        {
            lastBend1 = bend1;
            lastBend2 = bend2;

            const double first = frequencyOf ((double) note + bend1);

            osc1.setFrequency (first);
            osc2.setFrequency (frequencyOf ((double) note + patch.osc2Semitones + bend2));
            sub.setFrequency (first * 0.5);
        }

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

        const double foldMix = modulated (destination.foldMix, patch.foldMix);
        const double drive = modulated (destination.drive, patch.driveAmount, 0.0, 64.0);

        saturator.setDrive (drive);
        saturator.setBias (modulated (destination.driveBias, patch.driveBias, -1.0));

        // The cutoff is modulated in octaves and not in hertz, because the
        // destination declares itself in octaves and because an envelope
        // worth a fixed number of hertz is a different sweep on every note.
        // The patch's own cutoff is where it rests; everything else moves it
        // by a factor.
        const double octaves = modulated (destination.cutoff, 0.0, -12.0, 12.0)
                             + patch.filterKeyTrack * ((double) note - 60.0) / 12.0;

        const double q = modulated (destination.resonance, patch.resonance, 0.025, 40.0);

        // Guarded for the same reason the pitch is, and the saving is the
        // same shape: a power of two to turn octaves into a frequency, and a
        // tangent inside the filter to turn that frequency into its
        // coefficients. On a patch with nothing pointed at the cutoff or the
        // resonance, both answers hold for the whole note.
        if (octaves != lastOctaves || q != lastQ)
        {
            lastOctaves = octaves;
            lastQ = q;

            filter.set (patch.filterType, patch.cutoff * std::exp2 (octaves), q);
        }

        mixed = throughTheNonlinearSection (mixed, foldMix, drive);

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
    /** Fold, drive and filter, all at four times the rate.

        One sample in and one out. The oversampler's stages are finite
        impulse response filters holding their own history, so a call per
        sample is the same arithmetic as a call per block, and keeping the
        voice sample by sample is what lets every modulation source run at
        the sample rate rather than stepping once a block.

        The fold is crossfaded inside the section rather than outside it. Out
        here the dry side would have gone through the oversampler's filters
        and the folded side would have gone through them too, which sounds
        like the same thing and is not: the crossfade would then be between
        two signals that are each a filtered version of something, and the
        blend of the two is not the filtered version of the blend once the
        folder has been in between them. Inside, the two sides are the signal
        and the fold of it at the same instant.
    */
    double throughTheNonlinearSection (double input, double foldMix, double drive) noexcept
    {
        double* high = oversampler.upsample (&input, 1);

        if (high == nullptr)
            return input;

        // Neither nonlinearity is run when it is doing nothing, and on the
        // ordinary patch neither of them is: the fold rests mixed out and
        // the drive rests at zero. Running them anyway was most of the cost
        // of the whole instrument. The simplest possible patch, one
        // oscillator and nothing else, measured the same as the busiest one,
        // because the two expensive parts were in circuit either way.
        //
        // Skipping is exact rather than nearly exact. At a mix of nothing
        // the folded signal is multiplied by zero, and at the smallest drive
        // the saturator's own makeup gain cancels its curve to within a part
        // in ten thousand, which is what the saturator's test measures.
        const bool folding = foldMix > 1.0e-6;
        const bool driving = drive > 1.0e-3;

        const int total = oversampler.ratio();

        // Coming back in, the module has a history from whenever it was last
        // used, which may be a long time ago and a different signal. The
        // antialiasing is a difference across that history, so one sample
        // would come out of nowhere. Two throwaway evaluations at the
        // current value fill it with the present instead, which costs two
        // samples of work once rather than every sample forever.
        if (folding && ! wasFolding)
        {
            folder.processOneAtCurrentRate (high[0]);
            folder.processOneAtCurrentRate (high[0]);
        }

        if (driving && ! wasDriving)
        {
            saturator.processOneAtCurrentRate (high[0]);
            saturator.processOneAtCurrentRate (high[0]);
        }

        wasFolding = folding;
        wasDriving = driving;

        for (int i = 0; i < total; ++i)
        {
            double value = high[i];

            if (folding)
                value += foldMix * (folder.processOneAtCurrentRate (value) - value);

            if (driving)
                value = saturator.processOneAtCurrentRate (value);

            high[i] = filter.process (value);
        }

        double out = 0.0;
        oversampler.downsample (&out, 1);

        return out;
    }

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
        {
            // The rate is modulated before the slope is ticked, which is
            // what makes one slope able to speed another up and is how a
            // patch gets a sweep that accelerates. It reads the sums left by
            // the previous sample, because the sources for this one are
            // still being gathered: a sample of lag on a control that is
            // itself a slow sweep is not a thing anybody can hear, and the
            // alternative is evaluating the matrix twice per sample.
            const double octaves = modulated (destination.slopeRate[i], 0.0, -8.0, 8.0);

            if (octaves != 0.0)
            {
                const double faster = std::exp2 (-octaves);

                slopes[(std::size_t) i].setTimes (patch.slopes[i].riseSeconds * faster,
                                                  patch.slopes[i].fallSeconds * faster);
            }

            sourceValues[source.slope[i]] = (float) slopes[(std::size_t) i].nextSample();
        }

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
        // Anything that reaches here has changed the note or the patch, so
        // what the pitch and the filter were last computed from no longer
        // says anything about what they should be now. Forgetting is how the
        // guards in `nextSample` are kept honest: without this, changing a
        // patch's cutoff while a note is held would leave the filter on the
        // old coefficients until something happened to modulate it.
        lastBend1 = lastBend2 = lastOctaves = lastQ
            = std::numeric_limits<double>::quiet_NaN();

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
            foldMix      = ModMatrix::destinationFromId ("foldmix");
            drive        = ModMatrix::destinationFromId ("drive");
            driveBias    = ModMatrix::destinationFromId ("drivebias");
            cutoff       = ModMatrix::destinationFromId ("cutoff");
            resonance    = ModMatrix::destinationFromId ("resonance");
            gateOpen     = ModMatrix::destinationFromId ("gateopen");
            gateRange    = ModMatrix::destinationFromId ("gaterange");
            amp          = ModMatrix::destinationFromId ("amp");
            slopeRate[0] = ModMatrix::destinationFromId ("slope1rate");
            slopeRate[1] = ModMatrix::destinationFromId ("slope2rate");
        }

        int osc1Pitch = -1, osc2Pitch = -1;
        int osc1Position = -1, osc2Position = -1;
        int osc1Level = -1, osc2Level = -1, subLevel = -1, noiseLevel = -1;
        int fm = -1, fold = -1, foldSymmetry = -1, foldMix = -1;
        int drive = -1, driveBias = -1, cutoff = -1, resonance = -1;
        int gateOpen = -1, gateRange = -1, amp = -1;
        int slopeRate[2] { -1, -1 };
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

    /** Measured, not chosen, and measured twice: four times is where the
        wavefolder's aliasing stops improving, and where every one of the
        saturator's curves at every drive goes under the floor of the
        measurement. Both figures are in those two headers. */
    static constexpr int oversamplingFactor = 4;

    Fx::Oversampler oversampler;
    Fx::WaveFolder folder;
    Fx::Saturator saturator;
    Fx::Svf filter;
    LowPassGate gate;
    Envelope envelopes[4];
    Slope slopes[2];

    /** What the pitch and the filter were last set from, so that setting
        them again can be skipped when nothing moved. Seeded as quiet NaNs,
        which compare unequal to everything, so the first sample after any
        change of note or patch recomputes rather than trusting these. */
    double lastBend1 = std::numeric_limits<double>::quiet_NaN();
    double lastBend2 = std::numeric_limits<double>::quiet_NaN();
    double lastOctaves = std::numeric_limits<double>::quiet_NaN();
    double lastQ = std::numeric_limits<double>::quiet_NaN();

    double sampleRate = 44100.0;
    double velocity = 1.0;
    double perNote = 0.0;
    double noiseFilter = 0.0;

    std::uint64_t randomState = 0x9e3779b97f4a7c15ull;
    std::uint64_t noiseState = 0x2545f4914f6cdd1dull;

    int  note = -1;
    bool held = false;

    /** Whether each nonlinearity was in circuit last sample, so that
        coming back in can fill its history with the present signal
        rather than with whatever it was last given. */
    bool wasFolding = false, wasDriving = false;

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
