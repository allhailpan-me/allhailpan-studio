// Checks Source/ClipFade.h.
//
// There are four claims in that header and they are all about what the audio
// does, not about what the formulas look like, so this file measures rather
// than restates:
//
//  1. An equal gain crossfade leaves phase coherent material exactly as it
//     was, and an equal power one leaves uncorrelated material at exactly the
//     level it was. Both are checked by running signals through two real
//     windows, one clip's fade out against the next clip's fade in, and
//     measuring the result. Asserting `sin^2 + cos^2 == 1` would restate the
//     identity; measuring a splice proves the identity is wired to the audio.
//     Both failures are measured too: equal gain dips 3.01 dB on uncorrelated
//     material and equal power bulges 3.01 dB on correlated material, which
//     are the two things Avid's manual warns about.
//
//  2. The de-click fade suppresses the energy an edge adds at the rate the
//     shape's smoothness predicts: 12 dB per octave for a linear ramp and 18
//     for the raised cosine, which is Harris 1978 Table I applied to the two
//     joins of a fade. That is the whole justification for the de-click shape
//     not being the project's default shape, so it is measured with a
//     transform and a line fit rather than cited. It is also the only answer
//     this project has to "how long should a de-click fade be", since no
//     standard exists and every published figure is a vendor default.
//
//  3. A fade length is a time anchored to its edge and is never rescaled.
//     Trimming a clip must leave the gain at every point of the surviving
//     fade exactly as it was. A fade stored as a fraction of the clip would
//     pass every other check in this file and fail that one immediately,
//     which is why it is here: the research found that no studio stores a
//     fraction and that it is the plausible and wrong choice.
//
//  4. No sample is ever in both ramps, whatever is asked for. Ardour's
//     library layer clamps each fade independently and then applies both to
//     the same samples where they meet, so this is a real failure with a real
//     precedent rather than a hypothetical one.
//
// Randomised over thousands of generated geometries where the property allows
// it, because that has repeatedly found more here than hand-picked cases.

#include "ClipFade.h"
#include "RealFFT.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void fail (const std::string& what)
{
    std::printf ("FAIL  %s\n", what.c_str());
    ++failures;
}

void check (bool ok, const std::string& what)
{
    if (! ok)
        fail (what);
}

void near (double got, double want, double tol, const std::string& what)
{
    if (! (std::abs (got - want) <= tol))
        fail (what + ": got " + std::to_string (got) + " wanted " + std::to_string (want)
              + " (tolerance " + std::to_string (tol) + ")");
}

const char* shapeName (ClipFade::Shape s)
{
    switch (s)
    {
        case ClipFade::Shape::linear:     return "linear";
        case ClipFade::Shape::equalPower: return "equalPower";
        case ClipFade::Shape::sCurve:     return "sCurve";
    }
    return "?";
}

const ClipFade::Shape allShapes[] = { ClipFade::Shape::linear,
                                      ClipFade::Shape::equalPower,
                                      ClipFade::Shape::sCurve };

constexpr double pi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// 1. The shapes themselves: endpoints, range, monotonicity, and agreement
//    with the closed form each one is named by.
//
// Against the closed form and not against another implementation of itself,
// which is the lesson WaveBankTest already paid for. Ardour's own values come
// from a constrained cubic spline through 33 sampled points, so bit identity
// with Ardour is not expected and is not asserted; what is asserted is the
// function Ardour's breakpoints are sampled from.

void testShapes()
{
    for (auto s : allShapes)
    {
        const std::string name = shapeName (s);

        // Exactly, not nearly. A de-click fade whose silent end is 1e-9
        // rather than 0 leaves a step of 1e-9, which is inaudible, but a fade
        // whose unity end is 0.9999 is a gain error on every clip in the
        // session and would be invisible to every other check here.
        check (ClipFade::risingGain (s, 0.0) == 0.0, name + ": g(0) is exactly zero");
        check (ClipFade::risingGain (s, 1.0) == 1.0, name + ": g(1) is exactly one");

        // Ardour runs its fades between 1e-7 and 1 rather than 0 and 1,
        // because `GAIN_COEFF_SMALL` is its silence. Zero is used here
        // instead, deliberately: an edge fade exists to remove a step, and
        // stopping 140 dB short of zero leaves one.

        double previous = -1.0;
        for (int i = 0; i <= 200000; ++i)
        {
            const double x = (double) i / 200000.0;
            const double g = ClipFade::risingGain (s, x);

            check (g >= 0.0 && g <= 1.0, name + ": stays inside zero and one");
            check (g >= previous, name + ": never falls");
            previous = g;

            double want = x;
            if (s == ClipFade::Shape::equalPower) want = std::sin (x * pi / 2.0);
            if (s == ClipFade::Shape::sCurve)     want = 0.5 * (1.0 - std::cos (x * pi));

            if (std::abs (g - want) > 1.0e-15)
            {
                fail (name + ": disagrees with its closed form at x = " + std::to_string (x));
                break;
            }
        }

        // Positions outside the ramp are clamped rather than extrapolated. A
        // clip whose window has been rounded by a hair must not read a gain
        // above unity or below zero.
        check (ClipFade::risingGain (s, -3.0) == 0.0, name + ": clamps below zero");
        check (ClipFade::risingGain (s,  7.0) == 1.0, name + ": clamps above one");

        // A non-finite position is silence rather than a NaN that would poison
        // the insert buffer and everything sending off it.
        check (ClipFade::risingGain (s, std::nan ("")) == 0.0, name + ": NaN is silence");
        check (ClipFade::risingGain (s, std::numeric_limits<double>::infinity()) == 0.0,
               name + ": infinity is silence");
    }

    // The published midpoints, to the figures the manuals print.
    near (ClipFade::risingGain (ClipFade::Shape::linear, 0.5), 0.5, 0.0,
          "linear midpoint is exactly a half");
    near (ClipFade::risingGain (ClipFade::Shape::equalPower, 0.5), 0.70710678118654752, 1.0e-15,
          "equal power midpoint is one over root two");
    near (ClipFade::risingGain (ClipFade::Shape::sCurve, 0.5), 0.5, 1.0e-15,
          "raised cosine midpoint is exactly a half");

    // -6.0206 dB and -3.0103 dB, which is how Ardour's manual states them.
    near (20.0 * std::log10 (ClipFade::risingGain (ClipFade::Shape::linear, 0.5)), -6.0206, 1.0e-4,
          "linear midpoint is -6.0206 dB");
    near (20.0 * std::log10 (ClipFade::risingGain (ClipFade::Shape::equalPower, 0.5)), -3.0103, 1.0e-4,
          "equal power midpoint is -3.0103 dB");

    // The order of each join, which is the entire content of the table in the
    // header and the whole reason the de-click shape is not the default shape.
    // The energy a join adds falls at 6(n + 1) dB per octave for a
    // discontinuity in the nth derivative, so what the table is really saying
    // is which of the two joins are flat and which are not. Read as a one
    // sided difference, because the claim is about the order of the join and
    // not about the value of a derivative.
    //
    // This is also the one check in the file that tells sin(x * pi/2) from
    // sqrt(x), and that gap is wider than it looks. sqrt(x) runs from 0 to 1,
    // is monotone, passes through one over root two at the midpoint, and
    // satisfies g(x)^2 + g(1-x)^2 = 1 exactly, because x + (1 - x) = 1. So it
    // answers every property this file asserts about an equal power fade, and
    // it is only the flat join at the unity end that it does not have. That
    // is the shape of the fault PanLaw's test shipped with: the properties of
    // every law pinned and the shape of none of them.
    struct Join { ClipFade::Shape shape; bool flatAtSilence, flatAtUnity; };
    const Join joins[] =
    {
        { ClipFade::Shape::linear,     false, false },
        { ClipFade::Shape::equalPower, false, true  },
        { ClipFade::Shape::sCurve,     true,  true  }
    };

    for (const auto& j : joins)
    {
        const double h = 1.0e-6;
        const double atSilence = (ClipFade::risingGain (j.shape, h)
                                  - ClipFade::risingGain (j.shape, 0.0)) / h;
        const double atUnity   = (ClipFade::risingGain (j.shape, 1.0)
                                  - ClipFade::risingGain (j.shape, 1.0 - h)) / h;

        std::printf ("  %-11s join slope %.6f at silence, %.6f at unity\n",
                     shapeName (j.shape), atSilence, atUnity);

        // A flat join is one whose slope goes to zero with the step, so it is
        // judged against the step and not against a fixed small number: the
        // two flat joins here read about 1e-6 at a step of 1e-6, and sqrt's
        // unity end reads a half whatever the step is.
        check (j.flatAtSilence ? atSilence < 1.0e-4 : atSilence > 0.5,
               std::string (shapeName (j.shape)) + ": the join at silence has the order the table claims");
        check (j.flatAtUnity ? atUnity < 1.0e-4 : atUnity > 0.5,
               std::string (shapeName (j.shape)) + ": the join at unity has the order the table claims");
    }
}

// ---------------------------------------------------------------------------
// 2. The two laws, as identities over the pair.
//
// The falling half of a ramp is the rising half read backwards, so the law is
// a statement about g(x) and g(1 - x). These hold exactly rather than
// approximately, which is the whole reason there is one formula per shape
// instead of two.

void testLaws()
{
    double worstGain = 0.0, worstPower = 0.0, worstSPower = 0.0;

    for (int i = 0; i <= 100000; ++i)
    {
        const double x = (double) i / 100000.0;

        const double la = ClipFade::risingGain (ClipFade::Shape::linear, x);
        const double lb = ClipFade::risingGain (ClipFade::Shape::linear, 1.0 - x);
        worstGain = std::max (worstGain, std::abs (la + lb - 1.0));

        const double pa = ClipFade::risingGain (ClipFade::Shape::equalPower, x);
        const double pb = ClipFade::risingGain (ClipFade::Shape::equalPower, 1.0 - x);
        worstPower = std::max (worstPower, std::abs (pa * pa + pb * pb - 1.0));

        const double sa = ClipFade::risingGain (ClipFade::Shape::sCurve, x);
        const double sb = ClipFade::risingGain (ClipFade::Shape::sCurve, 1.0 - x);
        worstSPower = std::max (worstSPower, std::abs (sa + sb - 1.0));
    }

    check (worstGain  <= 1.0e-15, "linear is equal gain to float precision");
    check (worstPower <= 1.0e-15, "equal power is equal power to float precision");

    // The raised cosine is an equal gain law too, which is the second reason
    // it is the de-click shape and was not obvious: a cut joins two halves of
    // one waveform, which is the phase coherent case Avid's rule sends to
    // equal gain.
    check (worstSPower <= 1.0e-15, "the raised cosine is an equal gain law");

    std::printf ("  law residuals: linear %.3e  equalPower %.3e  sCurve %.3e\n",
                 worstGain, worstPower, worstSPower);

    // And the raised cosine is NOT equal power, which is worth pinning so
    // that nobody later assumes "smoother" means "holds power". Uncorrelated
    // material through it dips by the same 3.01 dB a linear crossfade does.
    const double s = ClipFade::risingGain (ClipFade::Shape::sCurve, 0.5);
    near (s * s + s * s, 0.5, 1.0e-15, "the raised cosine is not equal power");
}

// ---------------------------------------------------------------------------
// 3. The crossfade, measured on signals through two real windows.
//
// Two clips abutting with an overlap: clip A ends at `join + half` with a
// fade out `overlap` long, clip B starts at `join - half` with a fade in the
// same length, so the two ramps cover exactly the same span. That is what a
// crossfade is here, because `renderAudioClips` sums overlapping clips on a
// track into the same insert, and it is what the engine will actually do.

struct Pair
{
    ClipFade::Window a, b;      // the outgoing clip and the incoming one
    double from = 0.0, to = 0.0;
};

Pair crossfadeAt (double overlapBeats, ClipFade::Shape shape)
{
    const double bps = 2.0;                 // 120 bpm, so beats and seconds differ
    const double overlapSeconds = overlapBeats / bps;

    // The outgoing clip runs from 0 to `overlapBeats` and fades out across the
    // whole of it. The incoming clip runs from 0 to `overlapBeats` too and
    // fades in across the whole of it. Built through `resolve` and
    // `windowFor`, so the geometry the engine computes is what is measured.
    const auto ra = ClipFade::resolve (overlapSeconds, { 0.0, overlapSeconds, shape, shape }, 0.0);
    const auto rb = ClipFade::resolve (overlapSeconds, { overlapSeconds, 0.0, shape, shape }, 0.0);

    Pair p;
    p.a = ClipFade::windowFor (0.0, overlapBeats, ra, bps);
    p.b = ClipFade::windowFor (0.0, overlapBeats, rb, bps);
    p.from = 0.0;
    p.to   = overlapBeats;
    return p;
}

void testCorrelatedCrossfade()
{
    const double overlap = 3.0;
    const int    steps   = 20001;

    for (auto shape : allShapes)
    {
        const auto p = crossfadeAt (overlap, shape);
        const bool equalGain = shape != ClipFade::Shape::equalPower;

        // The same signal on both sides, which is what splicing a drum loop
        // against a copy of itself is, and what the two halves of a cut
        // always are. Phase coherent, so the two gains add as amplitudes.
        double worst = 0.0, peak = 0.0;
        for (int i = 0; i < steps; ++i)
        {
            const double beat = p.from + (p.to - p.from) * (double) i / (double) (steps - 1);
            const double sum  = ClipFade::gainAt (p.a, beat) + ClipFade::gainAt (p.b, beat);
            worst = std::max (worst, std::abs (sum - 1.0));
            peak  = std::max (peak, sum);
        }

        if (equalGain)
        {
            // Bit transparent: the splice is inaudible. This is the entire
            // reason equal gain exists and there is no statistics in it.
            check (worst <= 1.0e-12,
                   std::string (shapeName (shape)) + ": correlated material crosses transparently");
            std::printf ("  %-11s correlated: worst amplitude error %.3e\n", shapeName (shape), worst);
        }
        else
        {
            // The clipping Avid warns about, measured rather than assumed.
            near (peak, 1.4142135623730951, 1.0e-9,
                  "equal power bulges to root two on correlated material");
            near (20.0 * std::log10 (peak), 3.0103, 1.0e-4,
                  "equal power bulges by 3.0103 dB on correlated material");
            std::printf ("  %-11s correlated: peaks at %.10f (+%.4f dB)\n",
                         shapeName (shape), peak, 20.0 * std::log10 (peak));
        }
    }
}

void testUncorrelatedCrossfade()
{
    // Two independent noise sources. A single reading of a noise power is a
    // random quantity with as much spread as it has size, which is the trap
    // Fx/Tilt.h's header records, so the power at each position through the
    // crossfade is averaged over many independent draws rather than read once.
    const double overlap = 3.0;
    const int    positions = 65;
    const int    draws     = 20000;

    std::mt19937 rng (20261015u);
    std::normal_distribution<double> noise (0.0, 1.0);

    for (auto shape : allShapes)
    {
        const auto p = crossfadeAt (overlap, shape);

        double worstFlat = 0.0;
        double midPower  = 0.0;

        for (int i = 0; i < positions; ++i)
        {
            const double u    = (double) i / (double) (positions - 1);
            const double beat = p.from + (p.to - p.from) * u;
            const double ga   = ClipFade::gainAt (p.a, beat);
            const double gb   = ClipFade::gainAt (p.b, beat);

            double sumSquares = 0.0;
            for (int d = 0; d < draws; ++d)
            {
                const double y = ga * noise (rng) + gb * noise (rng);
                sumSquares += y * y;
            }
            const double measured = sumSquares / (double) draws;

            // What the law predicts: the two sources are independent, so the
            // powers add and the output mean square is ga^2 + gb^2.
            const double predicted = ga * ga + gb * gb;

            // The measurement is checked against the law before the law is
            // checked against anything, so a broken measurement cannot pass
            // for a broken filter. Twenty thousand draws gives a standard
            // error of about one per cent of the mean.
            near (measured, predicted, 0.06 * std::max (0.2, predicted),
                  std::string (shapeName (shape)) + ": measured power follows the gains");

            worstFlat = std::max (worstFlat, std::abs (predicted - 1.0));
            if (i == (positions - 1) / 2)
                midPower = predicted;
        }

        if (shape == ClipFade::Shape::equalPower)
        {
            check (worstFlat <= 1.0e-12,
                   "equal power holds uncorrelated power flat across the whole crossfade");
            near (midPower, 1.0, 1.0e-12, "equal power midpoint holds unity power");
            std::printf ("  %-11s uncorrelated: worst power deviation %.3e\n",
                         shapeName (shape), worstFlat);
        }
        else
        {
            // The dip Avid warns about and Apple's EqP exists to avoid,
            // measured: exactly a half, which is -3.0103 dB.
            near (midPower, 0.5, 1.0e-12,
                  std::string (shapeName (shape)) + ": dips to half power at the midpoint");
            near (10.0 * std::log10 (midPower), -3.0103, 1.0e-4,
                  std::string (shapeName (shape)) + ": dips by 3.0103 dB at the midpoint");
            std::printf ("  %-11s uncorrelated: midpoint power %.10f (%.4f dB)\n",
                         shapeName (shape), midPower, 10.0 * std::log10 (midPower));
        }
    }
}

// ---------------------------------------------------------------------------
// 4. The de-click measurement.
//
// The claim being checked is the one piece of arithmetic in the header that is
// derived rather than quoted, so it is the one that most needs measuring. A
// clip's edge applied to a signal that is otherwise constant is a window
// function, and the energy its boundary adds falls off at a rate set by the
// lowest discontinuous derivative of that boundary: Harris 1978 Table I gives
// -6 dB per octave for the rectangle, -12 where the discontinuity is in the
// first derivative and -18 where it is in the second.
//
// Two traps this project has already paid for apply. The analysis window's own
// sidelobes are the floor under every measurement behind it, which is why
// there is no analysis window here at all: the signal under test IS the
// window, transformed whole over an exact number of its own samples, so there
// is nothing in front of it to measure instead of it. And band energy is read
// as a mean power density per octave band and fitted as a line in log
// frequency, reporting the scatter as well as the slope, rather than read from
// two points.

/** The decay of the energy an edge adds, in decibels per octave.

    The window under test is a block of unity `blockSamples` long, ramped up
    over `rampSamples` at its start and down over `rampSamples` at its end, and
    then zero padded into a transform four times longer.

    The padding is what makes the measurement possible, and the reason is the
    bin spacing rather than anything about periodicity: an n point transform is
    already exact samples of the block's own spectrum, but transformed over
    exactly its own length the bins land on the spectrum's nulls. An all ones
    block of exactly the transform length then has every bin but direct current
    at zero, so a hard cut could not be measured at all, and each ramped case
    would yield one usable band rather than five. Padded four times over, the
    same code measures a hard cut and a ramp alike.

    `rampSamples` of zero is a plain rectangle, which is the thing all this
    exists to improve on.
*/
double decayPerOctave (ClipFade::Shape shape, int rampSamples, double& scatterDb)
{
    const int n     = 1 << 16;
    const int block = n / 4;
    RealFFT fft (n);

    std::vector<float> signal ((size_t) n, 0.0f);
    for (int i = 0; i < block; ++i)
    {
        // The worst case for a click: a clip cut where the waveform is
        // sitting at full scale.
        double g = 1.0;
        if (rampSamples > 0)
        {
            if (i < rampSamples)
                g = ClipFade::risingGain (shape, ((double) i + 0.5) / (double) rampSamples);
            else if (i >= block - rampSamples)
                g = ClipFade::risingGain (shape, ((double) (block - i) - 0.5) / (double) rampSamples);
        }
        signal[(size_t) i] = (float) g;
    }

    std::vector<std::complex<double>> bins ((size_t) fft.getNumBins());
    fft.forward (signal.data(), bins.data());

    // Octave bands above the corner, which a ramp puts at about one over its
    // own length and a rectangle puts at one over the block. Below the corner
    // is the main lobe and there is nothing to measure there.
    const int corner   = rampSamples > 0 ? rampSamples : block;
    const int firstBin = std::max (4, 4 * n / corner);

    std::vector<double> x, y;
    for (int lo = firstBin; lo * 2 <= n / 2; lo *= 2)
    {
        double power = 0.0;
        int    count = 0;
        for (int k = lo; k < lo * 2; ++k)
        {
            power += std::norm (bins[(size_t) k]);
            ++count;
        }
        if (count == 0)
            continue;

        x.push_back (std::log2 ((double) lo * 1.5));            // band centre
        y.push_back (10.0 * std::log10 (power / (double) count + 1.0e-300));
    }

    // Fewer than three bands is not a measurement. Returning a slope of zero
    // would read as a pass against any upper bound, which is exactly how a
    // check comes to assert nothing, so this says so instead.
    if (x.size() < 3)
    {
        scatterDb = 0.0;
        return std::numeric_limits<double>::quiet_NaN();
    }

    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    const double m = (double) x.size();
    for (size_t i = 0; i < x.size(); ++i)
    {
        sx += x[i]; sy += y[i];
        sxx += x[i] * x[i];
        sxy += x[i] * y[i];
    }
    const double slope     = (m * sxy - sx * sy) / (m * sxx - sx * sx);
    const double intercept = (sy - slope * sx) / m;

    double worst = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
        worst = std::max (worst, std::abs (y[i] - (slope * x[i] + intercept)));
    scatterDb = worst;

    return slope;
}

void testDeclickDecay()
{
    // The order of the lowest discontinuous derivative of each ramp at the end
    // where it meets silence, which is the end that governs: the energy falls
    // at 6(n + 1) dB per octave.
    struct Expectation { ClipFade::Shape shape; double perOctave; };
    const Expectation expected[] =
    {
        { ClipFade::Shape::linear,     -12.0 },   // slope jumps at both joins
        { ClipFade::Shape::equalPower, -12.0 },   // slope jumps where it meets silence
        { ClipFade::Shape::sCurve,     -18.0 }    // slope is zero at both joins
    };

    for (const auto& e : expected)
    {
        double scatter = 0.0;
        const double slope = decayPerOctave (e.shape, 256, scatter);

        std::printf ("  %-11s de-click decay %.2f dB/octave (wanted %.1f, scatter %.2f dB)\n",
                     shapeName (e.shape), slope, e.perOctave, scatter);

        check (std::isfinite (slope),
               std::string (shapeName (e.shape)) + ": the decay measurement produced a slope at all");

        // Two decibels an octave of tolerance over five octaves. That is tight
        // enough to tell 12 from 18, which is the only distinction the choice
        // of de-click shape rests on, and loose enough not to be a measurement
        // of the fit rather than of the filter.
        near (slope, e.perOctave, 2.0,
              std::string (shapeName (e.shape)) + ": decays at the rate its smoothness predicts");

        check (scatter <= 2.5,
               std::string (shapeName (e.shape)) + ": decay is a straight line in log frequency");
    }

    // And the thing the choice is for, stated as a comparison rather than as
    // two absolute numbers: the raised cosine really is steeper than the
    // project's default shape at suppressing an edge, which is why the
    // de-click fade does not simply use the default.
    double sa = 0.0, sb = 0.0;
    const double defaultSlope = decayPerOctave (ClipFade::defaultShape, 256, sa);
    const double declickSlope = decayPerOctave (ClipFade::declickShape, 256, sb);
    check (declickSlope < defaultSlope - 3.0,
           "the de-click shape suppresses an edge faster than the default shape");

    // The thing being compared against: a plain rectangle, whose boundary is
    // a step in the value itself, falls off at 6 dB per octave. That is
    // Harris's first row, and it is why a cut is audible right across the
    // band however loud the music is.
    double scatter = 0.0;
    const double cut = decayPerOctave (ClipFade::Shape::linear, 0, scatter);
    std::printf ("  hard cut    %.2f dB/octave (wanted -6.0, scatter %.2f dB)\n", cut, scatter);
    check (std::isfinite (cut), "the hard cut measurement produced a slope at all");
    near (cut, -6.0, 2.0, "a hard cut decays at only 6 dB per octave");

    // And the fade really is an improvement on it, by the order the shape's
    // smoothness predicts, which is the claim in one line.
    check (declickSlope < cut - 9.0, "the de-click fade suppresses an edge far faster than a cut");
}

// ---------------------------------------------------------------------------
// 5. Resolution: what a clip asks for against what its length can carry.

void testResolveProperties()
{
    std::mt19937 rng (7u);
    std::uniform_real_distribution<double> len (0.0005, 40.0);
    std::uniform_real_distribution<double> ask (-1.0, 45.0);
    std::uniform_real_distribution<double> autoLen (0.0, 0.05);
    std::uniform_int_distribution<int>     shape (0, ClipFade::numShapes - 1);

    int exactlyHonoured = 0, splitBetween = 0, declicked = 0, declickedOut = 0;

    for (int trial = 0; trial < 200000; ++trial)
    {
        const double clipSeconds = len (rng);
        const double autoSeconds = autoLen (rng);
        ClipFade::Request r;
        r.in       = ask (rng);
        r.out      = ask (rng);
        r.inShape  = ClipFade::shapeFromId (shape (rng));
        r.outShape = ClipFade::shapeFromId (shape (rng));

        const auto got = ClipFade::resolve (clipSeconds, r, autoSeconds);

        check (got.in >= 0.0 && got.out >= 0.0, "a resolved fade is never negative");
        check (std::isfinite (got.in) && std::isfinite (got.out), "a resolved fade is finite");

        // The invariant everything else rests on: the two together fit, so no
        // sample can be in both ramps.
        check (got.in + got.out <= clipSeconds * (1.0 + 1.0e-12) + 1.0e-15,
               "the two fades fit inside the clip");

        const double wantedIn  = r.in  > 0.0 ? r.in  : autoSeconds;
        const double wantedOut = r.out > 0.0 ? r.out : autoSeconds;

        if (wantedIn + wantedOut <= clipSeconds)
        {
            // It fits, so it is honoured exactly. Nothing is scaled, ever: a
            // fade length is a time, and a studio that quietly shortened one
            // that fits would make trimming change how a fade sounds.
            near (got.in,  wantedIn,  0.0, "a fade that fits is honoured exactly");
            near (got.out, wantedOut, 0.0, "a fade that fits is honoured exactly");
            ++exactlyHonoured;
        }
        else
        {
            // It does not fit, so the clip is split between the two and they
            // meet at exactly one instant.
            near (got.in + got.out, clipSeconds, 1.0e-12 * std::max (1.0, clipSeconds),
                  "an impossible pair is split to meet exactly");
            ++splitBetween;

            // And the share each end gets is in proportion to what that end
            // could have carried on its own, which is not the same thing as
            // what it asked for. An edge cannot buy more than the whole clip,
            // so once an ask is past the clip length it stops carrying any
            // information about the ratio: two ends that each ask for more
            // than the clip split it evenly however unequal the asks were.
            //
            // This is here because adding up to the clip length is a very weak
            // thing to pin. Dropping the two `std::min` clamps in `resolve`
            // leaves a split that still adds up, still meets exactly, is still
            // symmetric and is still continuous in the clip length, and only
            // the ratio moves: a one second clip asking for five seconds in
            // and half a second out goes from two thirds and one third to ten
            // elevenths and one eleventh. Nothing else in this file could see
            // that.
            const double canIn  = std::min (wantedIn,  clipSeconds);
            const double canOut = std::min (wantedOut, clipSeconds);
            near (got.in / (got.in + got.out), canIn / (canIn + canOut), 1.0e-12,
                  "the split is in proportion to what each end can carry");
        }

        // An edge that asked for nothing takes the de-click fade and the
        // de-click shape with it. The stored shape belongs to a drawn fade,
        // and an edge with no drawn fade has no opinion about it.
        if (r.in <= 0.0 && autoSeconds > 0.0)
        {
            check (got.inShape == ClipFade::declickShape,
                   "an automatic fade uses the de-click shape");
            ++declicked;
        }
        if (r.in > 0.0 && wantedIn + wantedOut <= clipSeconds)
            check (got.inShape == r.inShape, "a drawn fade keeps the shape it was given");

        // Both ends, not one. The two edges are written by two separate pairs
        // of lines in `resolve`, so checking only the fade in leaves the fade
        // out's half of it unasserted, and an automatic fade out taking the
        // default shape instead of the de-click shape is exactly the 6 dB per
        // octave the de-click shape exists to buy.
        if (r.out <= 0.0 && autoSeconds > 0.0)
        {
            check (got.outShape == ClipFade::declickShape,
                   "an automatic fade out uses the de-click shape too");
            ++declickedOut;
        }
        if (r.out > 0.0 && wantedIn + wantedOut <= clipSeconds)
            check (got.outShape == r.outShape, "a drawn fade out keeps the shape it was given");
    }

    check (exactlyHonoured > 1000 && splitBetween > 1000 && declicked > 1000 && declickedOut > 1000,
           "the randomised sweep reached all three outcomes at both ends");
    std::printf ("  resolve: %d honoured exactly, %d split, %d de-clicked in, %d de-clicked out\n",
                 exactlyHonoured, splitBetween, declicked, declickedOut);

    // The boundary between honouring a pair and splitting it. The two fades
    // touching is legal and overlapping is not, so this is the one comparison
    // in `resolve` that could be written either way, and written as `>=` the
    // split runs on a pair that already fits: it returns the same two lengths
    // through a multiply and a divide, which lands one or two ulps away from
    // them. Nothing audible, but the clip's fade handles are drawn from these
    // numbers, and a length that comes back changed from what was stored is a
    // handle that creeps every time the clip is redrawn.
    {
        std::mt19937 fitRng (4242u);
        std::uniform_real_distribution<double> part (0.01, 20.0);
        int changed = 0;

        for (int trial = 0; trial < 50000; ++trial)
        {
            const double a = part (fitRng), b = part (fitRng);
            const auto got = ClipFade::resolve (a + b, { a, b }, 0.004);
            if (! (got.in == a && got.out == b))
                ++changed;
        }

        check (changed == 0, "a pair that exactly fills the clip is honoured bit for bit");
        std::printf ("  pairs exactly filling their clip: %d of 50000 came back changed\n", changed);
    }

    // The two ends asking for wildly different impossible lengths, which is
    // what the proportion above is really about, printed so the rule is
    // readable rather than only asserted.
    {
        const auto both = ClipFade::resolve (1.0, { 60.0, 3600.0 }, 0.0);
        near (both.in,  0.5, 1.0e-12, "two impossible asks split the clip evenly");
        near (both.out, 0.5, 1.0e-12, "two impossible asks split the clip evenly");

        const auto one = ClipFade::resolve (1.0, { 5.0, 0.5 }, 0.0);
        near (one.in / one.out, 2.0, 1.0e-9,
              "an ask past the clip length carries no ratio past it either");
        std::printf ("  resolve(1 s, {5 s, 0.5 s}) = {%.6f, %.6f};  "
                     "resolve(1 s, {60 s, 3600 s}) = {%.6f, %.6f}\n",
                     one.in, one.out, both.in, both.out);
    }

    // Zero turns the automatic fade off, which is Reaper's own convention for
    // the same field.
    const auto off = ClipFade::resolve (10.0, {}, 0.0);
    check (off.in == 0.0 && off.out == 0.0, "an automatic length of zero means no fade");

    // And a negative or non-finite one does too rather than becoming a fade of
    // a strange length.
    for (double bad : { -1.0, std::nan (""), -std::numeric_limits<double>::infinity() })
    {
        const auto r = ClipFade::resolve (10.0, {}, bad);
        check (r.in == 0.0 && r.out == 0.0, "a nonsense automatic length means no fade");
    }

    // A clip with no length carries no fades rather than a window that could
    // read outside itself, and it leaves both shapes at the default rather
    // than at whatever was asked for, because there is nothing left for a
    // shape to describe. Asked for with two shapes that are both something
    // other than the default, which `testShapeIds` pins is true of these two.
    for (double bad : { 0.0, -5.0, std::nan ("") })
    {
        const auto r = ClipFade::resolve (bad, { 1.0, 1.0,
                                                 ClipFade::Shape::linear,
                                                 ClipFade::Shape::sCurve }, 0.004);
        check (r.in == 0.0 && r.out == 0.0, "a clip with no length carries no fades");
        check (r.inShape == ClipFade::defaultShape && r.outShape == ClipFade::defaultShape,
               "a clip with no length leaves both shapes at the default");
    }

    // A fade asked for in nonsense is read as "no fade of my own" and takes
    // the automatic one, which is the right answer for a file that has written
    // rubbish into it.
    const auto nonsense = ClipFade::resolve (10.0, { std::nan (""), -4.0 }, 0.004);
    near (nonsense.in,  0.004, 0.0, "a NaN fade length falls back to the automatic one");
    near (nonsense.out, 0.004, 0.0, "a negative fade length falls back to the automatic one");
}

// ---------------------------------------------------------------------------
// 6. The window, and the two properties that are about geometry rather than
//    about sound.

void testWindowGeometry()
{
    std::mt19937 rng (99u);
    std::uniform_real_distribution<double> startD (-50.0, 50.0);
    std::uniform_real_distribution<double> lenD (0.001, 30.0);
    std::uniform_real_distribution<double> bpmD (20.0, 300.0);
    std::uniform_real_distribution<double> ask (-1.0, 35.0);
    std::uniform_int_distribution<int>     shape (0, ClipFade::numShapes - 1);

    for (int trial = 0; trial < 200000; ++trial)
    {
        const double bps       = bpmD (rng) / 60.0;
        const double startBeat = startD (rng);
        const double beats     = lenD (rng);
        const double endBeat   = startBeat + beats;
        const double seconds   = beats / bps;

        ClipFade::Request r;
        r.in       = ask (rng);
        r.out      = ask (rng);
        r.inShape  = ClipFade::shapeFromId (shape (rng));
        r.outShape = ClipFade::shapeFromId (shape (rng));

        const auto resolved = ClipFade::resolve (seconds, r, 0.004);
        const auto w        = ClipFade::windowFor (startBeat, endBeat, resolved, bps);

        // No sample is ever in both ramps. Ardour's library layer permits the
        // opposite and then applies both to the same samples, so this is the
        // one property worth asserting structurally rather than inferring.
        check (w.inEnd <= w.outStart, "the two ramps never overlap");

        // Neither ramp reaches outside the clip.
        check (w.inStart == startBeat && w.outEnd == endBeat, "the ramps are anchored to the edges");
        check (w.inEnd >= startBeat && w.inEnd <= endBeat, "the fade in ends inside the clip");
        check (w.outStart >= startBeat && w.outStart <= endBeat, "the fade out starts inside the clip");

        // And each ramp runs forwards. `gainAt` cannot tell a ramp collapsed
        // to a point from one that runs backwards, because it reads both as
        // unity, so this is the only place that distinction gets asserted. A
        // window is handed whole to whatever reads it next and a reversed ramp
        // is not a window.
        check (w.inStart <= w.inEnd,   "the fade in ramp runs forwards");
        check (w.outStart <= w.outEnd, "the fade out ramp runs forwards");

        // The gain is bounded and the ends are exact. A fade truncated mid
        // curve would leave a step at the edge, which is the click the fade
        // was there to prevent: the one real risk in the clamping rule.
        check (ClipFade::gainAt (w, startBeat) == 0.0 || resolved.in == 0.0,
               "a clip with a fade in starts at exactly silence");
        check (ClipFade::gainAt (w, endBeat) == 0.0 || resolved.out == 0.0,
               "a clip with a fade out ends at exactly silence");

        for (int i = 0; i <= 32; ++i)
        {
            const double beat = startBeat + beats * (double) i / 32.0;
            const double g    = ClipFade::gainAt (w, beat);
            check (g >= 0.0 && g <= 1.0, "the fade gain stays inside zero and one");
            check (std::isfinite (g), "the fade gain is finite");
        }
    }

    // A clip with no fades at all is unity everywhere, not nearly unity: a
    // gain of 0.999 on every clip in a session would be a mix that is quietly
    // wrong and nothing would say so.
    const auto none = ClipFade::windowFor (4.0, 20.0, ClipFade::resolve (8.0, {}, 0.0), 2.0);
    for (int i = 0; i <= 1000; ++i)
        check (ClipFade::gainAt (none, 4.0 + 16.0 * (double) i / 1000.0) == 1.0,
               "a clip with no fades is exactly unity everywhere");

    // A degenerate span carries no fades rather than dividing by zero.
    for (auto bad : { std::pair<double, double> { 5.0, 5.0 },
                      std::pair<double, double> { 9.0, 2.0 },
                      std::pair<double, double> { std::nan (""), 4.0 } })
    {
        const auto w = ClipFade::windowFor (bad.first, bad.second,
                                            ClipFade::resolve (1.0, { 0.5, 0.5 }, 0.004), 2.0);

        // The invariant holds with no exception, including for a span whose
        // end is before its start. An invariant with an exception is not one,
        // and this is the invariant that stops a gain being applied twice.
        check (w.inEnd <= w.outStart || std::isnan (w.inEnd) || std::isnan (w.outStart),
               "a degenerate span produces no overlapping ramps");

        // Both ramps collapsed onto the start, which is what the header says
        // the degenerate branch does. Reading it only through `gainAt` cannot
        // see the difference between that and a pair of backwards ramps, since
        // both come out as unity: for a span whose end is before its start,
        // leaving the two ramps to be built normally produces inEnd before
        // inStart and outEnd before outStart, keeps inEnd <= outStart, and
        // reads as unity everywhere, so it passes every other check here.
        check ((w.inStart <= w.inEnd && w.outStart <= w.outEnd)
               || std::isnan (w.inStart) || std::isnan (w.inEnd),
               "a degenerate span leaves both ramps running forwards");

        for (double beat : { -10.0, 0.0, 3.0, 5.0, 12.0 })
        {
            const double g = ClipFade::gainAt (w, beat);
            check (g == 1.0, "a degenerate span is exactly unity everywhere");
        }
    }
}

// ---------------------------------------------------------------------------
// 6b. The two clamps in `windowFor`, which the sweep above cannot reach.
//
// `resolve` is what guarantees the two fades fit, and the sweep above hands
// `windowFor` the output of `resolve` on exactly the matching length, so there
// the clamps to `endBeat` and `startBeat` only ever absorb rounding, and only
// when one fade reaches the whole clip. The sweep never produces that, because
// it runs with the automatic fade on, so the far end always holds back at
// least four milliseconds. Both clamps therefore sit unexercised, and dropping
// either of them passes the whole file. Two separate reasons to reach them
// deliberately.
//
// The engine does not hand `windowFor` a length and a span that agree to the
// bit. `Project::outputSeconds` divides the clip's beats by the tempo and
// `MainComponent` multiplies the fade back by it, so the round trip is two
// roundings in opposite directions. Measured over the spans below, a fade
// filling its clip overshoots the far edge a few per cent of the time, by
// around 1e-14 beats. That is not audible by itself, but it breaks the
// window's own invariants, and those are what `gainAt` is documented to rely
// on for the two ramps being exclusive.
//
// And a `Resolved` is a plain struct with public fields, so nothing stops the
// next caller building one that does not fit the span at all: a length taken
// from the source audio rather than from the arrangement would do it on any
// stretched clip. A fade longer than the clip has to come back as a window
// inside the clip, not as one whose ramp never reaches unity.

void testWindowContainment()
{
    int violations = 0;
    std::string firstViolation;

    const auto holds = [&violations, &firstViolation]
                       (const ClipFade::Window& w, double startBeat, double endBeat, const char* what)
    {
        const bool ok = w.inStart  == startBeat
                     && w.outEnd   == endBeat
                     && w.inEnd    >= startBeat && w.inEnd    <= endBeat
                     && w.outStart >= startBeat && w.outStart <= endBeat
                     && w.inEnd    <= w.outStart
                     && w.inStart  <= w.inEnd
                     && w.outStart <= w.outEnd;

        if (! ok && ++violations == 1)
            firstViolation = std::string (what) + ": span " + std::to_string (startBeat) + " to "
                           + std::to_string (endBeat) + ", window " + std::to_string (w.inStart) + " "
                           + std::to_string (w.inEnd) + " " + std::to_string (w.outStart) + " "
                           + std::to_string (w.outEnd);
        return ok;
    };

    std::mt19937 rng (31337u);
    std::uniform_real_distribution<double> bpmD (20.0, 300.0);
    std::uniform_real_distribution<double> beatsD (0.001, 40.0);
    std::uniform_real_distribution<double> startD (-200.0, 200.0);

    int wouldOvershoot = 0;

    for (int trial = 0; trial < 60000; ++trial)
    {
        const double bpm       = bpmD (rng);
        const double beats     = beatsD (rng);
        const double startBeat = startD (rng);
        const double endBeat   = startBeat + beats;
        const double bps       = bpm / 60.0;

        // Exactly the two conversions the engine does, written the way it
        // writes them: `Project::outputSeconds` and then `bpm / 60`.
        const double clipSeconds = beats * 60.0 / bpm;

        // The automatic fade off and an ask far past the clip, so one fade
        // fills the clip exactly and the conversion back lands on the far
        // edge, give or take the last bit of two multiplications.
        for (int which = 0; which < 2; ++which)
        {
            const ClipFade::Request ask = which == 0 ? ClipFade::Request { 1.0e9, 0.0 }
                                                     : ClipFade::Request { 0.0, 1.0e9 };
            const auto res = ClipFade::resolve (clipSeconds, ask, 0.0);
            const auto w   = ClipFade::windowFor (startBeat, endBeat, res, bps);

            holds (w, startBeat, endBeat, "a fade filling its clip");

            if (startBeat + res.in * bps > endBeat || endBeat - res.out * bps < startBeat)
                ++wouldOvershoot;

            // The far edge still reads exactly unity, which is what an end
            // with no fade on it has to do. Unity, not nearly unity: this is
            // the last sample of a clip that was never asked to fade there.
            if (which == 0)
                check (ClipFade::gainAt (w, endBeat) == 1.0,
                       "a clip whose fade in fills it still ends at exactly unity");
            else
                check (ClipFade::gainAt (w, startBeat) == 1.0,
                       "a clip whose fade out fills it still starts at exactly unity");
        }
    }

    std::printf ("  window clamps: %d of 120000 spans would have read outside the clip unclamped\n",
                 wouldOvershoot);
    check (wouldOvershoot > 100, "the sweep reached the rounding the clamps are there for");

    // A `Resolved` built by hand rather than by `resolve`, including pairs
    // that do not fit the span at all and pairs far larger than it.
    const double lengths[] = { 0.0, 1.0e-9, 0.004, 0.5, 1.0, 2.0, 50.0, 1.0e6 };

    for (double a : lengths)
        for (double b : lengths)
            for (double bps : { 0.25, 2.0, 5.0 })
            {
                const double startBeat = -7.25, endBeat = startBeat + 4.0;

                ClipFade::Resolved odd;
                odd.in  = a;
                odd.out = b;
                odd.inShape  = ClipFade::Shape::equalPower;
                odd.outShape = ClipFade::Shape::sCurve;

                const auto w = ClipFade::windowFor (startBeat, endBeat, odd, bps);
                holds (w, startBeat, endBeat, "a resolved pair that does not fit");

                for (int i = 0; i <= 64; ++i)
                {
                    const double g = ClipFade::gainAt (w, startBeat + 4.0 * (double) i / 64.0);
                    check (g >= 0.0 && g <= 1.0 && std::isfinite (g),
                           "a resolved pair that does not fit still gives a bounded gain");
                }
            }

    check (violations == 0, "every window stays inside its clip with both ramps forwards"
                            + (violations == 0 ? std::string() : ", first: " + firstViolation));
    std::printf ("  window containment: %d violation(s)\n", violations);
}

// ---------------------------------------------------------------------------
// 6c. The abutting join: the automatic fade must be declined where another
//     clip already meets this one.
//
// This is a regression test for a real fault that was nearly shipped. Clips
// here do not overlap unless somebody overlaps them, so two abutting clips'
// automatic fades are consecutive rather than simultaneous: the first ramps
// down to nothing and the second ramps up from nothing, and the composite gain
// passes through exactly zero at a join where the waveform was continuous.
// `Project::split` and `Project::duplicate` both produce exactly that join, so
// before `Request::autoIn` and `autoOut` existed, slicing a clip put a hole in
// it. An audit measured up to 0.99 of full scale of error at such a join.
//
// The check is on the composite gain of the two clips, which is what the mixer
// sums, and it is asserted both ways round: declined, the join is bit for bit
// what it was; granted, it is not. A test that only asserted the first could
// be passed by a studio with no automatic fade at all.

void testAbuttingJoin()
{
    const double bps  = 128.0 / 60.0;
    const double autoLen = ClipFade::defaultAutoSeconds;

    // Two halves of a four second clip cut in the middle, which is what the
    // slice tool produces: the left runs 0 to 2 seconds and the right 2 to 4,
    // each with no drawn fade, and the join is at 2 seconds.
    const double halfSeconds = 2.0;
    const double joinBeat    = halfSeconds * bps;
    const double endBeat     = 2.0 * joinBeat;

    for (bool declined : { true, false })
    {
        ClipFade::Request leftReq, rightReq;
        leftReq.autoOut = ! declined;    // the left half's inner edge
        rightReq.autoIn = ! declined;    // the right half's inner edge

        const auto left  = ClipFade::windowFor (0.0, joinBeat,
                                                ClipFade::resolve (halfSeconds, leftReq, autoLen), bps);
        const auto right = ClipFade::windowFor (joinBeat, endBeat,
                                                ClipFade::resolve (halfSeconds, rightReq, autoLen), bps);

        // Only one of the two clips is sounding at any point, so the composite
        // gain is whichever window covers the beat. That is exactly what
        // `renderAudioClips` computes, because a clip contributes nothing
        // outside its own span.
        const double reach = autoLen * bps * 2.0;
        double worst = 0.0;
        for (int i = 0; i <= 4000; ++i)
        {
            const double beat = joinBeat - reach + 2.0 * reach * (double) i / 4000.0;
            const double g = beat < joinBeat ? ClipFade::gainAt (left, beat)
                                             : ClipFade::gainAt (right, beat);
            worst = std::max (worst, std::abs (g - 1.0));
        }

        if (declined)
        {
            check (worst == 0.0,
                   "a join another clip already makes is left exactly as it was");
            std::printf ("  join declined: worst gain error %.3e\n", worst);
        }
        else
        {
            // The fault, measured, so that the flags are demonstrably doing
            // work rather than hopefully doing it. The gain reaches zero at
            // the join from both sides.
            check (worst > 0.9, "granting the automatic fade at a join really does make a hole");
            near (ClipFade::gainAt (left, std::nextafter (joinBeat, 0.0)), 0.0, 1.0e-6,
                  "the outgoing half reaches silence at the join");
            near (ClipFade::gainAt (right, joinBeat), 0.0, 0.0,
                  "the incoming half starts from silence at the join");
            std::printf ("  join granted:  worst gain error %.3f (this is the fault)\n", worst);
        }
    }

    // The outer edges are untouched by the flags, so a sliced clip still gets
    // de-clicked where it meets silence. Declining at the join must not
    // decline everywhere.
    ClipFade::Request r;
    r.autoOut = false;
    const auto got = ClipFade::resolve (2.0, r, autoLen);
    near (got.in,  autoLen, 0.0, "declining one edge leaves the other with its automatic fade");
    near (got.out, 0.0,     0.0, "a declined edge takes no automatic fade");
    check (got.inShape == ClipFade::declickShape, "the surviving automatic fade keeps the de-click shape");

    // And a drawn fade is never declined: somebody who drags a handle at a
    // join means it, and the studio does not second guess them.
    ClipFade::Request drawn;
    drawn.out      = 0.5;
    drawn.outShape = ClipFade::Shape::linear;
    drawn.autoOut  = false;
    const auto kept = ClipFade::resolve (2.0, drawn, autoLen);
    near (kept.out, 0.5, 0.0, "a drawn fade is kept at a joined edge");
    check (kept.outShape == ClipFade::Shape::linear, "a drawn fade keeps its shape at a joined edge");

    // Both edges declined and nothing drawn is a clip with no fades at all,
    // which must be exactly unity rather than nearly.
    ClipFade::Request neither;
    neither.autoIn = neither.autoOut = false;
    const auto none = ClipFade::resolve (2.0, neither, autoLen);
    check (none.in == 0.0 && none.out == 0.0, "a clip joined at both ends carries no automatic fades");
    const auto w = ClipFade::windowFor (0.0, joinBeat, none, bps);
    for (int i = 0; i <= 1000; ++i)
        check (ClipFade::gainAt (w, joinBeat * (double) i / 1000.0) == 1.0,
               "a clip joined at both ends is exactly unity everywhere");
}

// ---------------------------------------------------------------------------
// 7. Trim anchoring: the test that tells a time from a fraction.
//
// This single check is the difference between the right choice and the
// plausible one. Trim a clip's end and the surviving part of its fade out must
// be bit for bit what it was. A fade stored as a fraction of the clip, or
// rescaled on trim, passes everything else in this file and fails here.

void testTrimAnchoring()
{
    const double bps      = 140.0 / 60.0;
    const double autoLen  = 0.004;
    const double fadeOut  = 0.75;           // three quarters of a second
    const double fadeIn   = 0.30;

    for (double lengthSeconds : { 12.0, 6.0, 3.0, 2.0, 1.5 })
    {
        const double startBeat = 17.5;
        const double endBeat   = startBeat + lengthSeconds * bps;

        const auto resolved = ClipFade::resolve (lengthSeconds,
                                                 { fadeIn, fadeOut,
                                                   ClipFade::Shape::equalPower,
                                                   ClipFade::Shape::sCurve },
                                                 autoLen);
        const auto w = ClipFade::windowFor (startBeat, endBeat, resolved, bps);

        // While the pair still fits, the fade is exactly the length it was
        // asked for, wherever the edges happen to be.
        if (fadeIn + fadeOut <= lengthSeconds)
        {
            near ((w.outEnd - w.outStart) / bps, fadeOut, 1.0e-12,
                  "the fade out keeps its length as the clip is trimmed");
            near ((w.inEnd - w.inStart) / bps, fadeIn, 1.0e-12,
                  "the fade in keeps its length as the clip is trimmed");

            // And the gain at a fixed distance back from the end is identical
            // to what it was at the longer length. Measured against the
            // longest clip in the list rather than against a formula.
            const double refEnd = startBeat + 12.0 * bps;
            const auto   refRes = ClipFade::resolve (12.0,
                                                     { fadeIn, fadeOut,
                                                       ClipFade::Shape::equalPower,
                                                       ClipFade::Shape::sCurve },
                                                     autoLen);
            const auto   ref    = ClipFade::windowFor (startBeat, refEnd, refRes, bps);

            // The tolerance is 1e-10, which is the last few bits of two
            // subtractions against different clip ends and nothing else. The
            // failure it is here to catch is not subtle: a fade stored as a
            // fraction of the clip would be 0.375 seconds long on the six
            // second clip against 0.75 on the twelve, so the gains would
            // differ by tenths rather than by 1e-14. The worst difference is
            // printed so that a drift towards the tolerance is visible rather
            // than silent.
            double worst = 0.0;
            for (int i = 0; i <= 500; ++i)
            {
                const double back = fadeOut * bps * (double) i / 500.0;
                const double got  = ClipFade::gainAt (w,   endBeat - back);
                const double was  = ClipFade::gainAt (ref, refEnd  - back);
                worst = std::max (worst, std::abs (got - was));
            }

            std::printf ("  trim to %5.2f s: worst fade out difference %.3e\n", lengthSeconds, worst);
            check (worst <= 1.0e-10,
                   "trimming changed the fade out's shape: a fade length is a time, "
                   "not a fraction of the clip (at " + std::to_string (lengthSeconds) + " s)");
        }
    }

    // The same for the project tempo, which is the other way a fraction would
    // show up: a de-click fade must last the same number of milliseconds at
    // any tempo, which is why the length is in seconds and only the ramp is
    // converted into beats. Ardour makes the same decision by building its
    // fade on `Temporal::AudioTime`.
    for (double bpm : { 40.0, 90.0, 120.0, 174.0, 240.0 })
    {
        const double b = bpm / 60.0;
        const double lengthSeconds = 4.0;
        const auto   r = ClipFade::resolve (lengthSeconds, { 0.25, 0.5 }, autoLen);
        const auto   w = ClipFade::windowFor (0.0, lengthSeconds * b, r, b);

        near ((w.inEnd  - w.inStart)  / b, 0.25, 1.0e-12, "a fade in lasts the same time at any tempo");
        near ((w.outEnd - w.outStart) / b, 0.50, 1.0e-12, "a fade out lasts the same time at any tempo");
    }
}

// ---------------------------------------------------------------------------
// 8. Saved shape ids.

void testShapeIds()
{
    for (int i = 0; i < ClipFade::numShapes; ++i)
        check (ClipFade::idOfShape (ClipFade::shapeFromId (i)) == i, "a shape id round trips");

    // And round tripped by name as well, which is a different check from the
    // one above however much it looks like the same one. Every id check in
    // this function is parameterised by `numShapes`, so it checks the count
    // against itself: lower `numShapes` to two and the loop above stops at
    // one, the fallback checks below start asking about id two and get linear,
    // which is what they want to hear, and every randomised sweep in this file
    // narrows its own distribution to match. Nothing fails. Meanwhile
    // `ProjectIO` writes `idOfShape (sCurve)` as two and reads it back through
    // `shapeFromId`, which no longer recognises it, so every saved S-curve
    // fade loads as linear: a fade that is quietly the wrong shape, which is
    // exactly the failure the header says it chose a named fallback to avoid.
    for (auto s : allShapes)
    {
        check (ClipFade::shapeFromId (ClipFade::idOfShape (s)) == s,
               std::string (shapeName (s)) + ": survives being saved and loaded by id");
        check (ClipFade::idOfShape (s) >= 0 && ClipFade::idOfShape (s) < ClipFade::numShapes,
               std::string (shapeName (s)) + ": has an id inside the range shapeFromId accepts");
    }

    check (ClipFade::numShapes == (int) (sizeof (allShapes) / sizeof (allShapes[0])),
           "numShapes counts exactly the shapes that exist");

    // An id nobody recognises lands on the first entry rather than off the end
    // of the enum, which is the discipline PanOneParams and ModMatrix follow.
    for (int bad : { -1, -99, ClipFade::numShapes, 1000 })
        check (ClipFade::shapeFromId (bad) == ClipFade::Shape::linear,
               "an unknown shape id falls back to linear");

    // And the first entry is the safe one, which is what makes that fallback
    // harmless: it is the only shape with no transcendental in it.
    check (ClipFade::idOfShape (ClipFade::Shape::linear) == 0, "linear is the fallback entry");

    // The de-click shape is deliberately not the default shape. If somebody
    // later makes them the same, the derivation in the header stops being the
    // reason for anything and this says so.
    check (ClipFade::declickShape != ClipFade::defaultShape,
           "the de-click shape and the default shape are different choices");
    check (ClipFade::declickShape == ClipFade::Shape::sCurve, "the de-click shape is the raised cosine");
    check (ClipFade::defaultShape == ClipFade::Shape::equalPower,
           "the default shape is equal power, following Ardour");

    // Ableton's published figure, so that changing it is a deliberate act.
    near (ClipFade::defaultAutoSeconds, 0.004, 0.0, "the automatic fade is four milliseconds");
}

} // namespace

int main()
{
    std::printf ("ClipFade\n");
    testShapes();
    testLaws();
    testCorrelatedCrossfade();
    testUncorrelatedCrossfade();
    testDeclickDecay();
    testResolveProperties();
    testWindowGeometry();
    testAbuttingJoin();
    testWindowContainment();
    testTrimAnchoring();
    testShapeIds();

    if (failures != 0)
    {
        std::printf ("\n%d check(s) failed.\n", failures);
        return 1;
    }

    std::printf ("\nAll checks passed.\n");
    return 0;
}
