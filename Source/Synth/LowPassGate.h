#pragma once
#include "../Fx/Svf.h"

#include <cmath>

//==============================================================================
/** A low pass gate: the thing that makes west coast patches sound struck
    rather than switched on.

    An ordinary synth voice has a filter and an amplifier and an envelope for
    each. A low pass gate has one control doing both at once, through a
    vactrol: a small lamp facing a light dependent resistor, sealed in a tube.
    Turn the control up and the lamp brightens, the resistance falls, and the
    sound gets both louder and brighter together. Turn it down and it gets
    quieter and duller together.

    That pairing is the whole point, and it is why a low pass gate sounds like
    something physical being hit. Real objects do this: a struck drum, a
    plucked string and a bell all lose their high partials faster than their
    fundamental, so quiet and dark arrive together. A synth with a separate
    filter and amplifier can be set up to imitate that, and almost nobody
    does, because it means drawing the same envelope twice and matching them
    by ear.

    The second half of it is the vactrol itself, which is slow and lopsided.
    The lamp lights almost at once and the resistor's material then takes a
    long time to relax back, so the gate strikes fast and releases slowly
    whatever the control does, and no envelope setting reproduces that. It is
    why a low pass gate hit with a step still sounds like a plucked note.

    Modelled from Parker and D'Angelo's "A Digital Model of the Buchla
    Lowpass-Gate", DAFx-13:
    https://dafx.de/paper-archive/2013/papers/44.dafx2013_submission_56.pdf

    Taken from that paper:

    - The vactrol is a lowpass whose time constant switches on the sign of
      the control's slope, and is further modulated by the current output, so
      it responds faster when it is already bright.
    - Its measured time constants, from the VTL5C3/2 datasheet: about **12 ms**
      rising and about **250 ms** falling. Twenty times slower to let go than
      to light up, which is the figure that gives the sound its shape.
    - The three modes the hardware offers, and that this keeps: **both**,
      which is the one worth having, **lowpass** on its own, and **amplitude**
      on its own, which is an ordinary gate.
    - That in the combined mode the two poles are not coincident, so the slope
      is nearer 6 dB per octave than 12.

    Not taken from it: the component values. That paper models one particular
    circuit, the Buchla 292, down to its zener breakdown voltage and the fit
    of its photodiode, and reproducing those here would be claiming to be that
    module. What is modelled is the behaviour the paper measures and explains,
    which is what the 0-Coast's Dynamics section and every other low pass gate
    share.

    No JUCE, so the thing that actually identifies a low pass gate can be
    asserted rather than hoped for: `LowPassGateTest` strikes it and measures
    the brightness falling as the level falls, which is the property, and
    which a separate filter and amplifier would not have.
*/
namespace Synth
{

class LowPassGate
{
public:
    enum class Mode
    {
        both = 0,     /**< brightness and level together, the one to use */
        lowpass,      /**< brightness only, level left alone */
        amplitude     /**< level only, an ordinary gate */
    };

    //==========================================================================
    /** The vactrol's measured time constants, in seconds.

        From the VTL5C3/2 datasheet by way of the paper above. These are not
        tuning: they are what the part does, and they are the reason a low
        pass gate cannot be imitated with a fast envelope.
    */
    static constexpr double risingSeconds  = 0.012;
    static constexpr double fallingSeconds = 0.250;

    void setSampleRate (double newRate) noexcept
    {
        sampleRate = newRate > 0.0 ? newRate : 44100.0;
        filter.setSampleRate (sampleRate);
        updateCoefficients();
    }

    void setMode (Mode m) noexcept { mode = m; }

    /** How far the gate opens when the control is fully up, in octaves above
        its resting cutoff. Wider is brighter at the top and makes the
        darkening on the way down more pronounced. */
    void setRange (double octaves) noexcept
    {
        range = octaves < 0.0 ? 0.0 : (octaves > 10.0 ? 10.0 : octaves);
    }

    /** Where the filter sits with the gate shut. Low enough to be nearly
        silent, high enough that the tail does not vanish before the level
        does. */
    void setRestingCutoff (double hz) noexcept
    {
        resting = hz < 10.0 ? 10.0 : (hz > 8000.0 ? 8000.0 : hz);
    }

    /** How much of the vactrol's lag to apply, from none to all of it.

        At zero this is an ordinary filter and amplifier following the
        control exactly, which is worth having because a patch sometimes
        wants the pairing without the slowness. At one it is the measured
        part. In between is not a model of anything and is not claimed to be:
        it is a control, and it interpolates the time constants.
    */
    void setLag (double amount) noexcept
    {
        lag = amount < 0.0 ? 0.0 : (amount > 1.0 ? 1.0 : amount);
        updateCoefficients();
    }

    void reset() noexcept
    {
        filter.reset();
        level = 0.0;
    }

    /** What the vactrol is doing, from shut to fully open. Exposed because it
        is worth drawing: it is the thing that is slow, and seeing it is how
        somebody understands why the gate does not follow their envelope. */
    double getOpenness() const noexcept { return level; }

    //==========================================================================
    /** One sample. `control` is where the envelope wants the gate, from zero
        to one; the vactrol decides how fast it gets there. */
    double process (double input, double control) noexcept
    {
        if (control < 0.0) control = 0.0;
        if (control > 1.0) control = 1.0;

        // The lamp lights quickly and the resistor relaxes slowly, so which
        // constant applies depends on which way the control is going.
        const double rising = control > level;

        // And it is faster when it is already bright, which the datasheet
        // shows and the paper models the same way. At the bottom of its range
        // the vactrol is at its slowest, which is what stretches the tail of
        // a note out rather than ending it.
        const double speedUp = 1.0 + 3.0 * level;

        const double coefficient = (rising ? risingCoefficient : fallingCoefficient);

        // A one pole lag towards the control. The coefficient is the fraction
        // of the remaining distance covered per sample, so raising it by the
        // level makes the approach faster without changing where it lands.
        const double step = (control - level) * coefficient * speedUp;

        level += step;

        if (level < 0.0) level = 0.0;
        if (level > 1.0) level = 1.0;

        // The level drives both, which is the definition of the thing.
        const double cutoff = resting * std::pow (2.0, range * level);

        if (mode != Mode::amplitude)
        {
            filter.set (Fx::Svf::Type::lowpass, cutoff, 0.70710678118654752);
            input = filter.process (input);
        }

        if (mode != Mode::lowpass)
        {
            // Squared rather than straight, because a vactrol's transmission
            // falls off faster than its resistance and because a gate that
            // closes linearly sounds like a fade rather than a decay.
            input *= level * level;
        }

        return input;
    }

private:
    void updateCoefficients() noexcept
    {
        // The fraction of the remaining distance a one pole lag covers in one
        // sample, for a given time constant. Written from the exponential
        // rather than approximated, so it stays right at any sample rate.
        const auto fractionFor = [this] (double seconds)
        {
            if (! (seconds > 0.0))
                return 1.0;

            return 1.0 - std::exp (-1.0 / (seconds * sampleRate));
        };

        // At no lag the gate follows the control within a sample; at full lag
        // it takes the measured times. Interpolated on the time rather than
        // on the coefficient, because the coefficient is exponential in the
        // time and halfway between the two coefficients is not halfway
        // between the two speeds.
        const double rise = risingSeconds * lag;
        const double fall = fallingSeconds * lag;

        risingCoefficient = fractionFor (rise);
        fallingCoefficient = fractionFor (fall);
    }

    Fx::Svf filter;

    Mode   mode = Mode::both;
    double sampleRate = 44100.0;
    double resting = 60.0;
    double range = 7.0;
    double lag = 1.0;

    double level = 0.0;
    double risingCoefficient = 1.0;
    double fallingCoefficient = 1.0;
};

} // namespace Synth
