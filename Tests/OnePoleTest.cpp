// Checks Fx/OnePole.h: the one pole section, which is what makes an odd order
// filter, a phaser and a six decibel per octave shelf possible at all.
//
// The header said it was verified here before this file existed. What covered
// it was three functions inside PassFilterTest.cpp, and they were weaker than
// they looked in one specific way: everything they asserted went through
// `magnitudeAt` and `phaseAt`, and the only thing that ever ran a sample
// through the section was the order one path of the cascade, which is the low
// pass and the high pass and nothing else.
//
// So this file measures. Every check here drives the running filter and reads
// the complex gain back out of its output, which is the only way to say
// anything about `phaseAt`: `magnitudeAt` carries a comment about a curve
// drawn from a second copy of the algebra being a second copy that can
// disagree, and `phaseAt` was a second copy that nothing had compared against
// a sample.
//
// Two things that matter about the measurement:
//
// **The test tones sit on exact bin centres of the window they are read with.**
// A window that does not close on a whole number of cycles smears each
// component across its neighbours, and at the bottom of a shelf's skirt that
// smear is larger than the thing being read. On bin centres the settled
// output is exactly periodic in the window, so a single point transform is
// exact and the whole of the response can be read in one pass.
//
// **The settling is measured rather than assumed.** A one pole's transient
// decays geometrically at a rate that depends on the corner and, for a shelf,
// on the gain as well: a boosted low shelf divides the coefficient by the
// square root of its gain, so sixteen decibels of boost is a transient six
// times longer. A fixed settling time is either wrong for the slow cases or
// wasteful for every other one, so this runs whole periods until one is the
// same as the last.
//
// The claims against closed forms are written from the specification of each
// response rather than from `update()`: a one pole shelf is `(A + jw)/(1 + jw)`
// with its half gain point at the corner, which fixes the prewarping, the two
// output coefficients and the recursion all at once, in phase as well as in
// magnitude. The one that earns its place most is the last: it pins the
// signature of the two pole prewarping, which is the obvious thing to write
// here and is wrong by three and a half decibels at eighteen decibels of
// boost.

#include "Fx/OnePole.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void fail (const std::string& what)
{
    std::printf ("  FAIL  %s\n", what.c_str());
    ++failures;
}

std::string show (double v)
{
    char buffer[40];
    std::snprintf (buffer, sizeof (buffer), "%.6g", v);
    return buffer;
}

void expectNear (double actual, double expected, double tolerance,
                 const std::string& what)
{
    if (! (std::fabs (actual - expected) <= tolerance))
        fail (what + ": expected " + show (expected) + ", got " + show (actual)
                   + ", off by " + show (std::fabs (actual - expected)));
}

constexpr double pi = 3.14159265358979323846;

double db (double x) { return 20.0 * std::log10 (x); }

using Complex = std::complex<double>;

const char* nameOf (Fx::OnePole::Type type)
{
    switch (type)
    {
        case Fx::OnePole::Type::lowpass:   return "low pass";
        case Fx::OnePole::Type::highpass:  return "high pass";
        case Fx::OnePole::Type::allpass:   return "allpass";
        case Fx::OnePole::Type::lowShelf:  return "low shelf";
        case Fx::OnePole::Type::highShelf: return "high shelf";
    }

    return "?";
}

//==============================================================================
/** The complex gain of the running filter at each of a set of frequencies,
    measured in one pass.

    The frequencies are bin centres of the window, so the settled output is
    periodic in it and the transform of one window is exact. The gain is the
    transform of the output over the transform of the input at the same bin,
    which needs no bookkeeping about amplitudes or starting phases: whatever
    went in, the ratio is the gain.
*/
class Probe
{
public:
    static constexpr int window = 1024;

    explicit Probe (const std::vector<int>& binsToRead)
        : bins (binsToRead)
    {
        for (int n = 0; n < window; ++n)
        {
            const double theta = 2.0 * pi * (double) n / (double) window;

            cosine.push_back (std::cos (theta));
            sine.push_back (std::sin (theta));
        }

        // One period of the input: every bin at once, each with a different
        // starting phase so the sum does not pile up into one tall spike that
        // a clamp or a saturation somewhere would read differently from the
        // rest of the waveform.
        period.assign (window, 0.0);

        for (std::size_t b = 0; b < bins.size(); ++b)
        {
            const double offset = 0.61803398875 * (double) b * 2.0 * pi;

            for (int n = 0; n < window; ++n)
                period[(std::size_t) n]
                    += std::sin (2.0 * pi * (double) (bins[b] * n)
                                   / (double) window + offset)
                         / (double) bins.size();
        }
    }

    double frequencyOf (std::size_t index, double rate) const
    {
        return (double) bins[index] * rate / (double) window;
    }

    /** Runs whole periods until the output stops moving, then reads one.
        Returns false if it never settled, which is a result in itself: an
        unstable section cannot settle. */
    bool run (Fx::OnePole& filter, std::vector<Complex>& gains,
              int mostPeriods = 4000)
    {
        filter.reset();

        std::vector<double> previous ((std::size_t) window, 0.0);
        std::vector<double> current ((std::size_t) window, 0.0);

        bool settled = false;

        for (int p = 0; p < mostPeriods && ! settled; ++p)
        {
            double worst = 0.0;

            for (int n = 0; n < window; ++n)
            {
                current[(std::size_t) n]
                    = filter.process (period[(std::size_t) n]);

                worst = std::max (worst, std::fabs (current[(std::size_t) n]
                                                      - previous[(std::size_t) n]));
            }

            if (! std::isfinite (worst))
                return false;

            settled = p > 0 && worst < 1.0e-15;
            previous.swap (current);
        }

        if (! settled)
            return false;

        // previous now holds the settled period, since the loop swapped.
        gains.assign (bins.size(), Complex (0.0, 0.0));

        for (std::size_t b = 0; b < bins.size(); ++b)
        {
            Complex in (0.0, 0.0), out (0.0, 0.0);

            for (int n = 0; n < window; ++n)
            {
                const int at = (bins[b] * n) % window;
                const Complex turn (cosine[(std::size_t) at],
                                    -sine[(std::size_t) at]);

                in += period[(std::size_t) n] * turn;
                out += previous[(std::size_t) n] * turn;
            }

            gains[b] = out / in;
        }

        return true;
    }

private:
    std::vector<int> bins;
    std::vector<double> cosine, sine;
    std::vector<double> period;
};

/** The response each type is supposed to have, as a function of the ratio of
    the prewarped frequency to the prewarped corner and of the linear gain.

    Written from the specification of the response rather than from the
    header's `update()`. A one pole shelf is `(A + jw)/(1 + jw)`, whose half
    gain point is at `w = sqrt(A)`; placing that at the corner is what fixes
    `w` to `x sqrt(A)` for a low shelf and `x / sqrt(A)` for a high one, and
    that is the whole of the prewarping claim.
*/
Complex specified (Fx::OnePole::Type type, double x, double gainDb)
{
    const double a = std::pow (10.0, gainDb / 20.0);
    const double root = std::sqrt (a);

    const Complex jx (0.0, x);

    switch (type)
    {
        case Fx::OnePole::Type::lowpass:   return 1.0 / (1.0 + jx);
        case Fx::OnePole::Type::highpass:  return jx / (1.0 + jx);
        case Fx::OnePole::Type::allpass:   return (jx - 1.0) / (1.0 + jx);

        case Fx::OnePole::Type::lowShelf:
            return (a + jx * root) / (1.0 + jx * root);

        case Fx::OnePole::Type::highShelf:
            return (1.0 + jx * root) / (1.0 + jx / root);
    }

    return Complex (1.0, 0.0);
}

double ratioAt (double at, double fc, double rate)
{
    return std::tan (pi * at / rate) / std::tan (pi * fc / rate);
}

//==============================================================================
/** The running filter against the specification of each response, in
    magnitude and in phase together, over a few hundred randomised settings.

    This is the check the file exists for. Everything the old coverage
    asserted could be satisfied by a curve drawing function that agreed with
    itself; nothing here can be satisfied by anything but a section that
    actually does to a sample what its name says.
*/
void theRunningFilterIsTheSpecifiedResponse()
{
    std::printf ("the running filter against the specification\n");

    const std::vector<int> bins { 1, 2, 3, 5, 9, 17, 33, 65, 129, 257, 401, 487 };

    Probe probe (bins);
    std::vector<Complex> gains;

    unsigned seed = 20261021u;
    const auto next = [&seed]
    {
        seed = seed * 1664525u + 1013904223u;

        return (double) ((seed >> 8) & 0xffffffu) / (double) 0x1000000u;
    };

    double worstRelative = 0.0, worstPhase = 0.0;
    std::string worstWhere;
    int measured = 0, unsettled = 0;

    for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        for (auto type : { Fx::OnePole::Type::lowpass, Fx::OnePole::Type::highpass,
                           Fx::OnePole::Type::allpass, Fx::OnePole::Type::lowShelf,
                           Fx::OnePole::Type::highShelf })
        {
            for (int trial = 0; trial < 12; ++trial)
            {
                // Away from the clamps, which have their own checks: here the
                // corner asked for is the corner used, so the closed form can
                // be written in terms of it.
                const double lowest = 3.0 * rate / (double) Probe::window;
                const double corner = lowest
                                        * std::pow (0.45 * rate / lowest, next());
                const double gainDb = type == Fx::OnePole::Type::lowShelf
                                        || type == Fx::OnePole::Type::highShelf
                                            ? -24.0 + 48.0 * next()
                                            : 0.0;

                Fx::OnePole filter;
                filter.setSampleRate (rate);
                filter.set (type, corner, gainDb);

                if (! probe.run (filter, gains))
                {
                    ++unsettled;
                    fail (std::string ("the ") + nameOf (type)
                            + " never settled at " + show (corner) + " Hz, "
                            + show (rate) + " Hz, " + show (gainDb) + " dB");
                    continue;
                }

                for (std::size_t b = 0; b < bins.size(); ++b)
                {
                    const double at = probe.frequencyOf (b, rate);
                    const Complex want = specified (type,
                                                    ratioAt (at, corner, rate),
                                                    gainDb);

                    const double relative = std::abs (gains[b] / want - 1.0);
                    const double phase = std::fabs (std::arg (gains[b] / want));

                    if (relative > worstRelative)
                    {
                        worstRelative = relative;
                        worstWhere = std::string (nameOf (type)) + " at "
                                       + show (at) + " Hz, corner "
                                       + show (corner) + " Hz, rate "
                                       + show (rate) + " Hz, gain "
                                       + show (gainDb) + " dB";
                    }

                    worstPhase = std::max (worstPhase, phase);
                    ++measured;
                }
            }
        }
    }

    std::printf ("    %d readings over 240 settings: worst off by %s relative,"
                 " %s radians of phase\n", measured, show (worstRelative).c_str(),
                 show (worstPhase).c_str());

    if (! worstWhere.empty() && worstRelative > 1.0e-10)
        std::printf ("    worst: %s\n", worstWhere.c_str());

    // The reading is an exact transform of an exactly periodic steady state,
    // so what is left is rounding in the recursion. A tolerance loose enough
    // to let a sign through would make the whole file decorative.
    if (worstRelative > 1.0e-10)
        fail ("the running filter is not its specified response: off by "
              + show (worstRelative));

    if (unsettled > 0)
        fail (show ((double) unsettled) + " setting(s) never settled");
}

/** The analytic curves against the same measurement.

    `magnitudeAt` had been checked against the running filter for the low pass
    and the high pass, through the cascade. `phaseAt` had been checked against
    nothing that runs, and the check it did have took the modulus of it, which
    is exactly what throws away the one thing the allpass exists for: negating
    the whole of `phaseAt` passed.
*/
void theCurvesAreWhatTheFilterDoes()
{
    std::printf ("the drawn curves against the running filter\n");

    const std::vector<int> bins { 1, 2, 5, 11, 23, 47, 97, 193, 331, 457 };

    Probe probe (bins);
    std::vector<Complex> gains;

    double worstMagnitude = 0.0, worstPhase = 0.0;
    std::string phaseWhere;

    for (double rate : { 44100.0, 48000.0, 192000.0 })
    {
        for (auto type : { Fx::OnePole::Type::lowpass, Fx::OnePole::Type::highpass,
                           Fx::OnePole::Type::allpass, Fx::OnePole::Type::lowShelf,
                           Fx::OnePole::Type::highShelf })
        {
            for (double gainDb : { -18.0, 0.0, 12.0 })
            {
                for (double corner : { 120.0, 1000.0, 0.3 * rate })
                {
                    Fx::OnePole filter;
                    filter.setSampleRate (rate);
                    filter.set (type, corner, gainDb);

                    if (! probe.run (filter, gains))
                    {
                        fail ("a section never settled while checking the curves");
                        continue;
                    }

                    for (std::size_t b = 0; b < bins.size(); ++b)
                    {
                        const double at = probe.frequencyOf (b, rate);

                        worstMagnitude
                            = std::max (worstMagnitude,
                                        std::fabs (db (std::abs (gains[b]))
                                                     - db (filter.magnitudeAt (at))));

                        // Compared as a turn rather than as two numbers, so
                        // that a phase either side of half a cycle is not
                        // read as a whole cycle of disagreement.
                        const double turn
                            = std::abs (std::arg (gains[b]
                                                    * std::polar (1.0,
                                                                  -filter.phaseAt (at))));

                        if (turn > worstPhase)
                        {
                            worstPhase = turn;
                            phaseWhere = std::string (nameOf (type)) + " at "
                                           + show (at) + " Hz";
                        }
                    }
                }
            }
        }
    }

    std::printf ("    worst %s dB of magnitude and %s radians of phase\n",
                 show (worstMagnitude).c_str(), show (worstPhase).c_str());

    if (worstMagnitude > 1.0e-9)
        fail ("magnitudeAt is not what the filter does: off by "
              + show (worstMagnitude) + " dB");

    if (worstPhase > 1.0e-9)
        fail ("phaseAt is not what the filter does: off by " + show (worstPhase)
              + " radians, worst at " + phaseWhere);
}

/** The allpass turns the phase the way round the header says, which is the
    thing a phaser's notch positions come out of, and which nothing could see
    while the assertion took the modulus.

    Measured on the output, not read off the curve. Smith's AP1 is positive
    and falling: half a cycle at DC, a quarter at the corner, nothing at
    Nyquist. The other sign is also an allpass, is also flat, and puts the
    notches somewhere else.
*/
void theAllpassTurnsThePhaseTheRightWay()
{
    std::printf ("the direction of the allpass phase\n");

    const double rate = 48000.0;
    const std::vector<int> bins { 1, 4, 21, 64, 128, 256, 480 };

    Probe probe (bins);
    std::vector<Complex> gains;

    // The corner on a bin centre, so the quarter cycle can be read directly.
    const double corner = 64.0 * rate / (double) Probe::window;

    Fx::OnePole all;
    all.setSampleRate (rate);
    all.set (Fx::OnePole::Type::allpass, corner);

    if (! probe.run (all, gains))
    {
        fail ("the allpass never settled");
        return;
    }

    double worstFlat = 0.0;
    double atCorner = 0.0;
    bool falling = true;
    double last = 4.0;

    for (std::size_t b = 0; b < bins.size(); ++b)
    {
        worstFlat = std::max (worstFlat, std::fabs (std::abs (gains[b]) - 1.0));

        const double phase = std::arg (gains[b]);

        if (probe.frequencyOf (b, rate) == corner)
            atCorner = phase;

        // Positive and falling all the way across the band. A negated phase
        // is negative and rising, and the modulus of both is the same.
        if (phase < 0.0 || phase > last)
            falling = false;

        last = phase;
    }

    std::printf ("    flat to %s, phase at the corner %s pi, at the bottom"
                 " %s pi, at the top %s pi\n", show (worstFlat).c_str(),
                 show (atCorner / pi).c_str(),
                 show (std::arg (gains.front()) / pi).c_str(),
                 show (std::arg (gains.back()) / pi).c_str());

    if (worstFlat > 1.0e-12)
        fail ("the running allpass is not flat: off by " + show (worstFlat));

    expectNear (atCorner, pi * 0.5, 1.0e-9,
                "the allpass does not turn a positive quarter cycle at its"
                " corner");

    if (! falling)
        fail ("the allpass phase is not positive and falling across the band,"
              " which is the other sign convention and moves a phaser's"
              " notches");

    // The ends, in the direction as well as the amount. Near DC it is close
    // enough to half a cycle to be read, and near Nyquist close to nothing.
    if (! (std::arg (gains.front()) > 0.99 * pi))
        fail ("the allpass does not approach a positive half cycle at the"
              " bottom of the band: got "
              + show (std::arg (gains.front()) / pi) + " pi");

    if (! (std::arg (gains.back()) > 0.0 && std::arg (gains.back()) < 0.06 * pi))
        fail ("the allpass does not come back towards nothing at the top of"
              " the band: got " + show (std::arg (gains.back()) / pi) + " pi");
}

/** A shelf's corner is at half its gain, and the two pole prewarping has a
    signature so it cannot quietly replace the one pole one.

    Dividing the coefficient by the fourth root of the gain rather than the
    square root is the obvious thing to write, because it is what every two
    pole shelf in the project does. The shelf stays a shelf and its two ends
    stay its two gains: all that moves is the corner, which is the number on
    the control.
*/
void theWrongPrewarpHasASignature()
{
    std::printf ("the shelf corner, and what the two pole prewarping would"
                 " read\n");

    const double rate = 48000.0;
    const std::vector<int> bins { 1, 64, 511 };

    Probe probe (bins);
    std::vector<Complex> gains;

    const double corner = 64.0 * rate / (double) Probe::window;

    for (double gainDb : { -18.0, -6.0, 6.0, 18.0 })
    {
        for (auto type : { Fx::OnePole::Type::lowShelf,
                           Fx::OnePole::Type::highShelf })
        {
            Fx::OnePole shelf;
            shelf.setSampleRate (rate);
            shelf.set (type, corner, gainDb);

            if (! probe.run (shelf, gains))
            {
                fail ("a shelf never settled");
                continue;
            }

            const bool isLow = type == Fx::OnePole::Type::lowShelf;
            const double at = db (std::abs (gains[1]));

            expectNear (at, gainDb * 0.5, 0.001,
                        std::string (isLow ? "a low" : "a high")
                          + " shelf's corner is not at half its gain at "
                          + show (gainDb) + " dB");

            // What the fourth root would have read there. The low shelf's
            // half gain point is at w = sqrt(A), so prewarping by A^(1/4)
            // leaves w = A^(1/4) at the corner instead.
            const double a = std::pow (10.0, gainDb / 20.0);
            const double w = isLow ? std::pow (a, 0.25) : std::pow (a, -0.25);
            const double wrong = isLow
                                   ? db (std::abs ((a + Complex (0.0, w))
                                                     / (1.0 + Complex (0.0, w))))
                                   : db (std::abs ((1.0 + Complex (0.0, a * w))
                                                     / (1.0 + Complex (0.0, w))));

            // The signature grows with the gain: half a decibel at six, three
            // and a third at eighteen. The bar is what the smallest of them
            // has to clear, and the corner above is asserted five hundred
            // times tighter than that.
            if (std::fabs (at - wrong) < 0.2)
                fail (std::string (isLow ? "a low" : "a high")
                        + " shelf at " + show (gainDb)
                        + " dB cannot tell the one pole prewarping from the two"
                          " pole one");

            if (gainDb == 18.0)
                std::printf ("    %s at +18 dB: corner reads %s dB, the fourth"
                             " root would read %s\n", nameOf (type),
                             show (at).c_str(), show (wrong).c_str());

            // And the ends are still the two gains, so a wrong prewarp cannot
            // be mistaken for a wrong gain.
            expectNear (db (std::abs (gains.front())), isLow ? gainDb : 0.0, 0.05,
                        std::string (nameOf (type)) + "'s bottom end is wrong at "
                          + show (gainDb) + " dB");
            expectNear (db (std::abs (gains.back())), isLow ? 0.0 : gainDb, 0.05,
                        std::string (nameOf (type)) + "'s top end is wrong at "
                          + show (gainDb) + " dB");
        }
    }
}

/** The low pass and the high pass are one signal split in two, measured on
    the two running filters rather than asserted about their closed forms.

    The old check computed `1/(1+s) + s/(1+s)` and compared it against one,
    which is an identity in `s` and true however the filter behaves: it never
    touched either section.
*/
void thePairAddsBackUpToTheInput()
{
    std::printf ("the complementary pair, on the running sections\n");

    double worst = 0.0;

    for (double rate : { 44100.0, 48000.0, 192000.0 })
    {
        for (double corner : { 20.0, 500.0, 7000.0, 0.45 * rate })
        {
            Fx::OnePole low, high;

            low.setSampleRate (rate);
            high.setSampleRate (rate);
            low.set (Fx::OnePole::Type::lowpass, corner);
            high.set (Fx::OnePole::Type::highpass, corner);

            unsigned seed = 7u;

            for (int i = 0; i < 4000; ++i)
            {
                seed = seed * 1664525u + 1013904223u;

                const double in = (double) ((seed >> 8) & 0xffffu)
                                    / (double) 0x8000u - 1.0;

                worst = std::max (worst, std::fabs (low.process (in)
                                                      + high.process (in) - in));
            }
        }
    }

    std::printf ("    the two halves add back up to the input within %s\n",
                 show (worst).c_str());

    // One rounding of a subtraction and one of an addition, and nothing else:
    // the high pass is the input with the low pass taken off it.
    if (worst > 1.0e-15)
        fail ("the pair is not complementary on the running filters: off by "
              + show (worst));
}

/** Moving the corner under a steady signal does not move the output, which is
    the whole of the claim that this is a topology preserving structure and the
    reason a swept corner does not click.

    The state of a trapezoidal integrator is the integrator's own output, which
    does not depend on the coefficient. So with the input held still the state
    is the input, the difference driving the integrator is nothing, and the
    corner can be put anywhere at all without the output moving by a bit. A
    structure whose state means something coefficient shaped passes every
    frequency response check in this file and clicks on every sweep.
*/
void aSweptCornerDoesNotClick()
{
    std::printf ("sweeping the corner under a steady signal\n");

    const double rate = 48000.0;
    double worst = 0.0;

    for (auto type : { Fx::OnePole::Type::lowpass, Fx::OnePole::Type::highpass,
                       Fx::OnePole::Type::allpass, Fx::OnePole::Type::lowShelf,
                       Fx::OnePole::Type::highShelf })
    {
        Fx::OnePole filter;
        filter.setSampleRate (rate);
        filter.set (type, 1000.0, 12.0);

        double out = 0.0;

        for (int i = 0; i < 400000; ++i)
            out = filter.process (1.0);

        const double settled = out;

        // Right across the band, in both directions, and through the clamps
        // at both ends.
        for (int i = 0; i <= 2000; ++i)
        {
            const double sweep = 0.5 + (double) i * 0.0005;
            const double corner = 20.0 * std::pow (1200.0, sweep > 1.0
                                                             ? 2.0 - sweep
                                                             : sweep);

            filter.set (type, corner, 12.0);

            // Relative to the level being held, because a shelf boosting by
            // twelve decibels settles at four and the rounding in the state
            // is proportional to that rather than to one.
            worst = std::max (worst, std::fabs (filter.process (1.0) - settled)
                                       / std::max (1.0, std::fabs (settled)));
        }
    }

    std::printf ("    the output moves by %s of its own level across a sweep of"
                 " the corner\n", show (worst).c_str());

    // A click is a step of a sizeable fraction of the signal. This is three
    // orders of magnitude below anything audible and still only rounding.
    if (worst > 1.0e-13)
        fail ("sweeping the corner moves the output by " + show (worst)
              + " of its level, which is a click");
}

/** Every setting the section can be put into is stable, including the ones
    that arrive by the back door.

    The ceiling on the corner is what keeps the pole inside the unit circle:
    the tangent runs away at Nyquist, and a coefficient of one puts the pole
    exactly on minus one, where an impulse rings for ever instead of decaying.
    The back door is a corner set at one rate and then read at a lower one,
    where the tangent of a frequency past Nyquist comes out negative, the
    coefficient with it, and the pole lands outside the circle altogether.
*/
void everySettingIsStable()
{
    std::printf ("stability at every setting, including after a rate change\n");

    // The statement is that the pole is inside the unit circle, and the way
    // to read that off an impulse response without caring how fast the pole
    // is, is that the envelope shrinks from one window to the next. A corner
    // of one hertz at 192 kHz is a perfectly stable section whose response is
    // still a thirtieth of its peak after a third of a million samples, so a
    // fixed floor by some sample number would fail it; a corner left at
    // Nyquist rings at exactly constant amplitude for ever, and only a ratio
    // sees that.
    const auto decays = [] (Fx::OnePole& filter, const std::string& what)
    {
        filter.reset();

        constexpr int windows = 20;
        constexpr int length = 20000;

        double peak[windows] = {};

        for (int w = 0; w < windows; ++w)
        {
            for (int i = 0; i < length; ++i)
            {
                const double out
                    = filter.process (w == 0 && i == 0 ? 1.0 : 0.0);

                if (! std::isfinite (out))
                {
                    fail (what + ": the impulse response stopped being finite");

                    return;
                }

                peak[w] = std::max (peak[w], std::fabs (out));
            }
        }

        for (int w = 1; w < windows; ++w)
        {
            // Once it has underflowed there is nothing left to shrink.
            if (peak[w] < 1.0e-250)
                break;

            if (! (peak[w] < peak[w - 1] * 0.999))
            {
                fail (what + ": the impulse response stops decaying, window "
                           + show ((double) w) + " peaks at " + show (peak[w])
                           + " against " + show (peak[w - 1]) + " before it");

                return;
            }
        }
    };

    for (double rate : { 8000.0, 44100.0, 192000.0 })
    {
        for (auto type : { Fx::OnePole::Type::lowpass, Fx::OnePole::Type::allpass,
                           Fx::OnePole::Type::lowShelf,
                           Fx::OnePole::Type::highShelf })
        {
            for (double corner : { 1.0e-9, 1.0, 1000.0, 0.49 * rate,
                                   0.5 * rate, rate, 1.0e9 })
            {
                Fx::OnePole filter;
                filter.setSampleRate (rate);
                filter.set (type, corner, 18.0);

                decays (filter, std::string (nameOf (type)) + " at "
                                  + show (corner) + " Hz, " + show (rate)
                                  + " Hz");
            }
        }
    }

    // The back door: set high at a high rate, then moved to a low one. The
    // corner has to be re-read against the rate that is current, or the
    // tangent of a frequency past the new Nyquist is negative and the section
    // runs away with no warning at all.
    for (auto type : { Fx::OnePole::Type::lowpass, Fx::OnePole::Type::highpass,
                       Fx::OnePole::Type::allpass, Fx::OnePole::Type::lowShelf,
                       Fx::OnePole::Type::highShelf })
    {
        for (double was : { 96000.0, 192000.0 })
        {
            for (double now : { 8000.0, 22050.0, 44100.0, 48000.0 })
            {
                Fx::OnePole filter;
                filter.setSampleRate (was);
                filter.set (type, 0.45 * was, 12.0);
                filter.setSampleRate (now);

                decays (filter, std::string (nameOf (type)) + " set at "
                                  + show (was) + " Hz and moved to "
                                  + show (now) + " Hz");
            }
        }
    }

    std::printf ("    every corner at every rate decays, and so does one left"
                 " above the new Nyquist by a rate change\n");
}

/** The corner lands where it was asked for, everywhere the header says it
    will, and is clamped rather than refused above that.

    The ceiling is a real part of the specification: anything up to 0.49 of
    the rate has to be honoured, because an equaliser's top band is up there,
    and a ceiling quietly brought down would move it. */
void theCornerLandsWhereItWasAsked()
{
    std::printf ("the corner against the frequency asked for\n");

    double worst = 0.0;

    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        for (double corner : { 1.0, 200.0, 5000.0, 0.3 * rate, 0.45 * rate,
                               0.489 * rate })
        {
            Fx::OnePole low;
            low.setSampleRate (rate);
            low.set (Fx::OnePole::Type::lowpass, corner);

            worst = std::max (worst,
                              std::fabs (db (low.magnitudeAt (corner)) + 3.0103));
        }
    }

    std::printf ("    three decibels down at the corner within %s dB, from one"
                 " hertz to 0.489 of the rate\n", show (worst).c_str());

    if (worst > 0.002)
        fail ("the corner is not where it was asked for: off by " + show (worst)
              + " dB");

    // And a corner asked for past the ceiling is clamped to it rather than
    // being allowed to take the tangent with it.
    for (double rate : { 44100.0, 192000.0 })
    {
        Fx::OnePole a, b;

        a.setSampleRate (rate);
        b.setSampleRate (rate);
        a.set (Fx::OnePole::Type::lowpass, 1.0e9);
        b.set (Fx::OnePole::Type::lowpass, 0.49 * rate);

        expectNear (db (a.magnitudeAt (0.25 * rate)),
                    db (b.magnitudeAt (0.25 * rate)), 1.0e-9,
                    "a corner past the ceiling is not clamped to the ceiling");
    }
}

/** A section nobody has configured still behaves like the filter its own
    defaults describe, and its curve agrees with it.

    The members say a one kilohertz low pass at 44.1 kHz. Before the
    coefficients were computed up front they said that and the section
    returned silence, while `magnitudeAt` returned one: the degenerate guard
    in the curve made an unconfigured filter look flat and it was mute. Both
    answers were about a filter nobody had asked for and they did not agree
    with each other, which is the harder kind of wrong to find.
*/
void anUnconfiguredSectionIsItsOwnDefaults()
{
    std::printf ("a section nobody has configured\n");

    Fx::OnePole fresh;
    Fx::OnePole asked;

    asked.setSampleRate (44100.0);
    asked.set (Fx::OnePole::Type::lowpass, 1000.0);

    expectNear (fresh.getSampleRate(), 44100.0, 0.0,
                "an unconfigured section's rate is not its default");

    double worst = 0.0;

    for (double at : { 10.0, 100.0, 1000.0, 10000.0, 20000.0 })
        worst = std::max (worst, std::fabs (db (fresh.magnitudeAt (at))
                                              - db (asked.magnitudeAt (at))));

    if (worst > 1.0e-12)
        fail ("an unconfigured section does not draw the curve of its own"
              " defaults: off by " + show (worst) + " dB");

    double worstSample = 0.0;

    for (int i = 0; i < 4000; ++i)
    {
        const double in = std::sin (2.0 * pi * 300.0 * (double) i / 44100.0);

        worstSample = std::max (worstSample,
                                std::fabs (fresh.process (in)
                                             - asked.process (in)));
    }

    std::printf ("    draws its defaults within %s dB and runs them within"
                 " %s\n", show (worst).c_str(), show (worstSample).c_str());

    if (worstSample > 1.0e-15)
        fail ("an unconfigured section does not run as its own defaults: off by "
              + show (worstSample));

    // And it is not silent, which is what it used to be.
    expectNear (db (fresh.magnitudeAt (10.0)), 0.0, 0.01,
                "an unconfigured section does not pass the bottom of the band");
}

/** reset puts it back where it started and nothing else does. */
void resetIsExact()
{
    std::printf ("reset\n");

    Fx::OnePole filter;
    filter.setSampleRate (48000.0);
    filter.set (Fx::OnePole::Type::lowShelf, 800.0, -9.0);

    std::vector<double> first;

    const auto drive = [&filter] (std::vector<double>& into)
    {
        into.clear();

        for (int i = 0; i < 2000; ++i)
            into.push_back (filter.process (std::sin (0.01 * (double) i)
                                              + ((i % 11) == 0 ? 0.4 : 0.0)));
    };

    drive (first);

    std::vector<double> again;
    filter.reset();
    drive (again);

    double worst = 0.0;

    for (std::size_t i = 0; i < first.size(); ++i)
        worst = std::max (worst, std::fabs (first[i] - again[i]));

    std::printf ("    the same input after a reset gives the same output"
                 " within %s\n", show (worst).c_str());

    if (worst != 0.0)
        fail ("reset does not put the section back where it started: off by "
              + show (worst));
}

/** Nonsense in every argument, at every type, stays finite and bounded.

    None of these is reachable through `PassFilter`, which clamps before it,
    so none of them was covered: the clamps in this header could all be
    removed without any test noticing.
*/
void nonsenseStaysFinite()
{
    std::printf ("out of range use\n");

    for (double rate : { 0.0, -1.0, 1.0, 8000.0, 192000.0, 1.0e9 })
    {
        for (auto type : { Fx::OnePole::Type::lowpass, Fx::OnePole::Type::highpass,
                           Fx::OnePole::Type::allpass, Fx::OnePole::Type::lowShelf,
                           Fx::OnePole::Type::highShelf })
        {
            for (double hz : { -1.0e9, -1.0, 0.0, 1.0e-12, 1.0, 1.0e9 })
            {
                for (double gainDb : { -1.0e6, -120.0, 0.0, 120.0, 1.0e6 })
                {
                    Fx::OnePole filter;
                    filter.setSampleRate (rate);
                    filter.set (type, hz, gainDb);
                    filter.reset();

                    const std::string what = std::string (nameOf (type)) + " at "
                                               + show (hz) + " Hz, " + show (rate)
                                               + " Hz, " + show (gainDb) + " dB";

                    for (int i = 0; i < 2000; ++i)
                    {
                        const double out
                            = filter.process ((double) (i % 7) * 0.3 - 1.0);

                        if (! std::isfinite (out))
                        {
                            fail (what + ": the output stopped being finite");

                            return;
                        }

                        // A one pole cannot resonate, so no setting of it has
                        // any business multiplying a signal of one by more
                        // than its own gain.
                        if (std::fabs (out) > 1.0e7)
                        {
                            fail (what + ": the output reached " + show (out));

                            return;
                        }
                    }

                    if (! std::isfinite (filter.magnitudeAt (1000.0))
                         || ! std::isfinite (filter.phaseAt (1000.0)))
                    {
                        fail (what + ": the curve stopped being finite");

                        return;
                    }

                    if (! (filter.magnitudeAt (-1000.0) >= 0.0))
                    {
                        fail (what + ": a negative frequency gives a negative"
                                     " magnitude");

                        return;
                    }

                    // The corner of a pass is three decibels down, whatever
                    // was asked for, and this is what says the degenerate
                    // branch at the top of `magnitudeAt` is never taken: that
                    // branch returns exactly one, and one is not three
                    // decibels down. So the branch is a backstop against a
                    // coefficient outside nought to one, and no setting of
                    // any of these three arguments produces one.
                    //
                    // Worked out here rather than read back, because the
                    // whole question is whether the clamping inside arrives
                    // at a corner that is usable: the floor first, then the
                    // ceiling, against the rate the filter settled on.
                    const double on = filter.getSampleRate();
                    const double corner = std::fmin (std::fmax (hz, 1.0),
                                                     0.49 * on);

                    if (type == Fx::OnePole::Type::lowpass
                         || type == Fx::OnePole::Type::highpass)
                    {
                        expectNear (db (filter.magnitudeAt (corner)), -3.0103,
                                    0.002, what + ": the corner is not three"
                                                  " decibels down");

                        // And it goes the right way from there. Not strictly:
                        // a corner at the one hertz floor against a rate of
                        // a gigahertz puts the far end of the band within
                        // 5e-13 of unity, which is the same double.
                        const bool rising = type == Fx::OnePole::Type::highpass;
                        double previous = rising ? -1.0 : 2.0;

                        for (double part : { 0.001, 0.01, 0.1, 0.3, 0.49 })
                        {
                            const double now = filter.magnitudeAt (part * on);

                            if (rising ? now < previous : now > previous)
                            {
                                fail (what + ": the curve turns back on itself,"
                                             " " + show (now) + " at "
                                           + show (part) + " of the rate"
                                             " against " + show (previous)
                                           + " below it");

                                return;
                            }

                            previous = now;
                        }
                    }

                    // And an allpass is flat at every setting, which is the
                    // one thing it must never stop being.
                    if (type == Fx::OnePole::Type::allpass)
                        for (double part : { 0.001, 0.05, 0.25, 0.49 })
                            if (std::fabs (filter.magnitudeAt (part * on) - 1.0)
                                  > 1.0e-15)
                            {
                                fail (what + ": the allpass is not flat at "
                                           + show (part) + " of the rate");

                                return;
                            }
                }
            }
        }
    }

    std::printf ("    survives nonsense in the rate, the corner and the gain at"
                 " every type\n");
}

} // namespace

int main()
{
    std::printf ("Fx::OnePole\n\n");

    theRunningFilterIsTheSpecifiedResponse();
    theCurvesAreWhatTheFilterDoes();
    theAllpassTurnsThePhaseTheRightWay();
    theWrongPrewarpHasASignature();
    thePairAddsBackUpToTheInput();
    aSweptCornerDoesNotClick();
    everySettingIsStable();
    theCornerLandsWhereItWasAsked();
    anUnconfiguredSectionIsItsOwnDefaults();
    resetIsExact();
    nonsenseStaysFinite();

    std::printf ("\n");

    if (failures > 0)
    {
        std::printf ("%d check(s) failed.\n", failures);
        return 1;
    }

    std::printf ("All checks passed.\n");
    return 0;
}
