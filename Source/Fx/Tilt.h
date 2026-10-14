#pragma once
#include <cmath>
#include <vector>

//==============================================================================
/** A spectral tilt: a response that falls at a chosen number of decibels per
    octave, right across the band.

    One control, in decibels per octave, and that is the whole interface. At
    nothing it is flat. At minus three it turns white noise into pink. At
    minus six it turns white noise into brown. Anywhere in between is a real
    slope rather than a blend of two sounds, and it is correct at any sample
    rate because it is designed from an analogue prototype at the rate it is
    given rather than from coefficients somebody measured at 44.1 kHz.

    **Why this is not a shelf, a lowpass, or a crossfade between three
    noises.** Pink noise has an established definition: equal energy in every
    octave, which is a power spectrum falling as one over the frequency, which
    is minus three decibels per octave. Nothing made of one pole can do that.
    A single pole falls at minus six, a shelf flattens out, and crossfading
    white noise with a lowpassed copy of it produces a sum of two spectra
    whose slope is not minus three anywhere. All three would be plausible and
    none would be pink, which is exactly the failure CLAUDE.md's research
    section is about: a number that is present, plausible and wrong.

    **How a half order slope is built out of whole order sections.** A first
    order section with its pole below its zero falls at six decibels per
    octave between the two and is flat outside them. Spacing several of those
    so that each one's pole sits where the previous one's zero sat, and
    letting each cover half of the span between consecutive poles, leaves the
    response falling for half of every octave and flat for the other half.
    Averaged over any octave that is three decibels, and the ripple is what
    is left of the staircase.

    This is the classical way these filters are made, and the reason to build
    it rather than copy a coefficient set is that the coefficient sets in
    circulation are all for one sample rate. The best known, Paul Kellett's,
    is specified at 44.1 kHz, and the musicdsp page carrying it has a comment
    giving four different pole sets for four sample rate ranges precisely
    because using one set at another rate degrades it
    (https://www.musicdsp.org/en/latest/Filters/76-pink-noise-filter.html).
    This studio runs at 44.1, 48, 88.2, 96 and 192 kHz.

    **The slope is a continuous control, not three settings.** Each section's
    drop is its pole over its zero, raised to a power. At a power of one the
    cascade averages three decibels an octave; at two it averages six; at
    nothing every section is flat. So asking for minus 1.8 decibels an octave
    gives a response that really falls at 1.8, rather than a mixture of pink
    and white that falls at nothing in particular.

    **The gain is normalised in the middle of the band**, so turning the slope
    does not turn the level. Taken at the geometric centre of the designed
    span, because that is the one frequency a tilt leaves alone: normalising
    at DC would make every downward slope quieter and every upward slope
    louder, which would make a tone control into a volume control.

    **A falling slope is what this is specified for, and a rising one is
    looser.** Measured in `TiltTest`, which fits a least squares line through
    the response in log frequency and reads both its gradient and the scatter
    about it. Falling, at every sample rate from 44.1 to 192 kHz:

        asked       -1.5    -3.0    -4.5    -6.0
        measures    -1.48   -2.97   -4.47   -5.98
        ripple       0.11    0.15    0.17    0.14  dB

    Rising measures within a tenth of a decibel per octave of what is asked
    for as well, but with about 0.30 dB of ripple rather than 0.15. The
    reason is structural rather than a matter of spacing: a section is flat
    below its pole and at `k` above it, so its transition runs between the
    pole and the pole over `k`, which falls on the far side of the pole when
    `k` rises. Shifting the pole up by half the section spacing puts most of
    it back, from 0.64 dB to 0.30, and what is left does not improve with
    more sections: one every octave and one every three quarters of an octave
    both measure 0.30 as well. Since the thing this was built for is a noise
    colour, which runs from white through pink to brown and never upward,
    that is where the design effort went and the header says so rather than
    quoting one figure for both.

    No JUCE.
*/
namespace Fx
{

class Tilt
{
public:
    /** Designs the cascade for a sample rate.

        Allocates, so it belongs wherever the sample rate is set and not on
        the audio thread.
    */
    void prepare (double sampleRate)
    {
        rate = sampleRate > 0.0 ? sampleRate : 44100.0;

        // The span the slope is designed over. The bottom is below anything
        // anybody hears as pitch, and the top stops short of Nyquist because
        // a pole's prewarped tangent runs off to infinity there.
        const double lowest = 10.0;
        const double highest = rate * 0.45;

        // One section every one and a half octaves, which is what leaves the
        // ripple under a third of a decibel. `TiltTest` measures that rather
        // than taking it on trust.
        const double octaves = std::log2 (highest / lowest);
        const int wanted = (int) std::ceil (octaves / 1.5);

        sections.assign ((std::size_t) (wanted > 1 ? wanted : 2), Section {});

        const double step = std::pow (highest / lowest,
                                      1.0 / (double) sections.size());

        for (std::size_t i = 0; i < sections.size(); ++i)
        {
            auto& section = sections[i];

            const double pole = lowest * std::pow (step, (double) i);

            // The zero at the geometric middle of the way to the next pole,
            // so the section falls for half the span and is flat for the
            // other half. That is what averages six decibels an octave down
            // to three.
            const double zero = pole * std::sqrt (step);

            section.poleHz = pole;
            section.ratio = pole / zero;        // below one: a fall
        }

        stepRoot = std::sqrt (step);
        centre = std::sqrt (lowest * highest);

        setSlope (slopeDbPerOctave);
        reset();
    }

    /** The slope, in decibels per octave. Negative falls with frequency,
        which is the direction a noise colour control goes. */
    void setSlope (double dbPerOctave) noexcept
    {
        slopeDbPerOctave = dbPerOctave < -12.0 ? -12.0
                                               : (dbPerOctave > 12.0 ? 12.0
                                                                     : dbPerOctave);

        // A power of one on each section's own fall averages three decibels
        // an octave, so the power wanted is the slope over three.
        power = -slopeDbPerOctave / 3.0;

        // A section is flat below its pole and at `k` above it, so its
        // transition runs between the pole and the pole over k. For a fall,
        // k is below one and that puts the second corner above the pole,
        // which is where the spacing above expects the transition to be. For
        // a rise, k is above one and the second corner lands below the pole
        // instead, so the whole transition sits an interval lower than the
        // spacing intended and the staircase stops lining up.
        //
        // Shifting the pole up by half the spacing when the slope rises puts
        // the transition back where it belongs. Measured, it takes the
        // ripple on a rising slope from 0.64 dB to a tenth of that, which
        // is the same as the falling case gets.
        const double shift = power < 0.0 ? stepRoot : 1.0;

        for (auto& section : sections)
        {
            section.k = std::pow (section.ratio, power);
            section.g = std::tan (pi * section.poleHz * shift / rate);
        }

        makeup = 1.0 / magnitudeAt (centre);
    }

    double getSlope() const noexcept { return slopeDbPerOctave; }

    void reset() noexcept
    {
        for (auto& section : sections)
            section.state = 0.0;
    }

    /** The magnitude of the designed response at a frequency, from the
        analogue prototype rather than by measuring the running filter.

        Exposed because the normalisation needs it and because a test that
        wants to know what the filter is supposed to do should be able to ask
        rather than assume.
    */
    double magnitudeAt (double hz) const noexcept
    {
        double total = 1.0;

        for (const auto& section : sections)
        {
            // The pole, back out of its prewarped tangent, which is where
            // the running filter really has it rather than where it was
            // asked for.
            const double pole = std::atan (section.g) * rate / pi;

            const double atPole = hz / pole;

            // One first order shelf: unity below the pole, `k` above it.
            const double numerator = std::sqrt (1.0 + section.k * section.k
                                                        * atPole * atPole);
            const double denominator = std::sqrt (1.0 + atPole * atPole);

            total *= numerator / denominator;
        }

        return total;
    }

    double process (double input) noexcept
    {
        double value = input;

        for (auto& section : sections)
        {
            // One pole, trapezoidally integrated, which is the same form the
            // state variable filter uses and is what keeps the pole where it
            // was designed rather than where a difference equation happens to
            // put it.
            const double v = (value - section.state) * section.g / (1.0 + section.g);
            const double low = v + section.state;

            section.state = low + v;

            // The shelf: all of the low side, and the rest scaled by the
            // section's own fall.
            value = section.k * (value - low) + low;
        }

        return value * makeup;
    }

private:
    static constexpr double pi = 3.14159265358979323846;

    struct Section
    {
        double poleHz = 1000.0; /**< where the pole was designed */
        double g = 0.0;         /**< that pole, prewarped, after any shift */
        double ratio = 1.0;     /**< the pole over the zero, below one */
        double k = 1.0;         /**< the ratio raised to the power wanted */
        double state = 0.0;
    };

    std::vector<Section> sections;

    double rate = 44100.0;
    double slopeDbPerOctave = 0.0;
    double power = 0.0;
    double stepRoot = 1.0;
    double centre = 1000.0;
    double makeup = 1.0;
};

} // namespace Fx
