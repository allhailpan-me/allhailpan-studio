// Checks Synth/Lfo.h.
//
// An LFO is mostly trivial arithmetic, so the checks here are aimed at the
// three parts of it that are not.
//
// That each shape is the shape it is named after, measured against its own
// known harmonic series rather than against a second copy of the formula. A
// square has odd harmonics falling as one over h and nothing even; a triangle
// has odd harmonics falling as one over h squared; a sawtooth has everything
// falling as one over h. Those are facts about the waveforms, so they catch a
// shape that is subtly the wrong one in a way that reading the code does not.
//
// That the antialiasing works, and by how much. The whole reason this is
// written as an oscillator rather than as a slow wiggle is that the matrix
// declares six octaves of rate modulation, which takes it to audio rate. A
// correction that is present and wrong is worse than none, and the only way
// to tell is to measure the stray partials with it and without it.
//
// And that the rate means what it says, for every shape including the two
// random ones, because a rate control that is right for a sine and wrong for
// sample and hold is the kind of thing nobody notices until a patch is
// supposed to be in time with something.

#include "Synth/Lfo.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
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
    std::snprintf (buffer, sizeof (buffer), "%.4f", v);
    return buffer;
}

constexpr double pi = 3.14159265358979323846;
constexpr double rate = 48000.0;

using Shape = Synth::Lfo::Shape;

const char* nameOf (Shape s)
{
    switch (s)
    {
        case Shape::sine:          return "sine";
        case Shape::triangle:      return "triangle";
        case Shape::sawUp:         return "saw up";
        case Shape::sawDown:       return "saw down";
        case Shape::square:        return "square";
        case Shape::pulse:         return "pulse";
        case Shape::sampleAndHold: return "sample and hold";
        case Shape::smoothRandom:  return "smooth random";
    }

    return "?";
}

const Shape everyShape[]
{
    Shape::sine, Shape::triangle, Shape::sawUp, Shape::sawDown,
    Shape::square, Shape::pulse, Shape::sampleAndHold, Shape::smoothRandom
};

std::vector<double> run (Shape shape, double hz, int samples,
                         double width = 0.5, bool fromFresh = true)
{
    Synth::Lfo lfo;
    lfo.setSampleRate (rate);
    lfo.setShape (shape);
    lfo.setPulseWidth (width);
    lfo.setRateHz (hz);
    lfo.reset();

    if (fromFresh)
        lfo.noteOn();

    std::vector<double> out;
    out.reserve ((std::size_t) samples);

    for (int i = 0; i < samples; ++i)
        out.push_back (lfo.nextSample());

    return out;
}

/** The amplitude of one harmonic, by hand, over a whole number of cycles so
    that nothing leaks between them. */
double harmonicOf (const std::vector<double>& v, double hz, int h)
{
    double real = 0.0, imaginary = 0.0;

    for (std::size_t i = 0; i < v.size(); ++i)
    {
        const double angle = 2.0 * pi * (double) h * hz * (double) i / rate;

        real += v[i] * std::cos (angle);
        imaginary -= v[i] * std::sin (angle);
    }

    return 2.0 * std::sqrt (real * real + imaginary * imaginary) / (double) v.size();
}

//==============================================================================
/** Every shape stays inside minus one to one and starts where it was told. */
void theShapesAreBounded()
{
    std::printf ("bounded\n");

    for (auto shape : everyShape)
        for (double hz : { 0.01, 1.0, 100.0, 5000.0 })
        {
            const auto rendered = run (shape, hz, 20000);

            double worst = 0.0;

            for (double v : rendered)
                worst = std::max (worst, std::fabs (v));

            // The corrected edges overshoot a little by construction: the
            // polynomial that replaces a step is a ramp through the middle,
            // and at the ends of it the curve sits slightly outside the
            // square it is correcting.
            if (! (worst <= 1.35))
                fail (std::string (nameOf (shape)) + " at " + show (hz)
                      + " Hz reached " + show (worst));

            for (double v : rendered)
                if (! std::isfinite (v))
                {
                    fail (std::string (nameOf (shape)) + " produced something that is"
                                                         " not a number");
                    break;
                }
        }

    std::printf ("    every shape at every rate stays in range\n");
}

/** Each shape has the harmonic series its name implies.

    Measured at a rate low enough that nothing is aliasing, so what this
    catches is a wrong shape rather than a wrong correction.
*/
void theShapesAreTheirOwnSeries()
{
    std::printf ("the series each shape has\n");

    // Twenty cycles of a rate that divides the sample rate exactly, so a
    // whole number of cycles fits the window and every harmonic lands on a
    // bin of its own.
    const double hz = 48000.0 / 4800.0;      // 10 Hz, 4800 samples a cycle
    const int samples = 4800 * 20;

    struct Expected
    {
        Shape shape;
        double second, third, fourth, fifth;    // against the first
    };

    // A sawtooth has every harmonic at one over h. A square and a triangle
    // have only the odd ones, at one over h and one over h squared.
    const Expected expectations[]
    {
        { Shape::sine,     0.0,     0.0,       0.0,    0.0       },
        { Shape::triangle, 0.0,     1.0 / 9.0, 0.0,    1.0 / 25.0 },
        { Shape::sawUp,    1.0 / 2, 1.0 / 3.0, 1.0 / 4, 1.0 / 5.0 },
        { Shape::sawDown,  1.0 / 2, 1.0 / 3.0, 1.0 / 4, 1.0 / 5.0 },
        { Shape::square,   0.0,     1.0 / 3.0, 0.0,    1.0 / 5.0 }
    };

    for (const auto& expected : expectations)
    {
        const auto rendered = run (expected.shape, hz, samples);

        const double first = harmonicOf (rendered, hz, 1);

        const double got[4]
        {
            harmonicOf (rendered, hz, 2) / first,
            harmonicOf (rendered, hz, 3) / first,
            harmonicOf (rendered, hz, 4) / first,
            harmonicOf (rendered, hz, 5) / first
        };

        const double want[4] { expected.second, expected.third,
                               expected.fourth, expected.fifth };

        std::printf ("    %-8s first %.4f, then", nameOf (expected.shape), first);

        for (int h = 0; h < 4; ++h)
            std::printf (" %.4f", got[h]);

        std::printf ("\n");

        if (! (first > 0.2))
            fail (std::string (nameOf (expected.shape)) + " has almost no fundamental");

        for (int h = 0; h < 4; ++h)
            if (std::fabs (got[h] - want[h]) > 0.02)
                fail (std::string (nameOf (expected.shape)) + " harmonic "
                      + std::to_string (h + 2) + " is at " + show (got[h])
                      + " where its series says " + show (want[h]));
    }

    // A pulse is a square when its width is a half, and is not one otherwise:
    // the even harmonics are what the width control brings in.
    {
        const auto wide = run (Shape::pulse, hz, samples, 0.5);
        const auto narrow = run (Shape::pulse, hz, samples, 0.25);

        const double evenWide = harmonicOf (wide, hz, 2) / harmonicOf (wide, hz, 1);
        const double evenNarrow = harmonicOf (narrow, hz, 2) / harmonicOf (narrow, hz, 1);

        std::printf ("    pulse    second harmonic at half width %.4f,"
                     " at a quarter %.4f\n", evenWide, evenNarrow);

        if (! (evenWide < 0.02))
            fail ("a pulse at half width has even harmonics: " + show (evenWide));

        // A pulse of width d has harmonic h at |sin(pi h d)| over h, so at a
        // quarter the second is sin(pi/2)/2 against the first's sin(pi/4),
        // which is about 0.707.
        if (std::fabs (evenNarrow - 0.7071) > 0.03)
            fail ("a quarter width pulse has its second harmonic at "
                  + show (evenNarrow) + " where the series says 0.7071");
    }
}

/** Where each shape starts, and how the pairs relate to each other.

    The harmonic check above measures magnitudes, which is most of what a
    shape is and not all of it. A triangle running the wrong way round, and
    a falling sawtooth that is really a rising one, both have exactly the
    harmonic amplitudes they should and are both wrong: the first means a
    phase offset lands somewhere else than the panel says, and the second
    means two shapes in the menu are one shape.
*/
void theShapesStartWhereTheyShould()
{
    std::printf ("where each shape starts\n");

    // Slow enough that one sample is a tiny part of a cycle, so the start
    // of the waveform is read rather than the edge correction at it.
    const double hz = 2.0;
    const int samples = (int) (rate / hz);

    // A triangle starts at the bottom and climbs.
    {
        const auto rendered = run (Shape::triangle, hz, samples);

        std::printf ("    triangle starts at %s and the next sample is %s\n",
                     show (rendered[0]).c_str(), show (rendered[1]).c_str());

        if (! (rendered[0] < -0.999))
            fail ("a triangle does not start at the bottom: " + show (rendered[0]));

        if (! (rendered[1] > rendered[0]))
            fail ("a triangle does not climb from its start");

        // And it turns over exactly halfway, which is what makes it a
        // triangle rather than a ramp with a corner somewhere else.
        double highest = -2.0;
        std::size_t at = 0;

        for (std::size_t i = 0; i < rendered.size(); ++i)
            if (rendered[i] > highest)
            {
                highest = rendered[i];
                at = i;
            }

        const double where = (double) at / (double) rendered.size();

        std::printf ("    and peaks %s of the way through\n", show (where).c_str());

        if (std::fabs (where - 0.5) > 0.01)
            fail ("a triangle peaks " + show (where) + " of the way through");
    }

    // The two sawtooths are each other upside down, sample for sample.
    {
        const auto up = run (Shape::sawUp, hz, samples);
        const auto down = run (Shape::sawDown, hz, samples);

        double worst = 0.0;

        for (std::size_t i = 0; i < up.size(); ++i)
            worst = std::max (worst, std::fabs (up[i] + down[i]));

        std::printf ("    the two sawtooths differ from being each other's"
                     " negative by %.2e\n", worst);

        if (worst > 1e-12)
            fail ("the falling sawtooth is not the rising one inverted: off by "
                  + show (worst));

        // Which would also be true if both were the same shape and that
        // shape were zero, so one of them has to actually ramp. Read a
        // quarter and three quarters of the way through rather than at the
        // ends: the first sample sits on the discontinuity, where the
        // correction puts a band limited edge through the middle of the
        // jump rather than at the bottom of the ramp.
        if (! (up[up.size() / 4] < -0.4 && up[up.size() * 3 / 4] > 0.4))
            fail ("the rising sawtooth does not ramp: a quarter in it is at "
                  + show (up[up.size() / 4]) + " and three quarters in at "
                  + show (up[up.size() * 3 / 4]));
    }

    // The smooth random arrives at each new value with no corner, which is
    // the entire difference between it and sample and hold.
    {
        const double slowHz = 20.0;
        const int cycle = (int) (rate / slowHz);

        const auto rendered = run (Shape::smoothRandom, slowHz, cycle * 8);

        double atTheJoin = 0.0, inTheMiddle = 0.0;

        for (int c = 2; c < 7; ++c)
        {
            const std::size_t start = (std::size_t) (c * cycle);

            // The step across the moment a new value is drawn.
            atTheJoin = std::max (atTheJoin,
                                  std::fabs (rendered[start] - rendered[start - 1]));

            const std::size_t middle = start + (std::size_t) (cycle / 2);

            inTheMiddle = std::max (inTheMiddle,
                                    std::fabs (rendered[middle] - rendered[middle - 1]));
        }

        std::printf ("    smooth random steps %.2e at a join and %.2e in the middle\n",
                     atTheJoin, inTheMiddle);

        // A raised cosine is flat at both ends and steepest in the middle.
        // Straight lines between the values would move by the same amount
        // everywhere, which is a corner at every join.
        if (! (inTheMiddle > atTheJoin * 10.0))
            fail ("the smooth random has a corner at its joins: it steps "
                  + show (atTheJoin) + " there against " + show (inTheMiddle)
                  + " in the middle");
    }
}

/** The antialiasing is doing something, and the measurement says how much.

    The claim is not that a corrected square is clean. It is that the first
    order of the aliasing is gone, which at these rates is the difference
    between a modulator that is usable high up and one that is not.
*/
void theEdgesAreAntialiased()
{
    std::printf ("antialiasing\n");

    // Off the bin grid on purpose, so the aliases cannot land on harmonics
    // and hide there.
    const double hz = 2371.3;
    const int samples = 16384;

    const auto strayDb = [&] (const std::vector<double>& v)
    {
        std::vector<double> windowed (v.size());

        const double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;

        for (std::size_t i = 0; i < v.size(); ++i)
        {
            const double t = 2.0 * pi * (double) i / (double) (v.size() - 1);

            windowed[i] = v[i] * (a0 - a1 * std::cos (t) + a2 * std::cos (2.0 * t)
                                     - a3 * std::cos (3.0 * t));
        }

        const int n = (int) v.size();
        const double binHz = rate / (double) n;

        double loudest = 0.0, stray = 0.0;

        for (int k = 4; k <= n / 2; ++k)
        {
            std::complex<double> sum (0.0, 0.0);

            for (int i = 0; i < n; ++i)
                sum += windowed[(std::size_t) i]
                     * std::exp (std::complex<double> (0.0,
                           -2.0 * pi * (double) k * (double) i / (double) n));

            const double atHz = (double) k * binHz;
            const double magnitude = std::abs (sum);
            const double nearest = std::floor (atHz / hz + 0.5);

            if (nearest >= 1.0 && std::fabs (atHz - nearest * hz) / binHz <= 8.0)
            {
                loudest = std::max (loudest, magnitude);
                continue;
            }

            if (atHz > 20000.0)
                continue;

            stray = std::max (stray, magnitude);
        }

        return 20.0 * std::log10 (stray / (loudest + 1e-300) + 1e-300);
    };

    for (auto shape : { Shape::sawUp, Shape::square })
    {
        const auto corrected = run (shape, hz, samples);

        // The same waveform with the correction taken out, written here so
        // that the comparison is against the thing the correction replaced
        // rather than against a number somebody chose.
        std::vector<double> naive;
        naive.reserve ((std::size_t) samples);

        double phase = 0.0;
        const double increment = hz / rate;

        for (int i = 0; i < samples; ++i)
        {
            naive.push_back (shape == Shape::sawUp ? 2.0 * phase - 1.0
                                                   : (phase < 0.5 ? 1.0 : -1.0));

            phase += increment;

            if (phase >= 1.0)
                phase -= 1.0;
        }

        const double before = strayDb (naive);
        const double after = strayDb (corrected);

        std::printf ("    %-8s naive %s dB, corrected %s dB, better by %s\n",
                     nameOf (shape), show (before).c_str(), show (after).c_str(),
                     show (before - after).c_str());

        if (! (after < before - 8.0))
            fail (std::string (nameOf (shape)) + " is no cleaner with the correction"
                  " than without it: " + show (before) + " against " + show (after));
    }
}

/** The rate means the same thing for every shape, including the two that
    are not waveforms at all. */
void theRateIsTheRate()
{
    std::printf ("the rate\n");

    for (auto shape : everyShape)
    {
        const double hz = 5.0;
        const int samples = (int) (rate * 4.0);

        const auto rendered = run (shape, hz, samples);

        // Counted as upward crossings of the middle, with the signal first
        // smoothed enough that a held random value stepping down and up
        // again is not read as two cycles.
        int crossings = 0;
        bool above = false;

        double mean = 0.0;

        for (double v : rendered)
            mean += v;

        mean /= (double) rendered.size();

        for (std::size_t i = 1; i < rendered.size(); ++i)
        {
            const bool nowAbove = rendered[i] > mean + 0.1;

            if (nowAbove && ! above)
                ++crossings;

            if (rendered[i] < mean - 0.1)
                above = false;
            else if (nowAbove)
                above = true;
        }

        const double measured = (double) crossings / 4.0;

        std::printf ("    %-16s set to %.0f Hz, crosses %.2f times a second\n",
                     nameOf (shape), hz, measured);

        // The random shapes draw a new value each cycle and a new value is
        // not guaranteed to be on the other side of the middle, so they
        // cross less often than they cycle. What is being checked is that
        // the rate is in the right territory for every shape, not that a
        // random sequence is periodic.
        const double lowest = (shape == Shape::sampleAndHold
                                 || shape == Shape::smoothRandom) ? 1.0 : 4.5;

        if (! (measured >= lowest && measured <= 5.5))
            fail (std::string (nameOf (shape)) + " set to 5 Hz measures "
                  + show (measured));
    }
}

/** A fade means the LFO arrives rather than being there from the first
    sample, and it reaches full depth. */
void theFadeArrives()
{
    std::printf ("the fade\n");

    Synth::Lfo lfo;
    lfo.setSampleRate (rate);
    lfo.setShape (Shape::sine);
    lfo.setRateHz (8.0);
    lfo.setFadeSeconds (1.0);
    lfo.reset();
    lfo.noteOn();

    double early = 0.0, late = 0.0;

    for (int i = 0; i < (int) (rate * 3.0); ++i)
    {
        const double v = std::fabs (lfo.nextSample());

        if (i < (int) (rate * 0.25))
            early = std::max (early, v);

        if (i > (int) (rate * 2.0))
            late = std::max (late, v);
    }

    std::printf ("    largest in the first quarter second %s, after two %s\n",
                 show (early).c_str(), show (late).c_str());

    // A quarter of the way through a one second fade, a sine that has been
    // round twice reaches about a quarter of its depth.
    if (! (early < 0.4))
        fail ("the fade did not hold the LFO back: " + show (early));

    if (! (late > 0.98))
        fail ("the fade never reached full depth: " + show (late));

    // And no fade means no holding back at all.
    Synth::Lfo immediate;
    immediate.setSampleRate (rate);
    immediate.setShape (Shape::sine);
    immediate.setRateHz (8.0);
    immediate.setPhaseOffset (0.25);        // starting at its peak
    immediate.setFadeSeconds (0.0);
    immediate.reset();
    immediate.noteOn();

    if (! (std::fabs (immediate.nextSample()) > 0.99))
        fail ("with no fade the LFO still did not start at full depth");
}

/** Retriggering restarts it from its phase offset, and not retriggering
    leaves it where it was. */
void retriggerDoesWhatItSays()
{
    std::printf ("retrigger\n");

    for (bool shouldRetrigger : { true, false })
    {
        Synth::Lfo lfo;
        lfo.setSampleRate (rate);
        lfo.setShape (Shape::sawUp);
        lfo.setRateHz (3.0);
        lfo.setPhaseOffset (0.0);
        lfo.setRetrigger (shouldRetrigger);
        lfo.reset();
        lfo.noteOn();

        // A third of the way round, so it is a long way from where it
        // started and nowhere near wrapping.
        for (int i = 0; i < (int) (rate / 9.0); ++i)
            lfo.nextSample();

        const double before = lfo.getOutput();

        lfo.noteOn();

        // Two samples, not one. The first sample after a retrigger lands
        // exactly on the ramp's discontinuity, and the correction puts a
        // band limited edge through the middle of a jump rather than at
        // either end of it, so it reads zero and is right to: that is what
        // the antialiasing is for. The sample after it is the bottom of the
        // ramp. Reading only the first one measured 0.0 and looked like a
        // retrigger that had not happened.
        lfo.nextSample();

        const double after = lfo.nextSample();

        std::printf ("    %-15s at %s, two samples after a note on %s\n",
                     shouldRetrigger ? "retriggering" : "free running",
                     show (before).c_str(), show (after).c_str());

        if (shouldRetrigger)
        {
            // Back to the start of the ramp, which is minus one.
            if (! (after < -0.95))
                fail ("retriggering did not restart the LFO: it is at " + show (after));
        }
        else
        {
            if (! (std::fabs (after - before) < 0.01))
                fail ("a free running LFO restarted anyway: " + show (before)
                      + " to " + show (after));
        }
    }

    // And the phase offset is where a retrigger lands.
    Synth::Lfo offset;
    offset.setSampleRate (rate);
    offset.setShape (Shape::sawUp);
    offset.setRateHz (3.0);
    offset.setPhaseOffset (0.75);
    offset.reset();
    offset.noteOn();

    const double landed = offset.nextSample();

    std::printf ("    a quarter from the end of the ramp starts at %s\n",
                 show (landed).c_str());

    if (std::fabs (landed - 0.5) > 0.02)
        fail ("a phase offset of three quarters started the ramp at " + show (landed)
              + " rather than at a half");
}

/** Two LFOs seeded differently do not draw the same random numbers, which is
    what stops every voice of a chord moving together. */
void theRandomSequencesDiffer()
{
    std::printf ("random\n");

    Synth::Lfo a, b;

    for (auto* lfo : { &a, &b })
    {
        lfo->setSampleRate (rate);
        lfo->setShape (Shape::sampleAndHold);
        lfo->setRateHz (40.0);
    }

    a.setSeed (1);
    b.setSeed (2);
    a.reset();
    b.reset();

    int same = 0, counted = 0;

    for (int i = 0; i < 40000; ++i)
    {
        const double left = a.nextSample();
        const double right = b.nextSample();

        if (i % 100 == 0)
        {
            ++counted;

            if (std::fabs (left - right) < 1e-9)
                ++same;
        }
    }

    std::printf ("    two seeds agreed on %d of %d readings\n", same, counted);

    if (same > counted / 20)
        fail ("two differently seeded LFOs draw the same numbers");

    // And one of them covers its whole range rather than sitting somewhere.
    double lowest = 10.0, highest = -10.0;

    for (int i = 0; i < 40000; ++i)
    {
        const double v = a.nextSample();
        lowest = std::min (lowest, v);
        highest = std::max (highest, v);
    }

    if (! (lowest < -0.9 && highest > 0.9))
        fail ("the random shape only covers " + show (lowest) + " to " + show (highest));
}

void nothingUnreasonableBreaksIt()
{
    std::printf ("out of range use\n");

    std::mt19937 rng (8812);
    std::uniform_real_distribution<double> wild (-1e6, 1e6);
    std::uniform_int_distribution<int> whichShape (0, 7);

    for (double sampleRate : { 0.0, -1.0, 8000.0, 192000.0 })
    {
        Synth::Lfo lfo;
        lfo.setSampleRate (sampleRate);

        for (int i = 0; i < 60000; ++i)
        {
            if (i % 250 == 0)
            {
                lfo.setRateHz (wild (rng));
                lfo.setPulseWidth (wild (rng));
                lfo.setPhaseOffset (wild (rng));
                lfo.setFadeSeconds (wild (rng));
                lfo.setShape ((Shape) whichShape (rng));

                if (i % 1000 == 0)
                    lfo.noteOn();
            }

            const double v = lfo.nextSample();

            if (! std::isfinite (v))
            {
                fail ("the output stopped being finite");
                return;
            }

            if (std::fabs (v) > 2.0)
            {
                fail ("the output reached " + show (v));
                return;
            }
        }
    }

    std::printf ("    survives nonsense at every sample rate\n");
}

} // namespace

int main()
{
    std::printf ("Synth::Lfo\n\n");

    theShapesAreBounded();
    theShapesAreTheirOwnSeries();
    theShapesStartWhereTheyShould();
    theEdgesAreAntialiased();
    theRateIsTheRate();
    theFadeArrives();
    retriggerDoesWhatItSays();
    theRandomSequencesDiffer();
    nothingUnreasonableBreaksIt();

    std::printf ("\n");

    if (failures > 0)
    {
        std::printf ("%d check(s) failed.\n", failures);
        return 1;
    }

    std::printf ("All checks passed.\n");
    return 0;
}
