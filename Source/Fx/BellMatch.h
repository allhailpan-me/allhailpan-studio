#pragma once
#include <cmath>

//==============================================================================
/** A bell that still has a skirt at the top of the band.

    The ordinary digital bell, the one `Svf::Type::bell` gives and the one
    every cookbook gives, is the bilinear image of the analogue prototype
    `(s^2 + kA^2 s + 1) / (s^2 + k s + 1)`. The bilinear transform maps
    infinite analogue frequency onto Nyquist, and at infinite frequency that
    prototype is exactly one. **So a digital bell is exactly 0 dB at Nyquist,
    always, whatever its gain.** Its upper skirt is not wide, it is crushed
    against the top of the band.

    Measured against the analogue prototype it is supposed to be, worst case
    over 20 Hz to 20 kHz at 44.1 kHz:

        bell  1 kHz Q 1 +12 dB      0.052 dB
        bell  8 kHz Q 1  +6 dB      1.299 dB
        bell 12 kHz Q 1 +12 dB      5.233 dB
        shelf 12 kHz Q 0.707 +12 dB 2.196 dB

    Five decibels is not a subtlety. An "air" boost at 12 kHz is one of the
    half dozen moves everybody makes, and a studio whose bell is five
    decibels short of what the number says is a studio that disagrees with
    every other one on the material where it matters most.

    This is Orfanidis's correction. Rather than a prototype with the same
    gain at DC and at infinity, design one with a prescribed gain `G1` at
    infinity, chosen so that after the transform the digital filter has at
    Nyquist the gain the analogue filter has at half the sample rate. Source:

        Sophocles J. Orfanidis, "Digital Parametric Equalizer Design with
        Prescribed Nyquist-Frequency Gain", Journal of the Audio Engineering
        Society 45(6), June 1997, pages 444 to 455. First given as AES
        convention 101 preprint 4361, November 1996.

    The same correction brings the 12 kHz bell from 5.233 dB of error to
    1.140, which is about what two times oversampling would buy, for the cost
    of a handful of square roots when the band is set.

    **This is not a new filter.** Equation (13) of the paper is a prototype in
    `s`; the direct form of equation (20) is one realisation of it. Dividing
    through by `W` gives

        H(s_n) = (G1 s_n^2 + (B/W) s_n + G0) / (s_n^2 + (A/W) s_n + 1)

    which is the shape `Svf` already realises, so the whole of this is five
    numbers for the filter that is already there: `g = W`, `k = A/W`,
    `m0 = G1`, `m1 = B/W - G1 k`, `m2 = G0 - G1`. Nothing about the running
    filter changes, which matters: no second topology to modulate, no second
    place for a coefficient to be wrong.

    **It does not always work, and pretending otherwise would be the worst
    option.** The design needs `G0 < G1 < GB < G`, which is what keeps every
    square root under a positive number. With the bandwidth measured at half
    the decibel gain, that reduces to a condition on the centre frequency and
    Q alone, with no dependence on the gain at all:

        feasible  <=>  f0 < (fs / 4) (sqrt(1/Q^2 + 4) - 1/Q)

    At 44.1 kHz that is 9.1 kHz at Q 0.5, 11.4 kHz at Q 0.707, 13.6 kHz at
    Q 1, 17.2 kHz at Q 2 and 20.7 kHz at Q 8. So a wide bell high in the band,
    which is exactly the air move above, falls outside it. Above the boundary
    this reports that it could not match and the caller keeps the conventional
    design, which is what FabFilter's manual describes their own zero latency
    mode as doing when it admits "a small deviation above 18 kHz". The
    alternative, moving the bandedge gain to keep the correction, would make
    the Q displayed on the control mean a different thing at different
    frequencies, which is worse than a known error.

    **A cut is the reciprocal of the boost, not a boost with a negative
    sign.** Section 5 of the paper inverts all four gains, which makes the cut
    exactly `1 / H_boost`. Taking the absolute values in equation (17) alone
    is not enough: `A` and `B` take the square root of `G^2 - GB^2`, negative
    for a cut, and the result is a quiet NaN that reaches the user as an
    equaliser that sometimes kills the audio. So a cut is designed as its own
    reciprocal boost and transformed, which is derived in `transformToCut`
    below and measured in `BellMatchTest` to mirror the boost exactly.

    **On the provenance of the equations.** The AES library copy is
    paywalled. These were transcribed from a third party scan and then
    validated against the paper's own published figures: a 12 dB boost with a
    bandwidth of 0.2 pi measured at 9 dB has a Nyquist gain of 2.053 dB at
    w0 = 0.3 pi, 2.725 dB at 0.5 pi and 4.420 dB at 0.7 pi. All three
    reproduce to six decimal places, as do the five design conditions on the
    realised filter, which is `BellMatchTest`'s first check and the reason to
    believe the rest of them.

    No JUCE.
*/
namespace Fx
{

/** What a design produces: the five numbers `Svf` runs on, and whether the
    match was possible at all. */
struct BellDesign
{
    double g  = 0.0;
    double k  = 1.0;
    double m0 = 1.0;
    double m1 = 0.0;
    double m2 = 0.0;

    /** False when the match was out of reach, in which case the numbers are
        left alone and the caller should keep its conventional design. There
        is no half matched state: this is the whole point of returning it
        rather than quietly producing something. */
    bool matched = false;
};

/** Whether the correction is available at a centre frequency and Q.

    Gain does not appear, and that is not an omission: with the bandwidth
    taken at half the decibel gain the feasibility condition cancels it
    exactly. Asserted over a grid in the test, because a future
    simplification of this predicate that happens to work on boosts is
    exactly how the NaN gets back in.
*/
inline bool bellIsMatchable (double hz, double q, double sampleRate) noexcept
{
    if (! (sampleRate > 0.0) || ! (q > 0.0) || ! (hz > 0.0))
        return false;

    const double boundary = 0.25 * sampleRate
                              * (std::sqrt (1.0 / (q * q) + 4.0) - 1.0 / q);

    return hz < boundary;
}

//==============================================================================
/** The paper's design, in the paper's own terms, so the test can reach it
    with the paper's own numbers.

    `w0` and `dw` are the centre frequency and the bandwidth in radians per
    sample, so `2 pi f / fs`, and `g`, `gb` and `g0` are the peak, bandedge
    and reference gains as linear amplitudes rather than decibels. The caller
    above picks `gb` and `g0`; the paper leaves both free and the worked
    example in section 6 uses a bandedge nine decibels up on a twelve decibel
    boost, which is neither of the conventions anybody ships. That is why this
    takes them rather than assuming them.

    Boosts only: `matchedBell` routes a cut through here and then inverts.
*/
inline BellDesign matchedBellBoost (double w0, double dw,
                                    double g, double gb, double g0) noexcept
{
    BellDesign out;

    constexpr double pi = 3.14159265358979323846;

    if (! (w0 > 0.0) || ! (w0 < pi) || ! (dw > 0.0))
        return out;

    const double gSquared = g * g;
    const double gbSquared = gb * gb;
    const double g0Squared = g0 * g0;

    if (! (gSquared > gbSquared) || ! (gbSquared > g0Squared))
        return out;

    // (21): the Nyquist gain to prescribe, which is the whole idea. It is
    // the analogue prototype's gain at half the sample rate, which is why
    // `pi` appears in it as a frequency rather than as a constant.
    const double r = (gbSquared - g0Squared) / (gSquared - gbSquared);

    const double offNyquist = (w0 * w0 - pi * pi) * (w0 * w0 - pi * pi);
    const double spread = pi * pi * dw * dw * r;

    const double g1Squared = (g0Squared * offNyquist + gSquared * spread)
                               / (offNyquist + spread);

    if (! (g1Squared > g0Squared) || ! (g1Squared < gbSquared))
        return out;                 // infeasible: see the header

    const double g1 = std::sqrt (g1Squared);

    // (16): the corner, already in the tangent domain, which is what lets it
    // be used as the state variable filter's `g` with nothing done to it.
    const double wSquared = std::sqrt ((gSquared - g1Squared)
                                         / (gSquared - g0Squared))
                              * std::tan (w0 * 0.5) * std::tan (w0 * 0.5);

    if (! (wSquared > 0.0))
        return out;

    // (19)
    const double deltaW = (1.0 + std::sqrt ((gbSquared - g0Squared)
                                              / (gbSquared - g1Squared)) * wSquared)
                            * std::tan (dw * 0.5);

    // (17)
    const double c = deltaW * deltaW * std::fabs (gbSquared - g1Squared)
                       - 2.0 * wSquared
                           * (std::fabs (gbSquared - g0 * g1)
                                - std::sqrt ((gbSquared - g0Squared)
                                               * (gbSquared - g1Squared)));

    const double d = 2.0 * wSquared
                       * (std::fabs (gSquared - g0 * g1)
                            - std::sqrt ((gSquared - g0Squared)
                                           * (gSquared - g1Squared)));

    const double aSquared = (c + d) / (gSquared - gbSquared);
    const double bSquared = (gSquared * c + gbSquared * d) / (gSquared - gbSquared);

    if (! (aSquared >= 0.0) || ! (bSquared >= 0.0))
        return out;

    const double w = std::sqrt (wSquared);
    const double a = std::sqrt (aSquared);
    const double b = std::sqrt (bSquared);

    out.g  = w;
    out.k  = a / w;
    out.m0 = g1;
    out.m1 = b / w - g1 * out.k;
    out.m2 = g0 - g1;

    // What the caller gets is five numbers that will be fed to a filter, so
    // the last word is that all five are numbers. Every return above is a
    // reason this cannot be reached; this is the one that catches a reason
    // nobody thought of.
    out.matched = std::isfinite (out.g) && std::isfinite (out.k)
                    && std::isfinite (out.m0) && std::isfinite (out.m1)
                    && std::isfinite (out.m2) && out.g > 0.0;

    if (! out.matched)
        out = BellDesign {};

    return out;
}

/** The reciprocal of a boost, as a cut, in the same five numbers.

    Derivation, since it is three lines and the alternative is believing it.
    The boost is

        H(s) = (G1 s^2 + B s + G0 W^2) / (s^2 + A s + W^2)

    so its reciprocal, with the denominator brought to the form the state
    variable filter wants, has corner `g' = W sqrt(G0/G1)` and damping
    `k' = B / (G1 g')`, and the numerator over that denominator reads off as
    `m0 = G0/G1`, `m1 = (A - B/G1) / (G1 g')` and `m2 = 1/G0 - 1/G1`.
*/
inline BellDesign transformToCut (const BellDesign& boost, double a, double b,
                                  double g0) noexcept
{
    BellDesign out;

    if (! boost.matched || ! (boost.m0 > 0.0) || ! (g0 > 0.0))
        return out;

    const double g1 = boost.m0;         // the prescribed Nyquist gain
    const double g = boost.g * std::sqrt (g0 / g1);

    if (! (g > 0.0))
        return out;

    out.g  = g;
    out.k  = b / (g1 * g);
    out.m0 = g0 / g1;
    out.m1 = (a - b / g1) / (g1 * g);
    out.m2 = 1.0 / g0 - 1.0 / g1;

    out.matched = std::isfinite (out.g) && std::isfinite (out.k)
                    && std::isfinite (out.m0) && std::isfinite (out.m1)
                    && std::isfinite (out.m2);

    if (! out.matched)
        out = BellDesign {};

    return out;
}

//==============================================================================
/** The one an equaliser band calls: a matched bell at a frequency, a Q and a
    gain in decibels, against a reference of unity.

    The bandwidth is `w0 / q`, and the bandedge gain is the square root of the
    peak gain, which is the half decibel point: a bell six decibels up is one
    octave wide between its two three decibel points if its Q says so. The
    half decibel convention rather than a fixed three decibels down, because a
    two decibel bell has no three decibel point at all, so a bandwidth defined
    there is undefined over most of the range anybody uses. `SvfTest` asserts
    that, since it is the fact that justifies the convention.
*/
inline BellDesign matchedBell (double hz, double q, double gainDb,
                               double sampleRate) noexcept
{
    constexpr double pi = 3.14159265358979323846;

    if (! bellIsMatchable (hz, q, sampleRate))
        return {};

    // Nothing to match: a flat bell is flat in both designs, and the algebra
    // above divides by the difference between the gains.
    if (std::fabs (gainDb) < 1.0e-9)
        return {};

    const double w0 = 2.0 * pi * hz / sampleRate;
    const double dw = w0 / q;

    const double g0 = 1.0;

    // A cut is designed as the boost it is the reciprocal of, which is what
    // keeps every square root above under a positive number. The boundary
    // does not move: it depends on neither the sign nor the size of the gain.
    const double boostDb = std::fabs (gainDb);

    const double g = std::pow (10.0, boostDb / 20.0);
    const double gb = std::sqrt (g0 * g);

    const auto boost = matchedBellBoost (w0, dw, g, gb, g0);

    if (! boost.matched || gainDb > 0.0)
        return boost;

    // `transformToCut` needs the paper's `A` and `B` rather than the filter's
    // `k` and `m1`, and they come straight back out: `k` is `A/W` and the
    // first moving weight is `B/W - G1 k`.
    const double a = boost.k * boost.g;
    const double b = (boost.m1 + boost.m0 * boost.k) * boost.g;

    return transformToCut (boost, a, b, g0);
}

} // namespace Fx
