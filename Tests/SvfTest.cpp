// Checks Fx/Svf.h against what each of its responses is actually supposed to
// do, rather than against the coefficients it was written from.
//
// The point of doing it this way: the mixing weights that turn one filter
// structure into nine responses are six numbers, and getting one wrong
// produces a filter that still works, still sounds like a filter, and is the
// wrong one. A bell whose boost is a decibel short, a shelf whose far end is
// not quite flat, an allpass with a dip in it. None of that announces itself,
// and all of it shows up the moment somebody compares a mix against the same
// move made in another studio.
//
// So nothing here asserts a coefficient. The filter's exact transfer function
// is computed from its state space form, and then each response is asked to
// prove what it claims: a lowpass passes DC and stops Nyquist, a bell reaches
// its stated gain at its centre and is flat at both ends, a shelf's two ends
// are its two gains, an allpass is flat everywhere. Those are the properties
// a musician would notice, and a weight typed wrong cannot satisfy them.
//
// Exact, not measured. Running an impulse through and taking a DFT truncates
// an infinite impulse response, and at Q of 40 the truncation error is larger
// than the error being looked for. The state space form has no such problem:
//
//     s[n+1] = A s[n] + B u[n]
//     y[n]   = C s[n] + D u[n]
//     H(z)   = C (zI - A)^-1 B + D
//
// A, B, C and D are read straight off the filter's own update equations, in
// stateSpace() below, so the thing under test is the filter's arithmetic and
// not a second copy of it.

#include "Fx/Svf.h"

#include <algorithm>
#include <complex>
#include <cstdio>
#include <cmath>
#include <limits>
#include <random>
#include <string>

namespace
{

int failures = 0;

void fail (const std::string& what)
{
    std::printf ("  FAIL  %s\n", what.c_str());
    ++failures;
}

/** std::to_string gives six decimals, which prints every interesting number
    here as 0.000000. These are tolerances near the floating point floor, so
    the exponent is the part worth reading. */
std::string show (double v)
{
    char buffer[32];
    std::snprintf (buffer, sizeof (buffer), "%.6e", v);
    return buffer;
}

void expectNear (double actual, double expected, double tolerance, const std::string& what)
{
    if (! (std::fabs (actual - expected) <= tolerance))
        fail (what + ": expected " + show (expected)
                   + ", got " + show (actual)
                   + ", off by " + show (std::fabs (actual - expected))
                   + " (tolerance " + show (tolerance) + ")");
}

//==============================================================================
using Complex = std::complex<double>;

constexpr double pi = 3.14159265358979323846;

struct StateSpace
{
    double a11, a12, a21, a22;
    double b1, b2;
    double c1, c2;
    double d;
};

/** The filter's update equations rearranged as a linear system in its two
    integrator states. Nothing is approximated here: this is the same
    arithmetic Svf::process does, written as matrices.

        v3 = u - s2
        v1 = a1 s1 + a2 v3  =  a1 s1 - a2 s2 + a2 u
        v2 = s2 + a2 s1 + a3 v3  =  a2 s1 + (1 - a3) s2 + a3 u
        s1 <- 2 v1 - s1,   s2 <- 2 v2 - s2
        y  = m0 u + m1 v1 + m2 v2
*/
StateSpace stateSpace (const Fx::Svf::Coefficients& c)
{
    StateSpace s;

    s.a11 = 2.0 * c.a1 - 1.0;
    s.a12 = -2.0 * c.a2;
    s.a21 = 2.0 * c.a2;
    s.a22 = 1.0 - 2.0 * c.a3;

    s.b1 = 2.0 * c.a2;
    s.b2 = 2.0 * c.a3;

    s.c1 = c.m1 * c.a1 + c.m2 * c.a2;
    s.c2 = -c.m1 * c.a2 + c.m2 * (1.0 - c.a3);

    s.d = c.m0 + c.m1 * c.a2 + c.m2 * c.a3;

    return s;
}

/** H(z) at the digital frequency `hz`, exactly. */
Complex response (const Fx::Svf& filter, double hz, double sampleRate)
{
    const auto s = stateSpace (filter.coefficients());
    const Complex z = std::polar (1.0, 2.0 * pi * hz / sampleRate);

    // (zI - A)^-1 for a two by two, written out.
    const Complex det = (z - s.a11) * (z - s.a22) - s.a12 * s.a21;

    const Complex top    = (z - s.a22) * s.b1 + s.a12 * s.b2;
    const Complex bottom = s.a21 * s.b1 + (z - s.a11) * s.b2;

    return (s.c1 * top + s.c2 * bottom) / det + s.d;
}

double magnitudeAt (const Fx::Svf& filter, double hz, double sampleRate)
{
    return std::abs (response (filter, hz, sampleRate));
}

/** Nyquist itself, where z is exactly -1. */
double magnitudeAtNyquist (const Fx::Svf& filter, double sampleRate)
{
    return magnitudeAt (filter, sampleRate * 0.5, sampleRate);
}

/** The modulus of the larger of the two poles, from the state transition
    matrix. Anything at or above one is a filter that grows without bound.

    Shared rather than written out at each use, for the reason `Svf.h` gives
    for keeping `magnitudeAt` in the filter: a second copy of this algebra is
    a second copy that can disagree, and the two places that want it are the
    random sweep over settings and the rate change checks, which is exactly
    the pair that has to agree. */
double largestPoleModulus (const Fx::Svf& filter)
{
    const auto s = stateSpace (filter.coefficients());

    const double trace = s.a11 + s.a22;
    const double det = s.a11 * s.a22 - s.a12 * s.a21;
    const double disc = trace * trace - 4.0 * det;

    if (disc >= 0.0)
    {
        const double root = std::sqrt (disc);
        return std::max (std::fabs ((trace + root) * 0.5),
                         std::fabs ((trace - root) * 0.5));
    }

    return std::sqrt (det);   // complex pair, modulus is sqrt(det)
}

/** Every response the filter has, including the matched bell, which nothing
    in this file reached before. */
const Fx::Svf::Type allTypes[] = {
    Fx::Svf::Type::lowpass, Fx::Svf::Type::highpass, Fx::Svf::Type::bandpass,
    Fx::Svf::Type::bandpassQ, Fx::Svf::Type::notch, Fx::Svf::Type::peak,
    Fx::Svf::Type::allpass, Fx::Svf::Type::bell,
    Fx::Svf::Type::lowShelf, Fx::Svf::Type::highShelf,
    Fx::Svf::Type::bellMatched
};

const char* nameOf (Fx::Svf::Type t)
{
    switch (t)
    {
        case Fx::Svf::Type::lowpass:     return "lowpass";
        case Fx::Svf::Type::highpass:    return "highpass";
        case Fx::Svf::Type::bandpass:    return "bandpass";
        case Fx::Svf::Type::bandpassQ:   return "bandpassQ";
        case Fx::Svf::Type::notch:       return "notch";
        case Fx::Svf::Type::peak:        return "peak";
        case Fx::Svf::Type::allpass:     return "allpass";
        case Fx::Svf::Type::bell:        return "bell";
        case Fx::Svf::Type::lowShelf:    return "lowShelf";
        case Fx::Svf::Type::highShelf:   return "highShelf";
        case Fx::Svf::Type::bellMatched: return "bellMatched";
    }

    return "?";
}

Fx::Svf make (double rate, Fx::Svf::Type type, double hz, double q, double gainDb = 0.0)
{
    Fx::Svf f;
    f.setSampleRate (rate);
    f.set (type, hz, q, gainDb);
    return f;
}

//==============================================================================
/** A lowpass is one at DC and nothing at Nyquist, and a highpass is the other
    way round. Both peak to Q at the cutoff, which is what resonance means.

    The Nyquist end is the one worth having: a direct form biquad designed by
    the bilinear transform does not reach zero there for every Q, and a two
    pole lowpass that still passes something at Nyquist is a filter whose
    sweep never quite closes. */
void lowpassAndHighpassEnds()
{
    std::printf ("lowpass and highpass ends\n");

    const double rates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };
    const double cutoffs[] = { 20.0, 100.0, 1000.0, 5000.0, 15000.0 };
    const double qs[] = { 0.5, 0.70710678, 2.0, 10.0, 40.0 };

    for (double rate : rates)
        for (double fc : cutoffs)
        {
            if (fc > rate * 0.4)
                continue;

            for (double q : qs)
            {
                const auto lp = make (rate, Fx::Svf::Type::lowpass, fc, q);
                const auto hp = make (rate, Fx::Svf::Type::highpass, fc, q);

                expectNear (magnitudeAt (lp, 0.0, rate), 1.0, 1e-12, "lowpass at DC");
                expectNear (magnitudeAtNyquist (lp, rate), 0.0, 1e-12, "lowpass at Nyquist");

                expectNear (magnitudeAt (hp, 0.0, rate), 0.0, 1e-12, "highpass at DC");
                expectNear (magnitudeAtNyquist (hp, rate), 1.0, 1e-12, "highpass at Nyquist");

                // The resonant peak sits at the cutoff and is Q tall. This is
                // the check that the tangent prewarping is really there: drop
                // it and this drifts, worst at the top of the range.
                expectNear (magnitudeAt (lp, fc, rate), q, q * 1e-9, "lowpass peak is Q");
                expectNear (magnitudeAt (hp, fc, rate), q, q * 1e-9, "highpass peak is Q");
            }
        }
}

/** The two bandpasses differ by exactly their peak gain, which is the whole
    reason both exist. */
void bandpassGains()
{
    std::printf ("bandpass gains\n");

    const double rate = 48000.0;

    for (double fc : { 50.0, 440.0, 3000.0, 12000.0 })
        for (double q : { 0.5, 1.0, 4.0, 20.0 })
        {
            const auto raw  = make (rate, Fx::Svf::Type::bandpassQ, fc, q);
            const auto unit = make (rate, Fx::Svf::Type::bandpass, fc, q);

            expectNear (magnitudeAt (raw, fc, rate), q, q * 1e-9, "bandpassQ peaks at Q");
            expectNear (magnitudeAt (unit, fc, rate), 1.0, 1e-9, "bandpass peaks at one");

            // Both ends stop, which is what makes it a bandpass rather than
            // a shelf with a bump in it.
            expectNear (magnitudeAt (raw, 0.0, rate), 0.0, 1e-12, "bandpass at DC");
            expectNear (magnitudeAtNyquist (raw, rate), 0.0, 1e-12, "bandpass at Nyquist");
        }
}

/** A notch passes both ends and removes the centre completely. Completely is
    the word that matters: a notch that only gets to -40 dB is a dip, and the
    hum it was reached for is still there. */
void notchIsADeepHole()
{
    std::printf ("notch\n");

    const double rate = 48000.0;

    for (double fc : { 50.0, 60.0, 1000.0, 9000.0 })
        for (double q : { 0.5, 2.0, 30.0 })
        {
            const auto n = make (rate, Fx::Svf::Type::notch, fc, q);

            expectNear (magnitudeAt (n, 0.0, rate), 1.0, 1e-12, "notch at DC");
            expectNear (magnitudeAtNyquist (n, rate), 1.0, 1e-12, "notch at Nyquist");

            // Not zero to the last bit, and it cannot be. The notch's zero
            // sits exactly on the unit circle, so evaluating H there is a
            // difference of two nearly equal complex numbers and the result
            // is double precision cancellation rather than the filter's own
            // depth: it measures about 1e-12 at Q of 30. That is -240 dB.
            // Asserting a depth instead of an exact zero says what is really
            // being claimed, which is that the notch removes the frequency
            // rather than dipping it.
            const double depth = magnitudeAt (n, fc, rate);

            if (! (depth < 1e-10))
                fail ("notch is only " + show (20.0 * std::log10 (depth + 1e-300))
                      + " dB deep at its centre");
        }
}

/** An allpass changes phase and nothing else. Flat is flat: if the magnitude
    moves at all it is not an allpass, and a phase rotator with a tilt in it
    is a thing that quietly changes a mix. */
void allpassIsFlat()
{
    std::printf ("allpass is flat everywhere\n");

    const double rate = 48000.0;

    for (double fc : { 30.0, 500.0, 7000.0, 20000.0 })
        for (double q : { 0.1, 0.70710678, 5.0, 25.0 })
        {
            const auto ap = make (rate, Fx::Svf::Type::allpass, fc, q);

            for (double hz = 0.0; hz <= rate * 0.5; hz += rate * 0.5 / 257.0)
                expectNear (magnitudeAt (ap, hz, rate), 1.0, 1e-9,
                            "allpass magnitude at " + std::to_string (hz));
        }
}

/** A bell reaches the gain it was asked for, at the frequency it was asked
    for, and leaves both ends alone.

    Both ends alone is the half that gets missed. A bell that lifts DC along
    with its band is a bell that moves the whole bottom of a mix every time
    somebody touches a midrange band. */
void bellReachesItsGain()
{
    std::printf ("bell gain and skirts\n");

    const double rate = 48000.0;

    for (double fc : { 40.0, 250.0, 1000.0, 8000.0, 16000.0 })
        for (double q : { 0.3, 0.70710678, 3.0, 12.0 })
            for (double dB : { -24.0, -12.0, -3.0, -0.5, 0.5, 3.0, 12.0, 24.0 })
            {
                const auto b = make (rate, Fx::Svf::Type::bell, fc, q, dB);
                const double wanted = std::pow (10.0, dB / 20.0);

                expectNear (magnitudeAt (b, fc, rate), wanted, wanted * 1e-9,
                            "bell reaches its gain at the centre");

                expectNear (magnitudeAt (b, 0.0, rate), 1.0, 1e-9, "bell is flat at DC");
                expectNear (magnitudeAtNyquist (b, rate), 1.0, 1e-9, "bell is flat at Nyquist");
            }
}

/** The frequency where a band crosses `target`, searched between `lo` and
    `hi`, which must straddle exactly one crossing. Bisection on a geometric
    mean rather than an arithmetic one, because filter bands are geometric:
    the interesting range spans four decades and halving it linearly spends
    every step at the top of it. */
double crossing (const Fx::Svf& f, double sampleRate, double target,
                 double lo, double hi, bool wantAboveAtHi)
{
    for (int i = 0; i < 200; ++i)
    {
        const double mid = std::sqrt (lo * hi);
        const bool above = magnitudeAt (f, mid, sampleRate) > target;

        if (above == wantAboveAtHi)
            hi = mid;
        else
            lo = mid;
    }

    return std::sqrt (lo * hi);
}

/** A bell's width does not change when its height does.

    This is what separates an equaliser somebody trusts from one they fight.
    Turning a band from 3 dB to 12 dB should make it taller, not wider: if the
    width moves with the gain, then every time you commit to a boost you are
    also changing how much of the neighbouring material you took with it, and
    the band you carefully placed is no longer the band you placed.

    It is also invisible. The band still has the right gain at its centre and
    is still flat at both ends, which is everything the checks above look at,
    so a bell built without this is wrong in a way that only shows up next to
    another studio's. The mutation that removes it survived this file until
    this check was written.

    The half gain point is the standard place to measure: the frequency where
    the band has reached half its boost in decibels. */
void bellWidthDoesNotMoveWithGain()
{
    std::printf ("bell width is independent of gain\n");

    const double rate = 48000.0;

    for (double fc : { 200.0, 1000.0, 6000.0 })
        for (double q : { 0.5, 2.0, 8.0 })
        {
            double reference = -1.0;

            for (double dB : { 3.0, 6.0, 12.0, 24.0, -3.0, -6.0, -12.0, -24.0 })
            {
                const auto b = make (rate, Fx::Svf::Type::bell, fc, q, dB);

                // Half the gain in decibels is the square root of it as a
                // ratio, and the same expression serves a cut, where the
                // band dips to it instead of rising to it.
                const double target = std::sqrt (std::pow (10.0, dB / 20.0));
                const bool boosting = dB > 0.0;

                const double below = crossing (b, rate, target, 1.0, fc, boosting);
                const double above = crossing (b, rate, target, fc, rate * 0.49, ! boosting);
                const double octaves = std::log2 (above / below);

                if (reference < 0.0)
                {
                    reference = octaves;

                    // A sanity floor: Q of 8 is a narrow band and Q of 0.5 a
                    // wide one, and if the search returned the ends of its
                    // range instead of a crossing this would not hold.
                    if (! (octaves > 0.01 && octaves < 12.0))
                        fail ("bell width came out as " + show (octaves)
                              + " octaves at Q " + show (q));
                }
                else
                {
                    expectNear (octaves, reference, reference * 1e-6,
                                "bell width in octaves at " + show (dB) + " dB");
                }
            }
        }
}

/** A shelf's half gain point lands on the frequency it was given.

    The same shape of fault as the bell's width, and the same invisibility: a
    shelf whose ends are both correct can still have its transition in the
    wrong place, so the number on the control is not where the shelf is. The
    half gain point is where a shelf is conventionally said to be, and here it
    is exact rather than approximate, which makes it worth asserting tightly. */
void shelfSitsWhereItWasPut()
{
    std::printf ("shelf transition lands on its frequency\n");

    const double rate = 48000.0;

    for (auto type : { Fx::Svf::Type::lowShelf, Fx::Svf::Type::highShelf })
        for (double fc : { 80.0, 400.0, 2000.0, 9000.0 })
            for (double q : { 0.4, 0.70710678, 1.5 })
                for (double dB : { -18.0, -6.0, 6.0, 18.0 })
                {
                    const auto f = make (rate, type, fc, q, dB);
                    const double half = std::sqrt (std::pow (10.0, dB / 20.0));

                    expectNear (magnitudeAt (f, fc, rate), half, half * 1e-9,
                                "shelf is at half its gain at the stated frequency");
                }
}

/** A shelf's two ends are its two gains. */
void shelvesLandOnTheirGains()
{
    std::printf ("shelf ends\n");

    const double rate = 48000.0;

    for (double fc : { 60.0, 300.0, 2000.0, 10000.0 })
        for (double q : { 0.4, 0.70710678, 1.5 })
            for (double dB : { -18.0, -6.0, -1.0, 1.0, 6.0, 18.0 })
            {
                const double wanted = std::pow (10.0, dB / 20.0);

                const auto low = make (rate, Fx::Svf::Type::lowShelf, fc, q, dB);
                expectNear (magnitudeAt (low, 0.0, rate), wanted, wanted * 1e-9,
                            "low shelf lifts DC by its gain");
                expectNear (magnitudeAtNyquist (low, rate), 1.0, 1e-9,
                            "low shelf leaves Nyquist alone");

                const auto high = make (rate, Fx::Svf::Type::highShelf, fc, q, dB);
                expectNear (magnitudeAtNyquist (high, rate), wanted, wanted * 1e-9,
                            "high shelf lifts Nyquist by its gain");
                expectNear (magnitudeAt (high, 0.0, rate), 1.0, 1e-9,
                            "high shelf leaves DC alone");
            }
}

/** A bell or shelf at zero decibels has to be exactly a wire.

    This is the one that bites in a real equaliser. A band sits at zero until
    somebody uses it, so every band in the chain is passing audio through its
    arithmetic all the time, and a bell that is 0.02 dB off at zero gain puts
    that error through eight bands on sixteen inserts. It has to be zero, not
    nearly zero. */
void neutralAtZeroGain()
{
    std::printf ("flat at zero gain\n");

    const double rate = 44100.0;

    for (auto type : { Fx::Svf::Type::bell, Fx::Svf::Type::lowShelf, Fx::Svf::Type::highShelf })
        for (double fc : { 25.0, 440.0, 5000.0, 19000.0 })
            for (double q : { 0.2, 0.70710678, 8.0 })
            {
                const auto f = make (rate, type, fc, q, 0.0);

                for (double hz = 0.0; hz <= rate * 0.5; hz += rate * 0.5 / 129.0)
                    expectNear (magnitudeAt (f, hz, rate), 1.0, 1e-12,
                                "zero gain band is a wire at " + std::to_string (hz));
            }
}

/** The structural identity the topology guarantees, checked on real samples
    rather than on paper.

        highpass + (1/Q) * bandpassQ + lowpass == input

    It is exact, which makes it the sharpest check in this file: it holds to
    machine precision or the arithmetic is wrong. FilterTest asserts the same
    thing about the synth's filter, and it is worth asserting twice, because
    it catches a mistyped a1, a2 or a3 that every magnitude check above would
    let through as a slightly different filter. */
void structuralIdentityOnSamples()
{
    std::printf ("structural identity, sample by sample\n");

    std::mt19937 rng (20261007);
    std::uniform_real_distribution<double> audio (-1.0, 1.0);
    std::uniform_real_distribution<double> freq (20.0, 18000.0);
    std::uniform_real_distribution<double> qDist (0.1, 40.0);

    for (int trial = 0; trial < 200; ++trial)
    {
        const double rate = 48000.0;
        const double fc = freq (rng);
        const double q = qDist (rng);

        auto lp = make (rate, Fx::Svf::Type::lowpass, fc, q);
        auto hp = make (rate, Fx::Svf::Type::highpass, fc, q);
        auto bp = make (rate, Fx::Svf::Type::bandpassQ, fc, q);
        auto notch = make (rate, Fx::Svf::Type::notch, fc, q);
        auto peak = make (rate, Fx::Svf::Type::peak, fc, q);
        auto ap = make (rate, Fx::Svf::Type::allpass, fc, q);

        for (int n = 0; n < 400; ++n)
        {
            const double x = audio (rng);

            const double l = lp.process (x);
            const double h = hp.process (x);
            const double b = bp.process (x);
            const double nt = notch.process (x);
            const double pk = peak.process (x);
            const double a = ap.process (x);

            expectNear (h + (1.0 / q) * b + l, x, 1e-9, "hp + k*bp + lp == input");
            expectNear (nt, l + h, 1e-9, "notch == lowpass + highpass");
            expectNear (pk, h - l, 1e-9, "peak == highpass - lowpass");
            expectNear (a, x - 2.0 * (1.0 / q) * b, 1e-9, "allpass == input - 2k*bandpass");
        }
    }
}

/** The filter it runs as is the filter its coefficients describe.

    Everything above splits into two halves that never met. The magnitude
    checks are computed from the coefficients and never call process(); the
    identity check calls process() but only compares outputs produced within
    one sample, which holds whatever the state update does afterwards. So a
    mistake in the integrator update, which is the line that makes this a
    trapezoidal integrator rather than a Euler one, fell straight through the
    gap: it changes every response the filter has and nothing noticed. It was
    a deliberately broken copy that found this, not a failing test.

    This closes it by running the state space form forward beside the real
    thing and comparing sample by sample. Any difference in how the state is
    carried shows up on the second sample and never recovers. */
void processMatchesItsStateSpace()
{
    std::printf ("process matches its own transfer function\n");

    std::mt19937 rng (424242);
    std::uniform_real_distribution<double> audio (-1.0, 1.0);
    std::uniform_real_distribution<double> freq (20.0, 18000.0);
    std::uniform_real_distribution<double> qDist (0.1, 30.0);
    std::uniform_real_distribution<double> gainDist (-24.0, 24.0);

    const Fx::Svf::Type types[] = {
        Fx::Svf::Type::lowpass, Fx::Svf::Type::highpass, Fx::Svf::Type::bandpass,
        Fx::Svf::Type::bandpassQ, Fx::Svf::Type::notch, Fx::Svf::Type::peak,
        Fx::Svf::Type::allpass, Fx::Svf::Type::bell,
        Fx::Svf::Type::lowShelf, Fx::Svf::Type::highShelf
    };

    for (int trial = 0; trial < 300; ++trial)
    {
        const double rate = 48000.0;

        Fx::Svf f;
        f.setSampleRate (rate);
        f.set (types[trial % 10], freq (rng), qDist (rng), gainDist (rng));

        const auto s = stateSpace (f.coefficients());

        double s1 = 0.0, s2 = 0.0;

        for (int n = 0; n < 2000; ++n)
        {
            const double x = audio (rng);

            const double expected = s.c1 * s1 + s.c2 * s2 + s.d * x;
            const double actual = f.process (x);

            const double next1 = s.a11 * s1 + s.a12 * s2 + s.b1 * x;
            const double next2 = s.a21 * s1 + s.a22 * s2 + s.b2 * x;
            s1 = next1;
            s2 = next2;

            if (! (std::fabs (actual - expected) <= 1e-9 * (1.0 + std::fabs (expected))))
            {
                fail ("process and its state space diverged at sample "
                      + std::to_string (n) + ": " + show (actual)
                      + " against " + show (expected));
                break;
            }
        }
    }
}

/** Over thousands of random settings, including the silly ones, the filter
    stays stable and finite.

    Randomised rather than hand picked because the settings that break a
    filter are the ones nobody thinks to write down: a cutoff a hair under
    Nyquist, a Q of 100 at 20 hertz, a shelf boosted 60 dB at the top of the
    range where its g is multiplied again. An automation lane sweeping a
    cutoff visits all of them. */
void stableEverywhere()
{
    std::printf ("stable over random settings\n");

    std::mt19937 rng (987654321);
    std::uniform_real_distribution<double> audio (-1.0, 1.0);
    std::uniform_real_distribution<double> rateDist (22050.0, 192000.0);
    std::uniform_real_distribution<double> gainDist (-60.0, 60.0);
    std::uniform_real_distribution<double> qLog (std::log (0.02), std::log (150.0));

    const Fx::Svf::Type types[] = {
        Fx::Svf::Type::lowpass, Fx::Svf::Type::highpass, Fx::Svf::Type::bandpass,
        Fx::Svf::Type::bandpassQ, Fx::Svf::Type::notch, Fx::Svf::Type::peak,
        Fx::Svf::Type::allpass, Fx::Svf::Type::bell,
        Fx::Svf::Type::lowShelf, Fx::Svf::Type::highShelf
    };

    for (int trial = 0; trial < 4000; ++trial)
    {
        const double rate = rateDist (rng);
        std::uniform_real_distribution<double> freqDist (0.5, rate);   // past Nyquist on purpose

        Fx::Svf f;
        f.setSampleRate (rate);
        f.set (types[trial % 10], freqDist (rng), std::exp (qLog (rng)), gainDist (rng));

        // The poles have to be inside the unit circle. Checking it directly
        // rather than waiting for the output to blow up catches a filter that
        // is only just unstable, which is the one that survives a short test
        // and rings for a minute in a real session.
        const double largest = largestPoleModulus (f);

        if (! (largest < 1.0 + 1e-12))
            fail ("pole outside the unit circle, modulus " + std::to_string (largest));

        for (int n = 0; n < 500; ++n)
        {
            const double y = f.process (audio (rng));

            if (! std::isfinite (y))
            {
                fail ("output stopped being finite");
                break;
            }
        }
    }
}

/** Settings outside what a filter can do are brought back to what it can,
    rather than producing a tangent of something close to a right angle. */
void outOfRangeSettingsAreHeldIn()
{
    std::printf ("out of range settings\n");

    const double rate = 48000.0;

    for (double hz : { -1000.0, 0.0, rate * 0.5, rate, rate * 4.0 })
        for (double q : { -5.0, 0.0, 1e-9, 1e9 })
        {
            auto f = make (rate, Fx::Svf::Type::lowpass, hz, q);

            const auto c = f.coefficients();

            if (! (std::isfinite (c.a1) && std::isfinite (c.a2) && std::isfinite (c.a3)))
                fail ("coefficients stopped being finite at "
                      + std::to_string (hz) + " hz, Q " + std::to_string (q));

            for (int n = 0; n < 200; ++n)
                if (! std::isfinite (f.process (0.5)))
                {
                    fail ("output stopped being finite at "
                          + std::to_string (hz) + " hz, Q " + std::to_string (q));
                    break;
                }
        }
}

/** Reset really clears it, so a filter reused on the next clip does not carry
    the last one's tail into it. */
void resetClearsTheTail()
{
    std::printf ("reset\n");

    auto f = make (48000.0, Fx::Svf::Type::lowpass, 200.0, 8.0);

    for (int n = 0; n < 1000; ++n)
        f.process (1.0);

    f.reset();

    // With no state and no input there is nothing to come out.
    for (int n = 0; n < 16; ++n)
        expectNear (f.process (0.0), 0.0, 0.0, "silence after reset");
}

} // namespace

//==============================================================================
/** The bell and the two shelves are the cookbook biquads, exactly.

    Not similar to them: the same transfer functions in a different topology.
    That is worth asserting rather than assuming, because it ties this filter
    to a published closed form rather than to itself, and because the mixing
    weights are the one place a typo produces a filter that still works. The
    coefficients come from Robert Bristow-Johnson, "Cookbook formulae for
    audio equalizer biquad filter coefficients", in the W3C's copy of his own
    text, https://www.w3.org/TR/audio-eq-cookbook, written out here rather
    than referred to so the comparison is against the paper and not against a
    second reading of this file.
*/
double cookbookMagnitude (Fx::Svf::Type type, double hz, double q, double gainDb,
                          double at, double rate)
{
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w0 = 2.0 * pi * hz / rate;
    const double alpha = std::sin (w0) / (2.0 * q);
    const double cosine = std::cos (w0);
    const double rootA = std::sqrt (A);

    double b0 = 0.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

    if (type == Fx::Svf::Type::bell)
    {
        b0 = 1.0 + alpha * A;  b1 = -2.0 * cosine;  b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A;  a1 = -2.0 * cosine;  a2 = 1.0 - alpha / A;
    }
    else if (type == Fx::Svf::Type::lowShelf)
    {
        b0 =        A * ((A + 1.0) - (A - 1.0) * cosine + 2.0 * rootA * alpha);
        b1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cosine);
        b2 =        A * ((A + 1.0) - (A - 1.0) * cosine - 2.0 * rootA * alpha);
        a0 =             (A + 1.0) + (A - 1.0) * cosine + 2.0 * rootA * alpha;
        a1 =     -2.0 * ((A - 1.0) + (A + 1.0) * cosine);
        a2 =             (A + 1.0) + (A - 1.0) * cosine - 2.0 * rootA * alpha;
    }
    else
    {
        b0 =        A * ((A + 1.0) + (A - 1.0) * cosine + 2.0 * rootA * alpha);
        b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosine);
        b2 =        A * ((A + 1.0) + (A - 1.0) * cosine - 2.0 * rootA * alpha);
        a0 =             (A + 1.0) - (A - 1.0) * cosine + 2.0 * rootA * alpha;
        a1 =      2.0 * ((A - 1.0) - (A + 1.0) * cosine);
        a2 =             (A + 1.0) - (A - 1.0) * cosine - 2.0 * rootA * alpha;
    }

    const Complex z = std::polar (1.0, -2.0 * pi * at / rate);

    return std::abs ((b0 + b1 * z + b2 * z * z) / (a0 + a1 * z + a2 * z * z));
}

void theBellAndShelvesAreTheCookbook()
{
    std::printf ("the bell and shelves against the cookbook\n");

    const double rate = 44100.0;
    double worst = 0.0;

    for (auto type : { Fx::Svf::Type::bell, Fx::Svf::Type::lowShelf,
                       Fx::Svf::Type::highShelf })
    {
        double worstHere = 0.0;

        for (double hz : { 30.0, 200.0, 1000.0, 5000.0, 12000.0, 18000.0, 21000.0 })
            for (double q : { 0.3, 0.70710678118654752, 2.0, 8.0 })
                for (double gainDb : { -18.0, -6.0, -1.0, 1.0, 6.0, 18.0 })
                {
                    const auto f = make (rate, type, hz, q, gainDb);

                    for (double at = 10.0; at < rate * 0.5; at *= 1.08)
                    {
                        const double mine = magnitudeAt (f, at, rate);
                        const double theirs = cookbookMagnitude (type, hz, q, gainDb,
                                                                 at, rate);

                        const double offBy = std::fabs (20.0 * std::log10 (mine)
                                                          - 20.0 * std::log10 (theirs));

                        worstHere = std::max (worstHere, offBy);
                    }
                }

        std::printf ("    %-10s worst disagreement %s dB\n",
                     type == Fx::Svf::Type::bell ? "bell"
                       : (type == Fx::Svf::Type::lowShelf ? "low shelf" : "high shelf"),
                     show (worstHere).c_str());

        worst = std::max (worst, worstHere);
    }

    if (worst > 1.0e-6)
        fail ("the filter is not the cookbook biquad: off by " + show (worst) + " dB");
}

//==============================================================================
/** The analogue prototype each form is the bilinear image of, which is what
    "the response this is supposed to have" means. */
double prototypeMagnitude (Fx::Svf::Type type, double hz, double q, double gainDb,
                           double at)
{
    const double A = std::pow (10.0, gainDb / 40.0);

    if (type == Fx::Svf::Type::bell)
    {
        const double k = 1.0 / (q * A);
        const Complex s (0.0, at / hz);

        return std::abs ((s * s + k * A * A * s + 1.0) / (s * s + k * s + 1.0));
    }

    const double k = 1.0 / q;

    if (type == Fx::Svf::Type::lowShelf)
    {
        const Complex s (0.0, (at / hz) * std::sqrt (A));

        return std::abs ((s * s + k * A * s + A * A) / (s * s + k * s + 1.0));
    }

    const Complex s (0.0, (at / hz) / std::sqrt (A));

    return std::abs ((A * A * s * s + k * A * s + 1.0) / (s * s + k * s + 1.0));
}

/** How far the conventional design is from its own prototype near the top of
    the band, asserted as a quantity rather than as "small".

    This is the fault `BellMatch.h` exists to correct, and the figures are
    here so that nobody has to take its header on trust. Asserted with a
    tolerance both ways on purpose: a test that only said the error was under
    something would pass on a filter that had stopped being a bell at all.
*/
void theNearNyquistErrorIsAKnownQuantity()
{
    std::printf ("the conventional design against its own prototype, at 44.1 kHz\n");

    const double rate = 44100.0;

    struct Case
    {
        const char* name;
        Fx::Svf::Type type;
        double hz, q, gainDb, expected;
    };

    // Measured, and then checked against an independent evaluation of the
    // same prototypes in a throwaway probe before being written down.
    const Case cases[]
    {
        { "high shelf 12 kHz Q 0.707 +12 dB", Fx::Svf::Type::highShelf,
          12000.0, 0.70710678118654752, 12.0, 2.196 },
        { "high shelf 16 kHz Q 0.707 +12 dB", Fx::Svf::Type::highShelf,
          16000.0, 0.70710678118654752, 12.0, 3.630 },
        { "high shelf 12 kHz Q 0.707  +6 dB", Fx::Svf::Type::highShelf,
          12000.0, 0.70710678118654752, 6.0, 1.034 },
        { "bell       12 kHz Q 1     +12 dB", Fx::Svf::Type::bell,
          12000.0, 1.0, 12.0, 5.233 },
        { "bell        8 kHz Q 1      +6 dB", Fx::Svf::Type::bell,
          8000.0, 1.0, 6.0, 1.299 },
        { "bell        1 kHz Q 1     +12 dB", Fx::Svf::Type::bell,
          1000.0, 1.0, 12.0, 0.052 }
    };

    for (const auto& c : cases)
    {
        const auto f = make (rate, c.type, c.hz, c.q, c.gainDb);

        double worst = 0.0;

        for (double at = 20.0; at <= 20000.0; at *= 1.002)
            worst = std::max (worst,
                              std::fabs (20.0 * std::log10 (magnitudeAt (f, at, rate))
                                           - 20.0 * std::log10 (prototypeMagnitude
                                                                  (c.type, c.hz, c.q,
                                                                   c.gainDb, at))));

        std::printf ("    %s: off by %.3f dB (known %.3f)\n", c.name, worst, c.expected);

        expectNear (worst, c.expected, 0.01,
                    std::string ("the known deviation for ") + c.name + " has moved");
    }
}

/** Two structural facts behind all of the above, which are exact rather than
    measured: a digital bell is unity at Nyquist whatever its gain, because
    the prototype is unity at infinite frequency and the transform maps one
    onto the other, and a shelf reaches its far gain there for the same
    reason. Asserting them means a future change to the prototype cannot
    pass unnoticed. */
void theEndsOfTheBandAreExact()
{
    std::printf ("the top of the band, exactly\n");

    const double rate = 48000.0;

    for (double hz : { 500.0, 4000.0, 15000.0 })
        for (double q : { 0.5, 1.0, 4.0 })
            for (double gainDb : { -12.0, -3.0, 6.0, 15.0 })
            {
                const auto bell = make (rate, Fx::Svf::Type::bell, hz, q, gainDb);

                expectNear (magnitudeAtNyquist (bell, rate), 1.0, 1.0e-12,
                            "a bell is not unity at Nyquist at " + show (hz) + " Hz");

                const auto high = make (rate, Fx::Svf::Type::highShelf, hz, q, gainDb);
                const double wanted = std::pow (10.0, gainDb / 20.0);

                expectNear (magnitudeAtNyquist (high, rate) / wanted, 1.0, 1.0e-9,
                            "a high shelf does not reach its gain at Nyquist at "
                              + show (hz) + " Hz");
            }

    std::printf ("    a bell is unity there and a high shelf is at its gain\n");
}

/** A cut is the exact mirror of the boost, in decibels, at every frequency.

    True of the analogue prototype, and the digital bell inherits it exactly
    because the bell's corner does not depend on its gain. The shelves inherit
    it too, which is less obvious since their corners do depend on the gain.
    Worth asserting because an equaliser whose cuts are not the mirror of its
    boosts is one where undoing a move does not undo it.
*/
void boostAndCutMirrorExactly()
{
    std::printf ("boost against cut\n");

    const double rate = 48000.0;

    for (auto type : { Fx::Svf::Type::bell, Fx::Svf::Type::lowShelf,
                       Fx::Svf::Type::highShelf })
    {
        double worst = 0.0;

        for (double hz : { 50.0, 400.0, 3000.0, 15000.0 })
            for (double q : { 0.3, 1.0, 8.0 })
                for (double gainDb : { 1.0, 6.0, 24.0 })
                {
                    const auto up = make (rate, type, hz, q, gainDb);
                    const auto down = make (rate, type, hz, q, -gainDb);

                    for (double at = 20.0; at < rate * 0.49; at *= 1.1)
                        worst = std::max (worst,
                                          std::fabs (20.0 * std::log10
                                                       (magnitudeAt (up, at, rate))
                                                       + 20.0 * std::log10
                                                           (magnitudeAt (down, at, rate))));
                }

        std::printf ("    %-10s mirrors to %s dB\n",
                     type == Fx::Svf::Type::bell ? "bell"
                       : (type == Fx::Svf::Type::lowShelf ? "low shelf" : "high shelf"),
                     show (worst).c_str());

        if (worst > 1.0e-11)
            fail ("a cut is not the mirror of a boost: off by " + show (worst) + " dB");
    }
}

/** Why the bandwidth is measured at half the decibel gain and not three
    decibels down: below three decibels of gain there is no three decibel
    point to measure at.

    This is the fact that justifies the convention, and without it written
    down somebody will improve the definition to the one that sounds more
    standard and break every Q in every preset.
*/
void aSmallBellHasNoThreeDecibelPoint()
{
    std::printf ("the bandwidth convention\n");

    const double rate = 48000.0;
    const double hz = 1000.0;

    for (double gainDb : { 1.0, 3.0, 6.0, 12.0, 24.0 })
    {
        const auto f = make (rate, Fx::Svf::Type::bell, hz, 1.0, gainDb);

        const double half = std::pow (10.0, gainDb / 40.0);

        const double below = crossing (f, rate, half, 10.0, hz, true);
        const double above = crossing (f, rate, half, hz, rate * 0.49, false);

        const double octaves = std::log2 (above / below);

        // The same span at every gain is what constant Q means, and it comes
        // free from dividing the damping by the amplitude.
        std::printf ("    %+5.1f dB: half gain span %.4f octaves, three decibel"
                     " point %s\n", gainDb, octaves,
                     gainDb > 3.0 ? "exists" : "does not exist");

        expectNear (octaves, 1.3839, 0.002,
                    "the half gain span moved at " + show (gainDb) + " dB of gain");

        const double peak = std::pow (10.0, gainDb / 20.0);
        const bool hasThreeDecibelPoint = peak > std::pow (10.0, 3.0 / 20.0);

        if (hasThreeDecibelPoint != (gainDb > 3.0))
            fail ("the three decibel point exists at " + show (gainDb)
                  + " dB where it should not, or the other way about");
    }
}

/** The filter's own `magnitudeAt` against the exact state space response.

    Both are in this project and they must agree, because the equaliser draws
    its curve from the first and every check here is made with the second. A
    curve that disagrees with the filter is a curve that lies to the person
    using it, and it would lie quietly.
*/
void theFiltersOwnCurveIsTheRealOne()
{
    std::printf ("magnitudeAt against the state space response\n");

    double worst = 0.0;

    for (double rate : { 44100.0, 48000.0, 96000.0 })
        for (auto type : { Fx::Svf::Type::lowpass, Fx::Svf::Type::highpass,
                           Fx::Svf::Type::bandpass, Fx::Svf::Type::bandpassQ,
                           Fx::Svf::Type::notch, Fx::Svf::Type::peak,
                           Fx::Svf::Type::allpass, Fx::Svf::Type::bell,
                           Fx::Svf::Type::lowShelf, Fx::Svf::Type::highShelf,
                           Fx::Svf::Type::bellMatched })
            for (double hz : { 40.0, 500.0, 6000.0, 17000.0 })
                for (double q : { 0.4, 1.0, 6.0 })
                {
                    const auto f = make (rate, type, hz, q, 9.0);

                    // Up to the filter's own ceiling rather than stopping at
                    // 0.45 of the rate. The top twentieth of the band was
                    // never compared, and it is where a bell's skirt is
                    // crushed and where the matched design exists to do
                    // something about it: the one stretch of the axis the two
                    // arithmetics are most likely to disagree on was the one
                    // stretch nothing looked at.
                    for (double at = 20.0; at < rate * 0.4998; at *= 1.3)
                    {
                        const double exact = magnitudeAt (f, at, rate);
                        const double drawn = f.magnitudeAt (at);

                        if (exact > 1.0e-6)
                            worst = std::max (worst,
                                              std::fabs (20.0 * std::log10 (drawn / exact)));
                    }
                }

    std::printf ("    worst disagreement %s dB\n", show (worst).c_str());

    // A nanodecibel rather than nothing at all. The two are different
    // arithmetic for the same answer: `magnitudeAt` evaluates the transfer
    // function directly, where the state space form above inverts a two by
    // two whose determinant is nearly zero for a high Q band near Nyquist,
    // and that inversion is what sets this floor. It is eight orders of
    // magnitude below any weight typed wrong, which is what this is for.
    if (worst > 1.0e-7)
        fail ("the curve the filter draws is not the response it has: off by "
              + show (worst) + " dB");
}

//==============================================================================
/** The sample rate can change under a filter that is already set up, and the
    filter has to come out the other side as the filter it was asked for.

    This is the check that was missing, and the shape of the gap is worth
    recording. Every other check in this file sets the sample rate first and
    the frequency second, which is the order a test naturally writes and the
    order `make()` above hard codes. Nothing here had ever moved the rate
    afterwards. So a cutoff clamped against one rate and then read against
    another was invisible, and what it produced was not a small error: past
    the new Nyquist the prewarping tangent is negative, a negative `g` puts
    the poles outside the unit circle, and the section doubles its own output
    until it is not a number any more. Measured before the fix, every one of
    the eleven responses reached a non-finite output within 2001 samples
    going from 192 kHz to 48 kHz.

    The property asserted is the strongest one available and it is not about
    the clamp at all: **a filter told the rate has changed must be
    indistinguishable from one built at that rate in the first place.** It
    holds whatever the clamp does, it needs no reference arithmetic, and
    there is no setting at which it is allowed to be approximately true.

    Reachable through nothing today, which is why it needs a test rather than
    a bug report. `LowPassGate` re-sets its corner every sample and
    `PassFilter` re-sets every section whenever the rate moves. `Voice`
    escapes by an accident of ordering in a different file: it guards
    `filter.set` on the modulated cutoff having actually moved, so after a
    rate change the stale coefficients would stay for the life of a note, and
    the only reason they do not is that `PanOne::prepareToPlay` happens to
    follow `setSampleRate` with `setPatch`, which ends in `retune`, which
    forgets the guard. */
void theRateCanChangeUnderAFilterThatIsAlreadySet()
{
    std::printf ("the rate changing under a filter already set up\n");

    const double rates[] = { 8000.0, 22050.0, 44100.0, 48000.0, 96000.0, 192000.0 };

    int worstCase = 0;
    double worstPole = 0.0;

    for (double first : rates)
        for (double second : rates)
        {
            // Across the whole band of the rate it was set at, including the
            // top of it, which is where the clamp binds and therefore the
            // only place the stale corner can differ from the asked for one.
            const double fractions[] = { 0.0001, 0.01, 0.2, 0.45, 0.4899, 0.6, 2.0 };

            for (double fraction : fractions)
                for (auto type : allTypes)
                {
                    const double hz = first * fraction;
                    const double q = 4.0;
                    const double gainDb = 9.0;

                    Fx::Svf moved;
                    moved.setSampleRate (first);
                    moved.set (type, hz, q, gainDb);
                    moved.setSampleRate (second);       // the device changed

                    Fx::Svf fresh;
                    fresh.setSampleRate (second);
                    fresh.set (type, hz, q, gainDb);

                    const auto a = moved.coefficients();
                    const auto b = fresh.coefficients();

                    const std::string where = std::string (nameOf (type)) + " "
                        + show (hz) + " hz, " + show (first) + " -> " + show (second);

                    if (a.a1 != b.a1 || a.a2 != b.a2 || a.a3 != b.a3
                        || a.m0 != b.m0 || a.m1 != b.m1 || a.m2 != b.m2)
                    {
                        fail ("a filter told the rate changed is not the filter"
                              " built at that rate: " + where
                              + ": a1 " + show (a.a1) + " against " + show (b.a1)
                              + ", a2 " + show (a.a2) + " against " + show (b.a2)
                              + ", a3 " + show (a.a3) + " against " + show (b.a3));
                        return;
                    }

                    if (moved.isMatched() != fresh.isMatched())
                    {
                        fail ("the matched bell decided differently after a rate"
                              " change: " + where);
                        return;
                    }

                    // Said twice on purpose. The equality above is exact and
                    // would hold just as well if both filters were unstable,
                    // because it compares one against the other and nothing
                    // against the world. This says what the arrangement is
                    // for: whatever the rate did, the thing that comes out
                    // runs.
                    const double pole = largestPoleModulus (moved);

                    if (! (pole < 1.0))
                    {
                        fail ("pole outside the unit circle after a rate change: "
                              + where + ", modulus " + show (pole));
                        return;
                    }

                    worstPole = std::max (worstPole, pole);
                    ++worstCase;
                }
        }

    constexpr std::size_t numRates = sizeof (rates) / sizeof (rates[0]);

    std::printf ("    %d settings across %zu pairs of rates, worst pole modulus %s\n",
                 worstCase, numRates * numRates, show (worstPole).c_str());

    // And the output really does stay finite, over long enough to have run
    // away. Before the fix the fastest case took 807 samples and the slowest
    // 11283, so a quarter of a million is a wide margin on both.
    std::mt19937 rng (20261022);
    std::uniform_real_distribution<double> audio (-1.0, 1.0);

    for (double first : { 96000.0, 192000.0 })
        for (double second : { 8000.0, 44100.0, 48000.0 })
            for (auto type : allTypes)
            {
                Fx::Svf f;
                f.setSampleRate (first);
                f.set (type, first * 0.48, 0.707, 6.0);
                f.setSampleRate (second);

                for (int n = 0; n < 250000; ++n)
                {
                    const double y = f.process (n == 0 ? 1.0 : audio (rng) * 1e-6);

                    if (! std::isfinite (y))
                    {
                        fail (std::string ("a ") + nameOf (type) + " ran away "
                              + show (first) + " -> " + show (second)
                              + " after " + std::to_string (n) + " samples");
                        break;
                    }
                }
            }
}

//==============================================================================
/** A filter nobody has configured yet is the filter its own members describe,
    and its two answers agree with each other.

    It gave three different answers before. The coefficients all started at
    zero, so `process` returned silence for ever; `magnitudeAt` saw a zero
    `a2`, took its degenerate branch and reported unity at every frequency;
    and the members said a one kilohertz lowpass at 44.1 kHz, which is
    neither of those. Nothing in this file had ever looked at a filter that
    had not been handed a rate and a setting first.

    Which of the two answers is wrong depends on who is asking, and that is
    why both are checked. An equaliser drawing its curve before anybody has
    touched a band would have drawn a flat line through unity, which looks
    exactly like a band doing nothing and is the one thing a bypassed band
    should look like. The same band in the signal path would have deleted
    everything through it. A fault that looks correct on screen and silences
    the audio is the worst possible pairing of the two.

    The agreement is asserted against the running filter rather than between
    the two formulas, because `process` returning silence and `magnitudeAt`
    returning unity are each individually self consistent. */
void anUnconfiguredFilterIsWhatItsDefaultsDescribe()
{
    std::printf ("an unconfigured filter\n");

    const double rate = 44100.0;

    Fx::Svf fresh;                       // nothing called on it at all

    if (! (fresh.getSampleRate() == rate))
        fail ("an unconfigured filter does not report the rate its members say");

    // 1. The curve it draws is the response it has. This is the one that
    //    fails whichever of the two answers is the wrong one.
    for (double hz = 10.0; hz < rate * 0.5; hz *= 1.3)
    {
        const double drawn = fresh.magnitudeAt (hz);
        const double real = magnitudeAt (fresh, hz, rate);

        expectNear (drawn, real, 1e-9 * (1.0 + real),
                    "unconfigured: drawn curve against the real response at "
                        + show (hz) + " hz");
    }

    // 2. It is a second order lowpass at a kilohertz with a Butterworth Q,
    //    which is what the members below it say. Spelled out rather than
    //    compared against a reference filter, because a reference built the
    //    same way would agree with it however wrong both were.
    expectNear (fresh.magnitudeAt (0.0), 1.0, 1e-12,
                "unconfigured: unity at DC");
    expectNear (fresh.magnitudeAt (1000.0), 1.0 / std::sqrt (2.0), 1e-9,
                "unconfigured: three decibels down at its corner");

    if (! (fresh.magnitudeAt (rate * 0.5) < 1e-6))
        fail ("unconfigured: should pass nothing at Nyquist, passes "
              + show (fresh.magnitudeAt (rate * 0.5)));

    // Twelve decibels an octave above the corner, which is what makes it two
    // poles rather than one.
    const double decadeUp = fresh.magnitudeAt (10000.0);
    const double twoDecadesUp = fresh.magnitudeAt (100000.0 > rate * 0.49 ? rate * 0.49 : 100000.0);

    if (! (decadeUp < 0.02 && twoDecadesUp < decadeUp))
        fail ("unconfigured: does not fall like two poles, "
              + show (decadeUp) + " then " + show (twoDecadesUp));

    // 3. It passes audio, and it passes it as that lowpass. A step into a
    //    lowpass settles at the DC gain, which is one. Silence for ever was
    //    the old answer and it is the one a musician would have met.
    double last = 0.0;

    for (int n = 0; n < 20000; ++n)
        last = fresh.process (1.0);

    expectNear (last, 1.0, 1e-9, "unconfigured: a step settles at unity");

    // 4. And it is the same filter as one set up by hand to its own defaults,
    //    to the last bit, which is what says the defaults and the
    //    coefficients have not drifted apart.
    Fx::Svf byHand;
    byHand.setSampleRate (rate);
    byHand.set (Fx::Svf::Type::lowpass, 1000.0, 0.70710678118654752);

    const auto a = fresh.coefficients();
    const auto b = byHand.coefficients();

    if (a.a1 != b.a1 || a.a2 != b.a2 || a.a3 != b.a3
        || a.m0 != b.m0 || a.m1 != b.m1 || a.m2 != b.m2)
        fail ("an unconfigured filter is not the one its members describe");

    std::printf ("    a step settles at %s, corner %s of unity\n",
                 show (last).c_str(), show (fresh.magnitudeAt (1000.0)).c_str());
}

//==============================================================================
/** `isMatched` says which of the two bell designs is running, and it has to
    be telling the truth.

    Nothing called it before tonight. `bellMatched` appeared once in this
    whole file, inside the curve check, and the flag the header says is
    "worth exposing rather than hiding" was never read. It is the difference
    between a twelve kilohertz bell being a tenth of a decibel out and five
    decibels out, and an equaliser that draws its own curve draws the wrong
    one if the flag lies.

    Four claims, none of which had a check:

    - every other type reports false, which is what makes the flag readable
      at all rather than stale from the last band that was a matched bell;
    - it never claims a match where `BellMatch.h`'s own feasibility predicate
      says there is none, so the flag and the predicate cannot drift apart;
    - wherever the match is feasible and the gain is a real one, it is taken,
      so the correction cannot quietly stop being applied;
    - where the match is out of reach the coefficients are the plain bell's
      exactly. The header says those are "the same three lines as above
      deliberately", and a typo in either copy is otherwise two bells that
      differ by a frequency nobody looks at.

    One thing checking this turned up, which is a property of the design
    rather than a fault. `bellIsMatchable` is a necessary condition and not a
    sufficient one: it deliberately leaves gain out, because at a bandwidth
    taken at half the decibel gain the feasibility condition cancels it, but
    `matchedBellBoost` additionally needs the three gains strictly ordered,
    `g^2 > gb^2 > g0^2`, and a bell of no gain has not got that. So a
    `bellMatched` band at exactly zero decibels reports false and runs the
    plain bell, which at zero gain is a wire to the last bit. That is the
    right answer rather than a missing one, and it is asserted below as a
    wire rather than left as an argument. */
void theMatchedBellSaysWhichDesignItIsRunning()
{
    std::printf ("the matched bell reports which design it is running\n");

    const double rates[] = { 44100.0, 48000.0, 96000.0 };

    int fellBack = 0, matched = 0;

    for (double rate : rates)
    {
        // Nothing but a matched bell may ever report true, whatever was set
        // before it.
        for (auto type : allTypes)
        {
            Fx::Svf f;
            f.setSampleRate (rate);
            f.set (Fx::Svf::Type::bellMatched, 500.0, 1.0, 6.0);   // true first
            f.set (type, 500.0, 1.0, 6.0);

            if (type != Fx::Svf::Type::bellMatched && f.isMatched())
                fail (std::string ("a ") + nameOf (type)
                      + " reports a matched bell design");
        }

        for (double hz = 20.0; hz < rate * 0.49; hz *= 1.25)
            for (double q : { 0.3, 0.707, 2.0, 8.0, 30.0 })
                for (double gainDb : { -18.0, -6.0, 0.0, 6.0, 18.0 })
                {
                    Fx::Svf f;
                    f.setSampleRate (rate);
                    f.set (Fx::Svf::Type::bellMatched, hz, q, gainDb);

                    const bool feasible = Fx::bellIsMatchable (hz, q, rate);

                    // Never a match where the geometry forbids one. This
                    // direction holds at every gain, zero included.
                    if (f.isMatched() && ! feasible)
                    {
                        fail ("isMatched claims a design bellIsMatchable says"
                              " is out of reach, at " + show (hz) + " hz, Q "
                              + show (q) + ", " + show (gainDb) + " dB, rate "
                              + show (rate));
                        return;
                    }

                    // And always a match where the geometry allows one and
                    // there is a gain to correct. Without this the flag could
                    // be false everywhere and the first direction alone would
                    // be satisfied.
                    if (feasible && std::fabs (gainDb) >= 0.01 && ! f.isMatched())
                    {
                        fail ("a feasible matched bell was not taken, at "
                              + show (hz) + " hz, Q " + show (q) + ", "
                              + show (gainDb) + " dB, rate " + show (rate));
                        return;
                    }

                    Fx::Svf plain;
                    plain.setSampleRate (rate);
                    plain.set (Fx::Svf::Type::bell, hz, q, gainDb);

                    const auto a = f.coefficients();
                    const auto b = plain.coefficients();

                    const bool identical = a.a1 == b.a1 && a.a2 == b.a2 && a.a3 == b.a3
                                        && a.m0 == b.m0 && a.m1 == b.m1 && a.m2 == b.m2;

                    if (f.isMatched())
                    {
                        ++matched;

                        // A matched design that came out identical to the
                        // plain one is a correction that is not correcting.
                        // Only worth asserting where the two really should
                        // differ, which at zero gain they do not: a bell of
                        // no gain is a wire either way.
                        if (identical && hz > rate * 0.02)
                            fail ("a matched bell is bit for bit the plain one at "
                                  + show (hz) + " hz, Q " + show (q) + ", "
                                  + show (gainDb) + " dB");
                    }
                    else
                    {
                        ++fellBack;

                        if (! identical)
                            fail ("a fallen back bell is not the plain bell at "
                                  + show (hz) + " hz, Q " + show (q) + ", "
                                  + show (gainDb) + " dB, rate " + show (rate));
                    }
                }
    }

    // A matched bell of no gain is a wire, and bit for bit rather than
    // nearly. `neutralAtZeroGain` above never reached this type, so the one
    // response in the file with a second design behind it was also the one
    // whose neutral setting nothing checked. The weights make this exact:
    // the fallback plain bell at A of one has m1 and m2 of zero and an m0 of
    // one, so the output is the input and not a rounded copy of it.
    for (double rate : rates)
        for (double hz : { 50.0, 1000.0, 9000.0 })
            for (double q : { 0.3, 1.0, 12.0 })
            {
                Fx::Svf f;
                f.setSampleRate (rate);
                f.set (Fx::Svf::Type::bellMatched, hz, q, 0.0);

                if (f.isMatched())
                    fail ("a bell of no gain reports a matched design, which"
                          " needs its three gains strictly ordered");

                std::mt19937 rng (7);
                std::uniform_real_distribution<double> audio (-1.0, 1.0);

                for (int n = 0; n < 2000; ++n)
                {
                    const double x = audio (rng);

                    if (f.process (x) != x)
                    {
                        fail ("a matched bell at zero gain is not a wire at "
                              + show (hz) + " hz, Q " + show (q));
                        break;
                    }
                }

                for (double at = 20.0; at < rate * 0.49; at *= 1.7)
                    expectNear (f.magnitudeAt (at), 1.0, 1e-12,
                                "a matched bell at zero gain draws unity at "
                                    + show (at) + " hz");
            }

    std::printf ("    %d settings matched, %d fell back to the plain bell\n",
                 matched, fellBack);
}

//==============================================================================
/** A corner above the ceiling is the corner at the ceiling, exactly.

    The clamp is not only a number that stops the tangent running away: what
    it produces has to be a filter, and the only sensible filter to give back
    for a cutoff nobody can have is the highest one somebody can. Said as an
    equality rather than as a bound, so there is nothing to interpret.

    This is the check that catches the clamp being applied to the coefficients
    and not to the matched bell's design, which is a mistake with a seam in it:
    `bellIsMatchable`'s boundary rises with Q and passes the ceiling somewhere
    around a Q of ninety, so above that a band asked for a cutoff past the
    ceiling is feasible at the ceiling and infeasible at the frequency asked
    for. Designed from the wrong one of those, the band silently drops to the
    conventional bell, which at the top of the band is the five decibel error
    the matched design exists to remove. Only visible at a high Q, which is
    why the grid goes there. */
void aCornerAboveTheCeilingIsTheCeiling()
{
    std::printf ("a corner above the ceiling\n");

    for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const double ceiling = rate * 0.49;

        for (double q : { 0.5, 4.0, 40.0, 95.0, 100.0 })
            for (double gainDb : { -12.0, 0.0, 12.0 })
                for (double above : { 1.0, 1.0000001, 1.5, 4.0, 1.0e6 })
                    for (auto type : allTypes)
                    {
                        Fx::Svf asked;
                        asked.setSampleRate (rate);
                        asked.set (type, ceiling * above, q, gainDb);

                        Fx::Svf atCeiling;
                        atCeiling.setSampleRate (rate);
                        atCeiling.set (type, ceiling, q, gainDb);

                        const auto a = asked.coefficients();
                        const auto b = atCeiling.coefficients();

                        if (a.a1 != b.a1 || a.a2 != b.a2 || a.a3 != b.a3
                            || a.m0 != b.m0 || a.m1 != b.m1 || a.m2 != b.m2
                            || asked.isMatched() != atCeiling.isMatched())
                        {
                            fail (std::string ("a ") + nameOf (type) + " asked for "
                                  + show (above) + " times its ceiling is not the one"
                                  " at its ceiling, at rate " + show (rate) + ", Q "
                                  + show (q) + ", " + show (gainDb) + " dB"
                                  + (asked.isMatched() != atCeiling.isMatched()
                                        ? " (and the two disagree about the matched design)"
                                        : ""));
                            return;
                        }
                    }
    }

    std::printf ("    identical at four rates, five Q values and five overshoots\n");
}

//==============================================================================
/** Nonsense in the rate, the corner, the Q and the gain still leaves a filter
    that runs, and the invariant the header rests its stability on holds at
    every one of them.

    That invariant is the point of this check rather than finiteness. With the
    corner floored at one hertz and held under 0.49 of a finite rate, `g` is
    strictly positive and finite, and a trapezoidally integrated state
    variable filter with a positive `g` and a positive `k` is unconditionally
    stable. `a2` is `g` times `a1` and `a3` is `g` times `a2`, so asserting
    all three are strictly positive asserts exactly that, and it is a far
    sharper instrument than waiting for an output to stop being finite: a
    corner that has come out a hair negative gives poles whose modulus is
    1.0000000000000002, which runs away in a few seconds of real time and
    passes any tolerance anybody would write around one.
  
    It is also what makes the clamps load bearing rather than decorative.
    Every one of them was removable without a single check noticing before
    tonight:

    - the one hertz floor. A cutoff of nothing gives a `g` of nothing, and
      then `process` returns silence for ever while `magnitudeAt` takes its
      degenerate branch and draws unity: the same pair of contradictory
      answers an unconfigured filter used to give. Reachable, and not
      obscurely: the instrument modulates its cutoff in octaves, twelve of
      them downward, so a patch resting at the bottom of its range sweeps to
      twenty hertz over four thousand and ninety six, which is a fifth of a
      hundredth of a hertz.
    - the lower bound on Q. A Q of nothing makes `k` infinite, which takes
      `a1` to zero and arrives at the same contradiction by another road.
    - the bound on the gain. Past about 170 decibels of cut the amplitude
      underflows to nothing, and a shelf divides its `g` by the square root
      of it, so `g` becomes infinite, `a1` becomes nothing, and `a2` is
      infinity times nothing, which is not a number. The filter then outputs
      silence, permanently, from the first sample.
    - the order of the floor and the ceiling, which a sample rate of one
      hertz reaches.

    The values are finite nonsense rather than NaN, which is the same
    boundary `OnePoleTest` draws: neither header claims to repair a
    not-a-number handed in from outside, and a test that asserted it did
    would be asserting a guard nobody has decided to write. */
void nonsenseStillLeavesAFilterThatRuns()
{
    std::printf ("out of range use\n");

    double worstCurve = 0.0;
    double worstPole = 0.0;
    int settings = 0;

    // Infinity is in the list and a not-a-number is not, and the line
    // between them is where a value is still well defined. An infinite rate
    // has an answer the arithmetic walks straight into: the prewarping
    // tangent of anything over infinity is exactly zero, which is the one
    // value that takes `a2` to nothing, makes `g` unrecoverable from the
    // coefficients and puts the drawn curve down its degenerate branch. A
    // not-a-number has no answer, and neither this header nor `OnePole.h`
    // claims to repair one handed in from outside.
    for (double rate : { 0.0, -1.0, 1.0, 8000.0, 192000.0, 1.0e9,
                         std::numeric_limits<double>::infinity() })
        for (double hz : { -1.0e9, -1.0, 0.0, 1.0e-12, 1.0, 1.0e9 })
            for (double q : { -5.0, 0.0, 1.0e-12, 0.70710678118654752, 1.0e9 })
                for (double gainDb : { -1.0e6, -120.0, 0.0, 120.0, 1.0e6 })
                    for (auto type : allTypes)
                    {
                        Fx::Svf f;
                        f.setSampleRate (rate);
                        f.set (type, hz, q, gainDb);

                        // Whatever it fell back to, it has to say so, because
                        // every tolerance below is a fraction of it.
                        const double actualRate = f.getSampleRate();

                        const std::string what = std::string (nameOf (type))
                            + " at " + show (hz) + " hz, rate " + show (rate)
                            + ", Q " + show (q) + ", " + show (gainDb) + " dB";

                        if (! (actualRate > 0.0) || ! std::isfinite (actualRate))
                        {
                            fail (what + ": the rate it kept is not usable");
                            return;
                        }

                        const auto c = f.coefficients();

                        if (! (std::isfinite (c.a1) && std::isfinite (c.a2)
                               && std::isfinite (c.a3) && std::isfinite (c.m0)
                               && std::isfinite (c.m1) && std::isfinite (c.m2)))
                        {
                            fail (what + ": coefficients stopped being finite");
                            return;
                        }

                        // The invariant. a1 > 0 and a2 > 0 and a3 > 0 is g
                        // strictly positive and finite, said in terms of what
                        // the filter actually holds.
                        if (! (c.a1 > 0.0 && c.a2 > 0.0 && c.a3 > 0.0))
                        {
                            fail (what + ": the prewarped corner is not strictly"
                                  " positive, a1 " + show (c.a1) + ", a2 "
                                  + show (c.a2) + ", a3 " + show (c.a3));
                            return;
                        }

                        // A cross-check rather than the guarantee, and held to
                        // a nanometre of the circle rather than to the circle,
                        // because at these settings the measurement is the
                        // ill-conditioned part. A corner sitting on the one
                        // hertz floor at a rate of a gigahertz gives a `g` of
                        // three nanounits, and the discriminant that decides
                        // whether the poles are a real pair or a complex one is
                        // then two nearly equal numbers subtracted: it reads a
                        // modulus of exactly one for a filter whose poles are
                        // comfortably inside. The sharp instrument here is the
                        // strictly positive test above, which is exact.
                        const double pole = largestPoleModulus (f);

                        if (! (pole < 1.0 + 1.0e-9))
                        {
                            fail (what + ": pole outside the unit circle, modulus "
                                  + show (pole));
                            return;
                        }

                        worstPole = std::max (worstPole, pole);

                        // The two answers still agree. Compared absolutely
                        // rather than in decibels on purpose: the failure
                        // these clamps prevent is a drawn unity against a
                        // real nothing, and a ratio of those is a number no
                        // tolerance can read.
                        // Fractions of the rate it kept, never a frequency in
                        // hertz. Above Nyquist the two are not comparing the
                        // same thing and are not meant to: the drawn curve
                        // holds the top of its own range, while the state
                        // space form walks `z` round the circle and comes
                        // back to DC, which is an honest answer to a question
                        // nobody should ask.
                        for (double at : { 0.0, actualRate * 0.001, actualRate * 0.01,
                                           actualRate * 0.2, actualRate * 0.4998 })
                        {
                            const double drawn = f.magnitudeAt (at);
                            const double real = magnitudeAt (f, at, actualRate);

                            if (! (std::isfinite (drawn) && drawn >= 0.0))
                            {
                                fail (what + ": the drawn curve at " + show (at)
                                      + " hz is " + show (drawn));
                                return;
                            }

                            const double off = std::fabs (drawn - real);

                            if (! (off <= 1.0e-4 * (1.0 + std::fabs (real))))
                            {
                                fail (what + ": drawn " + show (drawn)
                                      + " against real " + show (real) + " at "
                                      + show (at) + " hz");
                                return;
                            }

                            worstCurve = std::max (worstCurve,
                                                   off / (1.0 + std::fabs (real)));
                        }

                        for (int n = 0; n < 400; ++n)
                        {
                            const double y = f.process ((double) (n % 7) * 0.3 - 1.0);

                            if (! std::isfinite (y))
                            {
                                fail (what + ": the output stopped being finite at "
                                      + std::to_string (n));
                                return;
                            }
                        }

                        ++settings;
                    }

    std::printf ("    %d settings, worst pole modulus %s, worst curve disagreement %s\n",
                 settings, show (worstPole).c_str(), show (worstCurve).c_str());
}

//==============================================================================
int main()
{
    std::printf ("Fx::Svf\n\n");

    lowpassAndHighpassEnds();
    bandpassGains();
    notchIsADeepHole();
    allpassIsFlat();
    bellReachesItsGain();
    bellWidthDoesNotMoveWithGain();
    shelfSitsWhereItWasPut();
    shelvesLandOnTheirGains();
    neutralAtZeroGain();
    structuralIdentityOnSamples();
    processMatchesItsStateSpace();
    stableEverywhere();
    outOfRangeSettingsAreHeldIn();
    resetClearsTheTail();

    theBellAndShelvesAreTheCookbook();
    theNearNyquistErrorIsAKnownQuantity();
    theEndsOfTheBandAreExact();
    boostAndCutMirrorExactly();
    aSmallBellHasNoThreeDecibelPoint();
    theFiltersOwnCurveIsTheRealOne();

    theRateCanChangeUnderAFilterThatIsAlreadySet();
    anUnconfiguredFilterIsWhatItsDefaultsDescribe();
    theMatchedBellSaysWhichDesignItIsRunning();
    aCornerAboveTheCeilingIsTheCeiling();
    nonsenseStillLeavesAFilterThatRuns();

    std::printf ("\n");

    if (failures > 0)
    {
        std::printf ("%d check(s) failed.\n", failures);
        return 1;
    }

    std::printf ("All checks passed.\n");
    return 0;
}
