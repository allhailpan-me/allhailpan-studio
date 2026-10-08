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

#include <complex>
#include <cstdio>
#include <cmath>
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
        const auto s = stateSpace (f.coefficients());
        const double trace = s.a11 + s.a22;
        const double det = s.a11 * s.a22 - s.a12 * s.a21;
        const double disc = trace * trace - 4.0 * det;

        double largest = 0.0;

        if (disc >= 0.0)
        {
            const double root = std::sqrt (disc);
            largest = std::max (std::fabs ((trace + root) * 0.5),
                                std::fabs ((trace - root) * 0.5));
        }
        else
        {
            largest = std::sqrt (det);   // complex pair, modulus is sqrt(det)
        }

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

    std::printf ("\n");

    if (failures > 0)
    {
        std::printf ("%d check(s) failed.\n", failures);
        return 1;
    }

    std::printf ("All checks passed.\n");
    return 0;
}
