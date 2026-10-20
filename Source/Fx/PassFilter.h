#pragma once
#include "OnePole.h"
#include "Svf.h"

#include <array>
#include <cmath>
#include <cstddef>

//==============================================================================
/** A high pass or low pass of any slope from six to ninety six decibels per
    octave, Butterworth aligned.

    Butterworth means one thing exactly: `|H|^2 = 1 / (1 + (f/fc)^(2N))`,
    maximally flat in the passband, monotonic, and **minus 3.0103 decibels at
    the cutoff at every order**. That last number is what makes the frequency
    on the control mean something, and it is the thing the obvious
    implementation gets wrong.

    **The sections do not all have the same Q.** An order 2N cascade is N two
    pole sections, all at the same cutoff, whose damping comes from the
    Butterworth pole angles. Analog Devices, *Basic Linear Design*, chapter 8
    (Zumbahlen), equation 8-32 puts the poles of an order n Butterworth at

        pole_K = -sin((2K - 1) pi / 2n) + j cos((2K - 1) pi / 2n)

    on the unit circle, so a conjugate pair contributes `s^2 + 2 sin(theta) s
    + 1` and therefore

        Q_K = 1 / (2 sin((2K - 1) pi / (2N)))

    for K from one to half of N, plus, for odd N, one real pole: a single
    first order section at the same cutoff, which is what `OnePole.h` is for.

        slope    order   section Q values
        6        1       one pole only
        12       2       0.707107
        18       3       1.000000 and one pole
        24       4       1.306563, 0.541196
        36       6       1.931852, 0.707107, 0.517638
        48       8       2.562915, 0.899976, 0.601345, 0.509796
        72       12      3.830649, 1.306563, 0.821340, 0.630236, 0.541196, 0.504314
        96       16      5.101149, 1.722447, 1.060678, 0.788155, 0.646822,
                         0.566944, 0.522499, 0.502419

    **Cascading sections that are all at Q = 0.707 is a different filter and
    it droops.** That construction is a real alignment, it is what a
    Linkwitz-Riley crossover is, and it is not this one. Measured at the
    cutoff:

        built as                        gain at fc    worst passband error
        24 dB/oct as two at Q 0.7071    -6.021 dB     3.010 dB
        36 dB/oct as three at Q 0.7071  -9.031 dB     6.021 dB
        48 dB/oct as four at Q 0.7071   -12.042 dB    9.031 dB

    against minus 3.0103 for Butterworth at every order. `PassFilterTest`
    asserts both: the right construction against the closed form, and the
    wrong construction's signature, so that one cannot quietly replace the
    other. Writing a constant into the Q table is the likeliest future
    breakage here and nothing else in the suite would see it.

    **The steep end has a real resonance in it, and that is Butterworth
    rather than a fault.** The highest section's Q grows with order: 0.707 at
    order two, 1.31 at four, 2.56 at eight, **5.10 at sixteen**. A Q of five
    at the cutoff is a group delay peak and it rings, so a ninety six decibel
    per octave high pass on a kick drum will sound like one. Said here rather
    than discovered from a bug report. Bessel, which trades the flat magnitude
    for flat group delay and much lower section Qs, and Linkwitz-Riley, which
    is two cascaded Butterworths of half the order and sits at minus six
    decibels at the cutoff, are the two named alternatives (Biamp's Tesira
    documentation is a clean citation for those three being the established
    set, and for the minus three against minus six distinction). Neither is
    implemented here, and neither is approximated: they are named, not
    pretended at.

    **Do not test this by measuring decibels per octave.** The bilinear
    frequency axis makes that reading wrong in both directions depending on
    where it is taken: a six decibel per octave low pass at 1 kHz and 48 kHz
    measures 5.46 dB between 2 and 4 kHz, because the asymptote is not reached
    yet, and 9.49 dB between 8 and 16 kHz, because of the warping near
    Nyquist. Both are the same correct filter. Compare against the closed form
    on the prewarped axis, where the agreement is around 1e-14 dB and the test
    means something.

    No JUCE.
*/
namespace Fx
{

class PassFilter
{
public:
    /** Sixteen poles is ninety six decibels per octave, which is where every
        equaliser worth comparing against stops. */
    static constexpr int maxOrder = 16;

    enum class Type { highpass = 0, lowpass };

    void setSampleRate (double newRate) noexcept
    {
        sampleRate = newRate > 0.0 ? newRate : 44100.0;

        for (auto& section : pairs)
            section.setSampleRate (sampleRate);

        single.setSampleRate (sampleRate);

        update();
    }

    double getSampleRate() const noexcept { return sampleRate; }

    /** `poles` is the order: one pole is six decibels per octave, sixteen is
        ninety six. Anything outside that is clamped rather than refused,
        because a saved project with a value from a later version has to open. */
    void set (Type newType, double hz, int poles) noexcept
    {
        type = newType;
        frequency = hz;
        order = poles < 1 ? 1 : (poles > maxOrder ? maxOrder : poles);

        update();
    }

    int getOrder() const noexcept { return order; }
    double slopeDbPerOctave() const noexcept { return 6.0 * (double) order; }

    void reset() noexcept
    {
        for (auto& section : pairs)
            section.reset();

        single.reset();
    }

    double process (double input) noexcept
    {
        double value = input;

        for (std::size_t i = 0; i < numPairs; ++i)
            value = pairs[i].process (value);

        if (hasSingle)
            value = single.process (value);

        return value;
    }

    /** The exact magnitude, as the product of the sections' own. Same reason
        the two filters have one each: one copy of the algebra. */
    double magnitudeAt (double hz) const noexcept
    {
        double total = 1.0;

        for (std::size_t i = 0; i < numPairs; ++i)
            total *= pairs[i].magnitudeAt (hz);

        if (hasSingle)
            total *= single.magnitudeAt (hz);

        return total;
    }

    /** The Butterworth damping of one section of an order N cascade, from the
        pole angle. Exposed because the test checks the table above against
        it and because a cascade built anywhere else should use the same one
        rather than a copy.

        `which` runs from zero, and only `N / 2` of them exist: the sections
        are the conjugate pairs, and the real pole an odd order carries is not
        one of them.
    */
    static double sectionQ (int totalOrder, int which) noexcept
    {
        constexpr double pi = 3.14159265358979323846;

        const int n = totalOrder < 1 ? 1 : totalOrder;
        const double angle = (double) (2 * which + 1) * pi / (2.0 * (double) n);
        const double s = std::sin (angle);

        return s > 0.0 ? 0.5 / s : 0.5;
    }

private:
    void update() noexcept
    {
        numPairs = (std::size_t) (order / 2);
        hasSingle = (order % 2) != 0;

        const auto pairType = type == Type::highpass ? Svf::Type::highpass
                                                     : Svf::Type::lowpass;

        for (std::size_t i = 0; i < numPairs; ++i)
            pairs[i].set (pairType, frequency, sectionQ (order, (int) i));

        if (hasSingle)
            single.set (type == Type::highpass ? OnePole::Type::highpass
                                               : OnePole::Type::lowpass,
                        frequency);
    }

    double sampleRate = 44100.0;
    double frequency  = 20.0;
    int    order      = 2;
    Type   type       = Type::highpass;

    std::array<Svf, (std::size_t) maxOrder / 2> pairs;
    OnePole single;

    std::size_t numPairs = 1;
    bool hasSingle = false;
};

} // namespace Fx
