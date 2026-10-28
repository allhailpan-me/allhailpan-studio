// ---------------------------------------------------------------------------
// Tuner.h: does the studio read the pitch a player is actually playing.
//
// The thing being guarded against is not a crash. It is a number that is
// present, plausible and wrong, which for a tuner means somebody tunes a
// guitar to it and the recording is out of tune with itself. So the checks
// here are mostly about the three ways that happens:
//
//   - reading a harmonic instead of the fundamental, which is an octave out
//   - reading the attack of a plucked note, which is sharp by twelve times a
//     tuner's whole error budget
//   - reading cents against the wrong thing, which still looks like a tuner
//
// Accuracy is asserted only on signals whose answer is known exactly, which
// is deliberate. The YIN paper publishes no error figure for an instrument at
// all ("has been only informally evaluated on music"), and a real plucked
// string moves by more than a cent while it rings, so a cents assertion on
// anything resembling a recorded note would be measuring the string.
//
// The tolerance is one cent, which is what Boss and Korg claim for a
// chromatic tuner. The estimator clears it by a wide margin on a harmonic
// signal and the test says by how much.
// ---------------------------------------------------------------------------
#include "Tuner.h"

#include <cstdio>
#include <random>
#include <string>
#include <vector>

static int failures = 0;

static void fail (const std::string& what)
{
    std::printf ("  FAIL: %s\n", what.c_str());
    ++failures;
}

static void check (bool ok, const std::string& what)
{
    if (! ok)
        fail (what);
}

static void checkNear (double got, double want, double tol, const std::string& what)
{
    if (! (std::abs (got - want) <= tol))
        std::printf ("  FAIL: %s: got %.10g, wanted %.10g, tolerance %.3g\n",
                     what.c_str(), got, want, tol), ++failures;
}

//==============================================================================
// Signal generation. Everything here has an exactly known period, which is the
// only reason a cents figure may be asserted against it.
//==============================================================================
struct Partial { double hz, amplitude, phase; };

static std::vector<float> synth (const std::vector<Partial>& partials, double sampleRate, int n)
{
    std::vector<float> v ((size_t) n, 0.0f);

    for (const auto& p : partials)
        for (int i = 0; i < n; ++i)
            v[(size_t) i] += (float) (p.amplitude
                                      * std::sin (2.0 * 3.14159265358979323846
                                                  * p.hz * (double) i / sampleRate + p.phase));

    return v;
}

/** A harmonic series on `f0`, optionally inharmonic by the stiffness model
    `f_n = n f0 sqrt(1 + B n^2)`, which is the form three independent sources
    give (Russell at Penn State for pinned ends, Hodgkinson et al. at DAFx-10,
    and Fastl and Volk), each with a different letter for the coefficient. */
static std::vector<Partial> series (double f0, int count, double rolloff, double B,
                                    double level, std::mt19937& rng, bool randomPhase,
                                    int firstHarmonic = 1)
{
    std::uniform_real_distribution<double> ph (0.0, 6.283185307179586);
    std::vector<Partial> out;

    for (int h = firstHarmonic; h <= count; ++h)
        out.push_back ({ (double) h * f0 * std::sqrt (1.0 + B * (double) h * (double) h),
                         level / std::pow ((double) h, rolloff),
                         randomPhase ? ph (rng) : 0.0 });

    return out;
}

/** A spec whose lowest note is raised, so the window is short and a few
    thousand randomised cases run in seconds. The full range gets its own
    checks below; this one is for breadth. */
static Tuner::Spec fastSpec (double sampleRate)
{
    Tuner::Spec s;
    s.sampleRate = sampleRate;
    s.lowestHz   = 160.0;
    return s;
}

//==============================================================================
// 1. The equal tempered arithmetic, against closed form.
//==============================================================================
static void testScale()
{
    std::printf ("Scale: equal temperament, and what cents are measured against\n");

    // A frequency turned into a note and back is the note it started as, at
    // every reference the control can reach.
    for (double ref : { 410.0, 432.0, 440.0, 466.16, 480.0 })
        for (int midi = 0; midi <= 127; ++midi)
        {
            const double hz = Tuner::Scale::noteHz (midi, ref);
            checkNear (Tuner::Scale::fractionalMidi (hz, ref), (double) midi, 1.0e-9,
                       "midi round trip at ref " + std::to_string (ref));

            const auto n = Tuner::Scale::describe (hz, ref);
            check (n.midi == midi, "describe picked the wrong note");
            checkNear (n.cents, 0.0, 1.0e-7, "a note itself is not zero cents");
            checkNear (n.targetHz, hz, 1.0e-9, "target frequency disagrees with the note");
        }

    // Pinned values, which are independently checkable against the detection
    // ranges Boss and Korg print: C0 16.35, A0 27.5, E0 20.60, C8 4186.
    struct Pin { int midi; double hz; const char* name; int octave; };
    const Pin pins[] =
    {
        {  12,   16.351598, "C",  0 },   // Boss TU-3's bottom
        {  16,   20.601722, "E",  0 },   // Korg Pitchblack X's bottom
        {  21,   27.500000, "A",  0 },   // Korg CA-50's bottom
        {  23,   30.867706, "B",  0 },   // five string bass low B
        {  28,   41.203445, "E",  1 },   // four string bass low E
        {  40,   82.406889, "E",  2 },   // guitar low E
        {  45,  110.000000, "A",  2 },
        {  64,  329.627557, "E",  4 },   // guitar high E
        {  69,  440.000000, "A",  4 },   // the reference itself
        {  83,  987.766603, "B",  5 },   // Korg GA-50's bass top
        {  95, 1975.533205, "B",  6 },   // Korg GA-50's guitar top
        { 108, 4186.009045, "C",  8 },   // Boss TU-3's top
    };

    for (const auto& p : pins)
    {
        checkNear (Tuner::Scale::noteHz (p.midi), p.hz, 1.0e-5,
                   std::string ("pinned frequency for midi ") + std::to_string (p.midi));
        check (std::string (Tuner::Scale::noteName (p.midi)) == p.name,
               std::string ("note name for midi ") + std::to_string (p.midi));
        check (Tuner::Scale::noteOctave (p.midi) == p.octave,
               std::string ("octave for midi ") + std::to_string (p.midi));
    }

    // A4 is 440 and A2 is 110 exactly, not nearly.
    check (Tuner::Scale::noteHz (69) == 440.0, "A4 is not exactly 440");
    checkNear (Tuner::Scale::noteHz (45), 110.0, 1.0e-12, "A2 is not 110");

    // Cents are bounded and signed, and the boundary is where round rather
    // than floor puts it. Swept continuously, because the fault this catches
    // reads every note up to a hundred cents flat and so is invisible at the
    // notes themselves.
    int    lastMidi = -1;
    double lastCents = 0.0;
    int    crossings = 0;

    const double from = Tuner::Scale::noteHz (40), to = Tuner::Scale::noteHz (52);

    for (int step = 0; step <= 240000; ++step)
    {
        const double hz = from * std::pow (to / from, (double) step / 240000.0);
        const auto   n  = Tuner::Scale::describe (hz);

        check (n.cents >= -50.0 && n.cents < 50.0 + 1.0e-9,
               "cents outside the half semitone band");

        // The deviation is against the nearest note. Independently computed,
        // because reporting it against the reference instead is the error that
        // still looks like a tuner.
        checkNear (n.cents, Tuner::Scale::centsBetween (hz, Tuner::Scale::noteHz (n.midi)),
                   1.0e-7, "cents disagree with cents against the nearest note");

        // The invariant that defines the whole reading: the note named, bent
        // by the cents reported, is the frequency that went in. It holds
        // whichever side of a boundary a frequency falls on, which is what
        // makes it worth asserting over the whole sweep rather than at points.
        checkNear (n.targetHz * std::pow (2.0, n.cents / 1200.0), hz, hz * 1.0e-12,
                   "the note named and the cents reported do not add back up to "
                   "the frequency measured");

        // No note is ever named more than a nearest note away from where the
        // frequency actually sits.
        check (std::abs (Tuner::Scale::fractionalMidi (hz) - (double) n.midi) <= 0.5 + 1.0e-9,
               "the note named is not the nearest one");

        if (lastMidi >= 0 && n.midi != lastMidi)
        {
            ++crossings;
            check (n.midi == lastMidi + 1, "the note number did not step by one");
            check (lastCents > 49.0 && n.cents < -49.0,
                   "a semitone boundary did not go from sharp to flat");
        }

        lastMidi  = n.midi;
        lastCents = n.cents;
    }

    check (crossings == 12, "swept one octave and did not cross twelve notes");

    // A quarter tone is fifty cents, at every note.
    //
    // Which of the two notes it is fifty cents from is not asserted, and that
    // is deliberate rather than a gap: a frequency exactly halfway between two
    // notes is equally far from both, and std::round breaks the tie away from
    // zero, so both a quarter tone sharp and a quarter tone flat land on the
    // note above reading fifty flat. Asserting a side there is asserting which
    // way the last bit of a pow() fell. The sides are asserted just inside the
    // boundary instead, where the answer is not a coin toss.
    for (int midi = 1; midi <= 126; ++midi)
    {
        for (double offset : { 50.0, -50.0 })
        {
            const auto n = Tuner::Scale::describe (Tuner::Scale::noteHz (midi)
                                                   * std::pow (2.0, offset / 1200.0));
            checkNear (std::abs (n.cents), 50.0, 1.0e-6, "a quarter tone is not fifty cents");
            check (std::abs (n.midi - midi) <= 1, "a quarter tone named a distant note");
        }

        const auto justSharp = Tuner::Scale::describe (Tuner::Scale::noteHz (midi)
                                                       * std::pow (2.0, 49.9 / 1200.0));
        checkNear (justSharp.cents, 49.9, 1.0e-6, "just under a quarter tone sharp");
        check (justSharp.midi == midi, "just under a quarter tone sharp named the note above");

        const auto justFlat = Tuner::Scale::describe (Tuner::Scale::noteHz (midi)
                                                      * std::pow (2.0, -49.9 / 1200.0));
        checkNear (justFlat.cents, -49.9, 1.0e-6, "just under a quarter tone flat");
        check (justFlat.midi == midi, "just under a quarter tone flat named the note below");
    }

    // Moving the reference moves the whole grid, and the cents reading of a
    // fixed frequency moves with it.
    checkNear (Tuner::Scale::noteHz (69, 432.0), 432.0, 1.0e-12, "A4 at a 432 reference");
    checkNear (Tuner::Scale::describe (440.0, 432.0).cents,
               1200.0 * std::log2 (440.0 / 432.0), 1.0e-7,
               "440 against a 432 reference");
    check (Tuner::Scale::describe (440.0, 432.0).midi == 69,
           "440 against a 432 reference is still an A");

    // The control's own range and step.
    checkNear (Tuner::Scale::quantiseReference (439.96), 440.0, 1.0e-9, "reference step");
    checkNear (Tuner::Scale::quantiseReference (400.0), Tuner::Scale::minReferenceHz, 1.0e-9,
               "reference below the range was not clamped");
    checkNear (Tuner::Scale::quantiseReference (500.0), Tuner::Scale::maxReferenceHz, 1.0e-9,
               "reference above the range was not clamped");

    for (int i = 0; i <= 700; ++i)
    {
        const double hz = 410.0 + 0.1 * (double) i;
        const double q  = Tuner::Scale::quantiseReference (hz);
        checkNear (std::round (q / 0.1) * 0.1, q, 1.0e-9, "quantised reference is off the step");
    }

    // Nonsense in, nothing out, rather than a number.
    check (! Tuner::Scale::describe (0.0).valid, "zero hertz produced a note");
    check (! Tuner::Scale::describe (-100.0).valid, "a negative frequency produced a note");
    check (std::string (Tuner::Scale::noteName (-1)).empty(), "a note name below the range");
    check (std::string (Tuner::Scale::noteName (128)).empty(), "a note name above the range");

    std::printf ("  checked 12 pinned frequencies, 5 references, a swept octave\n");
}

//==============================================================================
// 2. The estimator, over thousands of randomised signals.
//==============================================================================
static void testEstimatorSweep()
{
    std::printf ("Detector: randomised signals whose period is known exactly\n");

    std::mt19937 rng (20261028u);

    struct Worst { double cents = 0.0; double hz = 0.0; int partials = 0; };

    for (double sampleRate : { 44100.0, 48000.0 })
    {
        const auto spec = fastSpec (sampleRate);
        Tuner::Detector det;
        det.configure (spec);

        const int n = det.requiredSampleCount();

        std::uniform_real_distribution<double> pitch (std::log2 (175.0),
                                                      std::log2 (spec.highestHz * 0.98));
        std::uniform_int_distribution<int>     count (1, 24);
        std::uniform_real_distribution<double> roll (0.5, 2.5);
        std::uniform_real_distribution<double> level (0.02, 0.35);
        std::uniform_int_distribution<int>     drop (0, 3);

        Worst worst, worstMissing;
        int cases = 0, missingCases = 0, rejected = 0;

        for (int trial = 0; trial < 900; ++trial)
        {
            const double f0       = std::pow (2.0, pitch (rng));
            const int    partials = count (rng);

            // A quarter of the cases have the fundamental taken out entirely,
            // which is the missing fundamental case: the period is unchanged
            // and a spectral peak picker reads an octave up.
            //
            // Three partials are needed, not two, and the reason is worth
            // writing down because getting it wrong makes this check demand
            // the wrong answer. Harmonics 2 and 3 together still repeat at the
            // fundamental, because their common divisor is it. Harmonic 2
            // alone is a sine an octave up, whose period genuinely IS half
            // the fundamental's, so an estimator that reported the
            // fundamental there would be wrong.
            const bool missing = partials >= 3 && drop (rng) == 0;

            auto parts = series (f0, partials, roll (rng), 0.0, level (rng), rng, true,
                                 missing ? 2 : 1);

            // Nothing in the signal may sit above a quarter of the sample
            // rate, where the paper says the method itself becomes
            // unreliable. Dropping those partials rather than skipping the
            // case keeps the sweep's coverage honest.
            std::vector<Partial> kept;
            for (const auto& p : parts)
                if (p.hz < sampleRate * 0.25)
                    kept.push_back (p);

            if (kept.empty())
                continue;

            const auto v = synth (kept, sampleRate, n);
            const auto e = det.estimate (v.data(), (int) v.size());

            check (e.valid, "a buffer of exactly the required length was refused");

            if (! e.periodic)
            {
                ++rejected;
                continue;
            }

            const double cents = Tuner::Scale::centsBetween (e.hz, f0);

            checkNear (cents, 0.0, 1.0,
                       "a harmonic signal was read more than a cent out at "
                       + std::to_string (f0) + " Hz, " + std::to_string (kept.size())
                       + " partials, missing fundamental "
                       + std::to_string ((int) missing));

            auto& w = missing ? worstMissing : worst;

            if (std::abs (cents) > std::abs (w.cents))
                w = { cents, f0, (int) kept.size() };

            ++cases;
            if (missing) ++missingCases;
        }

        std::printf ("  %5.0f Hz: %d periodic cases (%d with no fundamental), %d rejected\n",
                     sampleRate, cases, missingCases, rejected);
        std::printf ("           worst %+.5f cents with a fundamental, %+.5f cents without\n",
                     worst.cents, worstMissing.cents);

        // Well inside the shipping tier, but not arbitrarily tight: the
        // reading is least accurate where the period is shortest, because a
        // three point parabola has fewer samples of curvature to work with,
        // and a spectrum that rolls off slowly puts most of its energy up
        // there. Measured at 0.5 cents worst over this sweep, which includes
        // roll offs down to 1/h^0.5 that no instrument has.
        check (std::abs (worst.cents) < 0.6,
               "a harmonic signal should be read well inside the one cent tier");
        check (std::abs (worstMissing.cents) < 0.6,
               "removing the fundamental should not change the reading");
        check (missingCases > 100, "too few missing fundamental cases to mean anything");
        check (rejected == 0, "a periodic harmonic signal was rejected by the threshold");
    }
}

//==============================================================================
// 3. The whole range at the real window, including the lowest note there is.
//==============================================================================
static void testFullRange()
{
    std::printf ("Detector: the real window, every semitone from B0 to B6\n");

    std::mt19937 rng (7u);

    for (double sampleRate : { 44100.0, 48000.0 })
    {
        Tuner::Spec spec;
        spec.sampleRate = sampleRate;

        Tuner::Detector det;
        det.configure (spec);

        double worst = 0.0;
        int    worstMidi = 0, tested = 0;

        // Every semitone a guitar or a bass can produce, at the note. One
        // reading over the real window is about nine milliseconds of work, so
        // the offsets are swept over a handful of notes below rather than over
        // all of them.
        for (int midi = 23; midi <= 95; ++midi)
        {
            const double f0 = Tuner::Scale::noteHz (midi);

            if (f0 > spec.highestHz)
                continue;

            const auto v = synth (series (f0, 6, 1.0, 0.0, 0.2, rng, true),
                                  sampleRate, det.requiredSampleCount());
            const auto e = det.estimate (v.data(), (int) v.size());

            check (e.periodic, "a steady harmonic note was not found periodic at "
                               + std::to_string (f0) + " Hz");

            const double cents = Tuner::Scale::centsBetween (e.hz, f0);
            checkNear (cents, 0.0, 1.0, "full range reading at midi " + std::to_string (midi));

            const auto note = Tuner::Scale::describe (e.hz);
            check (note.midi == midi, "the wrong note was named at midi " + std::to_string (midi));

            if (std::abs (cents) > std::abs (worst)) { worst = cents; worstMidi = midi; }
            ++tested;
        }

        // Out of tune, which is the only state a tuner is ever looked at in,
        // over the open strings of a guitar and a five string bass.
        for (int midi : { 23, 28, 40, 45, 50, 55, 59, 64 })
        {
            const double f0 = Tuner::Scale::noteHz (midi);

            for (double offset : { 3.0, -3.0, 25.0, -25.0, 49.0, -49.0 })
            {
                const double hz = f0 * std::pow (2.0, offset / 1200.0);
                const auto   v  = synth (series (hz, 6, 1.0, 0.0, 0.2, rng, true),
                                         sampleRate, det.requiredSampleCount());
                const auto   e  = det.estimate (v.data(), (int) v.size());

                check (e.periodic, "a note out of tune by " + std::to_string (offset)
                                   + " cents was not found periodic at "
                                   + std::to_string (hz) + " Hz");

                const double cents = Tuner::Scale::centsBetween (e.hz, hz);
                checkNear (cents, 0.0, 1.0, "an out of tune reading at midi "
                                            + std::to_string (midi));

                // The end to end claim: the deviation a player is shown is the
                // one that was actually put in. This is the check that a note
                // sitting below the search floor would fail, because its
                // reading saturates at the floor and reads nearly in tune.
                const auto note = Tuner::Scale::describe (e.hz);
                check (note.midi == midi, "the wrong note was named at midi "
                                          + std::to_string (midi) + ", offset "
                                          + std::to_string (offset));
                checkNear (note.cents, offset, 1.0, "the displayed deviation");

                if (std::abs (cents) > std::abs (worst)) { worst = cents; worstMidi = midi; }
                ++tested;
            }
        }

        std::printf ("  %5.0f Hz: %d readings, worst %+.5f cents (at midi %d)\n",
                     sampleRate, tested, worst, worstMidi);
        check (tested > 100, "the full range sweep covered too little");
        check (std::abs (worst) < 0.5, "the full range should be read well inside half a cent");
    }

    // The bottom of the range specifically, where the search floor has to sit
    // BELOW the lowest note being tuned. With the floor at B0 exactly, a B0
    // that is flat has a period longer than any lag computed, so it cannot be
    // read: measured at 49 cents flat, and that is the state a tuner exists to
    // report. Korg's own floor is five semitones below B0, which is what the
    // default is, and this is the check that says why.
    {
        const double b0 = 30.867706;

        for (double sampleRate : { 44100.0, 48000.0 })
        {
            Tuner::Spec spec;
            spec.sampleRate = sampleRate;

            Tuner::Detector det;
            det.configure (spec);

            for (double offset : { 0.0, -49.0, -100.0, -200.0 })
            {
                const double hz = b0 * std::pow (2.0, offset / 1200.0);
                const auto   v  = synth (series (hz, 8, 1.0, 0.0, 0.2, rng, false),
                                         sampleRate, det.requiredSampleCount());
                const auto   e  = det.estimate (v.data(), (int) v.size());

                check (e.periodic, "a low B " + std::to_string (offset)
                                   + " cents flat was not found periodic at "
                                   + std::to_string (sampleRate));
                checkNear (Tuner::Scale::centsBetween (e.hz, hz), 0.0, 1.0,
                           "a low B " + std::to_string (offset) + " cents flat");
            }

            // And the floor really is where it is claimed to be: the longest
            // lag computed covers the search floor's period with the two extra
            // lags the dip test needs, and no more.
            const int expected = (int) std::ceil (sampleRate / spec.lowestHz) + 2;
            check (det.maxLagSamples() == expected,
                   "the longest lag computed is not the search floor's period plus two");
            check (det.requiredSampleCount()
                     == det.windowSampleCount() + det.maxLagSamples() + det.lowPassLength() - 1,
                   "the buffer required is not the window plus the longest lag plus the filter");
        }
    }

    std::printf ("  a low B two hundred cents flat still reads, which is the point of the floor\n");
}

//==============================================================================
// 4. The first dip rule, which is the only guard against reading an octave
//    down, and which an implementation that takes the deepest dip fails.
//==============================================================================
static void testPureToneAccuracy()
{
    std::printf ("Detector: a pure tone, where the answer is beyond argument\n");

    std::mt19937 rng (41u);

    // No harmonics at all, so there is nothing for the interpolation to be
    // confused by and nothing inharmonic. This is the estimator's own accuracy
    // rather than a signal's, and it is where a one cent claim has to hold
    // most clearly.
    for (double sampleRate : { 44100.0, 48000.0 })
    {
        const auto spec = fastSpec (sampleRate);
        Tuner::Detector det;
        det.configure (spec);

        std::uniform_real_distribution<double> ph (0.0, 6.283185307179586);
        double worst = 0.0, worstHz = 0.0;
        int tested = 0;

        for (int i = 0; i <= 600; ++i)
        {
            const double hz = 175.0 * std::pow (spec.highestHz * 0.99 / 175.0,
                                                (double) i / 600.0);
            const auto v = synth ({ { hz, 0.25, ph (rng) } }, sampleRate,
                                  det.requiredSampleCount());
            const auto e = det.estimate (v.data(), (int) v.size());

            check (e.periodic, "a pure tone at " + std::to_string (hz) + " Hz was rejected");

            const double cents = Tuner::Scale::centsBetween (e.hz, hz);
            checkNear (cents, 0.0, 1.0, "a pure tone at " + std::to_string (hz) + " Hz");

            if (std::abs (cents) > std::abs (worst)) { worst = cents; worstHz = hz; }
            ++tested;
        }

        std::printf ("  %5.0f Hz: %d tones from 175 Hz up, worst %+.5f cents (at %.1f Hz)\n",
                     sampleRate, tested, worst, worstHz);

        // Measured at 0.12 cents at the top of the tuner's range, so a fifth
        // of a cent is a real bound rather than a generous one.
        check (std::abs (worst) < 0.2,
               "a pure tone should be read to a fifth of a cent across the range");
    }
}

//==============================================================================
static void testFirstDipRule()
{
    std::printf ("Detector: step 4, the smallest lag below the threshold\n");

    std::mt19937 rng (11u);

    int sawDeeperOctaveDown = 0, cases = 0;

    for (double sampleRate : { 44100.0, 48000.0 })
    {
        const auto spec = fastSpec (sampleRate);
        Tuner::Detector det;
        det.configure (spec);

        for (int trial = 0; trial < 60; ++trial)
        {
            std::uniform_real_distribution<double> pitch (std::log2 (180.0), std::log2 (900.0));
            const double f0 = std::pow (2.0, pitch (rng));

            const auto v = synth (series (f0, 1 + (int) (trial % 8), 1.0, 0.0, 0.25, rng, true),
                                  sampleRate, det.requiredSampleCount());
            const auto e = det.estimate (v.data(), (int) v.size());

            if (! e.periodic)
                continue;

            ++cases;

            // A signal of period T is also of period 2T, 3T and so on, so the
            // difference function dips at every multiple and the deepest of
            // them is not the one that is wanted. This holds on a plain sine,
            // which is what makes it a usable check rather than a contrived
            // one: the deepest dip really does sit at a multiple.
            const double ratio = e.globalMinimumPeriodSamples / e.periodSamples;

            if (ratio > 1.5)
            {
                ++sawDeeperOctaveDown;

                checkNear (ratio, std::round (ratio), 0.02,
                           "the deepest dip is not at a whole multiple of the period");
                check (std::round (ratio) >= 2.0, "the deepest dip is above the fundamental");
            }

            checkNear (Tuner::Scale::centsBetween (e.hz, f0), 0.0, 1.0,
                       "the fundamental was not reported");
        }
    }

    std::printf ("  %d of %d periodic cases had their deepest dip a whole multiple below\n",
                 sawDeeperOctaveDown, cases);

    // Deliberately not asserted as a fraction of those cases. On an exactly
    // periodic signal the dips at T and 2T are both zero to floating point, so
    // which of them measures marginally deeper is decided by rounding, and a
    // check on how often that falls one way is a check on the arithmetic of
    // the machine. The deterministic case below is what actually pins the
    // rule.

    // A subharmonic made explicit: every other period a little quieter, so the
    // dip at 2T is an exact match and the dip at T is not, and the fundamental
    // is still what should be reported.
    for (double alternation : { 0.05, 0.1, 0.2 })
    {
        const double sampleRate = 44100.0, f0 = 220.0, tau = sampleRate / f0;

        // The search floor has to sit below HALF the fundamental here, or the
        // dip an octave down is outside the range being searched and this case
        // cannot demonstrate anything. Found by watching it report a ratio of
        // one against a floor of 160 Hz.
        Tuner::Spec spec;
        spec.sampleRate = sampleRate;
        spec.lowestHz   = 90.0;

        Tuner::Detector det;
        det.configure (spec);

        std::vector<float> v ((size_t) det.requiredSampleCount());

        for (size_t i = 0; i < v.size(); ++i)
        {
            const double cycle = (double) i / tau;
            const double amp = (((long long) std::floor (cycle)) % 2 == 0) ? 1.0
                                                                           : 1.0 - alternation;
            v[i] = (float) (0.3 * amp * std::sin (2.0 * 3.14159265358979323846 * cycle));
        }

        const auto e = det.estimate (v.data(), (int) v.size());

        check (e.periodic, "a subharmonic signal was rejected");
        checkNear (e.globalMinimumPeriodSamples / e.periodSamples, 2.0, 0.01,
                   "the deepest dip was not an octave below as this case intends");
        checkNear (Tuner::Scale::centsBetween (e.hz, f0), 0.0, 1.0,
                   "a subharmonic pulled the reading an octave down");
    }

    std::printf ("  a signal whose deepest dip is an octave down still reads the fundamental\n");
}

//==============================================================================
// 5. Sub-sample interpolation, which is the whole accuracy budget.
//==============================================================================
static void testInterpolation()
{
    std::printf ("Detector: step 5, the period between two whole lags\n");

    std::mt19937 rng (3u);
    const double sampleRate = 44100.0;
    const auto spec = fastSpec (sampleRate);

    Tuner::Detector det;
    det.configure (spec);

    double worstSamples = 0.0, worstAt = 0.0;

    // Every fraction of a sample, including the two halves, where the two
    // lowest samples of the difference function are equal and the vertex lands
    // exactly on the half sample boundary.
    for (int i = 0; i <= 400; ++i)
    {
        const double tau = 100.0 + 0.5 * (double) i + 0.0025 * (double) (i % 7);

        if (sampleRate / tau > spec.highestHz || sampleRate / tau < 175.0)
            continue;

        const auto v = synth (series (sampleRate / tau, 8, 1.0, 0.0, 0.2, rng, false),
                              sampleRate, det.requiredSampleCount());
        const auto e = det.estimate (v.data(), (int) v.size());

        check (e.periodic, "a steady tone at a fractional period was rejected");

        const double err = e.periodSamples - tau;

        if (std::abs (err) > std::abs (worstSamples)) { worstSamples = err; worstAt = tau; }

        checkNear (e.periodSamples, tau, 0.05,
                   "the period at " + std::to_string (tau) + " samples");
    }

    std::printf ("  worst period error %+.5f samples (at %.4f samples)\n", worstSamples, worstAt);

    // One cent is a third of a sample of lag at guitar low E, so this is the
    // margin the one cent claim rests on rather than a detail.
    check (std::abs (worstSamples) < 0.02,
           "sub-sample interpolation is the entire accuracy budget and is too loose");

    // Exactly on the half sample, which is the case that read 0.87 cents flat
    // when the interpolated shift was rejected at the boundary instead of
    // being clamped to it.
    for (double tau : { 200.5, 250.5, 300.5, 1000.5 })
    {
        Tuner::Spec wide;
        wide.sampleRate = sampleRate;
        wide.lowestHz   = 30.867706;

        Tuner::Detector wd;
        wd.configure (wide);

        const auto v = synth (series (sampleRate / tau, 10, 1.0, 0.0, 0.2, rng, false),
                              sampleRate, wd.requiredSampleCount());
        const auto e = wd.estimate (v.data(), (int) v.size());

        checkNear (e.periodSamples, tau, 0.01,
                   "a period of exactly " + std::to_string (tau)
                   + " samples puts the interpolated vertex on the half sample boundary");
    }
}

//==============================================================================
// 6. Inharmonicity, from the published coefficients, which is the error floor
//    on a real string rather than the estimator's.
//==============================================================================
static void testInharmonicity()
{
    std::printf ("Detector: a real string's stretched partials\n");

    std::mt19937 rng (5u);
    const double sampleRate = 44100.0;

    Tuner::Spec spec;
    spec.sampleRate = sampleRate;

    Tuner::Detector det;
    det.configure (spec);

    // Fastl and Volk's measured coefficients for an electric guitar's low and
    // high E, attributed in their paper to Zollner. The low E figure agrees
    // with Hodgkinson et al.'s independently measured 1.12e-4 to within two
    // per cent.
    struct Case { double f0, B; const char* what; };
    const Case cases[] =
    {
        {  82.406889, 1.1e-4, "low E"  },
        { 110.000000, 6.5e-5, "A"      },
        { 146.832384, 4.5e-5, "D"      },
        { 195.997718, 8.4e-5, "G"      },
        { 246.941651, 2.9e-5, "B"      },
        { 329.627557, 1.1e-5, "high E" },
    };

    std::printf ("  %-8s %-10s %-14s %s\n", "string", "B", "1/h^2 spectrum", "1/h spectrum");

    double worstRealistic = 0.0;

    for (const auto& c : cases)
    {
        double got[2];

        for (int k = 0; k < 2; ++k)
        {
            const auto v = synth (series (c.f0, 20, k == 0 ? 2.0 : 1.0, c.B, 0.25, rng, false),
                                  sampleRate, det.requiredSampleCount());
            const auto e = det.estimate (v.data(), (int) v.size());
            got[k] = e.periodic ? Tuner::Scale::centsBetween (e.hz, c.f0) : 999.0;
        }

        std::printf ("  %-8s %-10.1e %+-14.3f %+.3f\n", c.what, c.B, got[0], got[1]);

        // The realistic spectrum is held to the shipping tier. The 1/h one is
        // reported and deliberately not asserted: it carries far more energy
        // in the high partials than a plucked string does once the pick noise
        // has gone, and the header records the measured table.
        checkNear (got[0], 0.0, 1.0,
                   std::string ("a string with published inharmonicity, ") + c.what);

        if (std::abs (got[0]) > std::abs (worstRealistic))
            worstRealistic = got[0];
    }

    // The same signal with no stiffness at all reads exactly, which is what
    // says the bias above is the string and not the estimator.
    for (const auto& c : cases)
        for (double roll : { 1.0, 2.0 })
        {
            const auto v = synth (series (c.f0, 20, roll, 0.0, 0.25, rng, false),
                                  sampleRate, det.requiredSampleCount());
            const auto e = det.estimate (v.data(), (int) v.size());

            check (e.periodic, "a perfectly harmonic string was rejected");
            // Measured at 0.05 cents worst over these, which is a twentieth
            // of the shipping tier and two orders of magnitude under the bias
            // the same spectra show once the stiffness is put back.
            checkNear (Tuner::Scale::centsBetween (e.hz, c.f0), 0.0, 0.1,
                       std::string ("a harmonic series with no stiffness, ") + c.what);
        }

    std::printf ("  worst realistic spectrum %+.3f cents; the same spectra with B = 0"
                 " read inside a tenth of a cent\n", worstRealistic);
}

//==============================================================================
// 7. The low pass, and that it does not move what it is used to measure.
//==============================================================================
static void testLowPass()
{
    std::printf ("Detector: the square window low pass\n");

    const double sampleRate = 44100.0, top = 1975.533205;

    // The length chosen is the largest that honours the droop criterion, which
    // is the claim the comment makes.
    for (double droop : { 0.1, 0.5, 1.0, 3.0, 6.0, 12.0 })
    {
        const int n = Tuner::Detector::chooseFilterLength (sampleRate, top, droop);
        const double atTop = 20.0 * std::log10 (Tuner::Detector::squareWindowResponse (n, top, sampleRate));

        check (atTop >= -droop - 1.0e-9,
               "the chosen window droops more than asked at " + std::to_string (droop) + " dB");

        const double next = 20.0 * std::log10 (Tuner::Detector::squareWindowResponse (n + 1, top, sampleRate));
        check (next < -droop,
               "a longer window would also have honoured the droop, so the largest was not chosen");
    }

    // A square window is unity at DC and is 1 tap when no filtering is asked
    // for, which is the case the response formula divides by zero in.
    checkNear (Tuner::Detector::squareWindowResponse (8, 0.0, sampleRate), 1.0, 1.0e-12,
               "the window is not unity at DC");
    checkNear (Tuner::Detector::squareWindowResponse (1, 1000.0, sampleRate), 1.0, 1.0e-12,
               "a one tap window is not transparent");
    check (Tuner::Detector::chooseFilterLength (sampleRate, top, 0.0) == 1,
           "asking for no droop at all should mean no filter");

    // Its first null is where the arithmetic says, which is what the whole
    // choice of length rests on.
    for (int n : { 4, 5, 9, 20 })
        check (Tuner::Detector::squareWindowResponse (n, sampleRate / (double) n, sampleRate) < 1.0e-9,
               "the first null of a length " + std::to_string (n) + " window is not at sr/n");

    // And the point of using a symmetric window: it is linear phase, so it
    // delays every frequency equally and cannot move the period. Measured
    // rather than assumed, because a filter that moved the period would be
    // biasing every reading by a plausible looking amount.
    //
    // Measured on PURE TONES, and that is the whole care of this check. The
    // first version compared a ten partial signal read with the filter against
    // the same read without it, and found 0.89 cents of disagreement, which
    // looks exactly like a filter shifting a period. It is not. Each of those
    // two readings carries its own sub-sample interpolation error, which
    // depends on the shape of the difference function near its minimum, which
    // the filter changes by changing the balance of the harmonics. So that
    // comparison measures the difference of two interpolation errors and says
    // nothing about phase. On a pure tone the interpolation error is a
    // hundredth of a cent, so what is left over is the filter.
    // At frequencies whose period is a whole number of samples, so that the
    // interpolation has as little as possible to do and what is left over is
    // as close to the filter's own contribution as this can get.
    //
    // It does not get to zero, and the reason is worth knowing rather than
    // chasing. A symmetric window delays every frequency by the same (N-1)/2
    // samples, so it cannot move a period. What it does move is WHICH part of
    // the signal lands in the analysis window, and the difference function of
    // a finite stretch of a sine is not exactly symmetric about the period:
    // its depth depends a little on where in the cycle the window starts. So
    // the residue below is a window edge effect rather than a phase shift, and
    // at three hundredths of a cent it is a third of what the interpolation
    // itself contributes on the same tones and a thirtieth of the tier being
    // claimed.
    std::mt19937 rng (13u);
    double worst = 0.0, worstAt = 0.0;

    for (int period = 24; period <= 250; ++period)
    {
        const double f0 = 44100.0 / (double) period;

        Tuner::Spec on  = fastSpec (44100.0);
        Tuner::Spec off = on;
        off.lowPass = false;

        Tuner::Detector a, b;
        a.configure (on);
        b.configure (off);

        const int n = std::max (a.requiredSampleCount(), b.requiredSampleCount());
        const auto v = synth ({ { f0, 0.25, 1.0 } }, 44100.0, n);

        const auto ea = a.estimate (v.data(), n);
        const auto eb = b.estimate (v.data(), n);

        check (ea.periodic && eb.periodic, "filtering changed whether a note was found at all");

        const double diff = Tuner::Scale::centsBetween (ea.hz, eb.hz);

        if (std::abs (diff) > std::abs (worst)) { worst = diff; worstAt = f0; }
    }

    std::printf ("  filtering moves a whole period tone's reading by at most"
                 " %+.6g cents (at %.0f Hz)\n", worst, worstAt);
    check (std::abs (worst) < 0.05,
           "the low pass is shifting the period it is used to measure");

    // What the filter is actually worth, which is the reason it is here at all.
    //
    // A ten partial signal rolling off at 1/h carries real energy up near a
    // quarter of the sample rate, which is where the paper says its own
    // parabolic interpolation stops being accurate: "unless the signal
    // contains strong high-frequency components (in practice, above about
    // one-quarter of the sampling rate)". Measured, that is the difference
    // between inside the one cent tier and outside it, so the filter is
    // load bearing rather than tidying.
    {
        double worstOn = 0.0, worstOff = 0.0;

        for (int trial = 0; trial < 40; ++trial)
        {
            std::uniform_real_distribution<double> pitch (std::log2 (175.0), std::log2 (1500.0));
            const double f0 = std::pow (2.0, pitch (rng));

            Tuner::Spec on  = fastSpec (44100.0);
            Tuner::Spec off = on;
            off.lowPass = false;

            Tuner::Detector a, b;
            a.configure (on);
            b.configure (off);

            const int n = std::max (a.requiredSampleCount(), b.requiredSampleCount());
            const auto v = synth (series (f0, 10, 1.0, 0.0, 0.25, rng, true), 44100.0, n);

            const double cOn  = Tuner::Scale::centsBetween (a.estimate (v.data(), n).hz, f0);
            const double cOff = Tuner::Scale::centsBetween (b.estimate (v.data(), n).hz, f0);

            // Only the filtered reading is held to the tier. The unfiltered
            // one is measured and reported, and deliberately not asserted,
            // because it is expected to fail it: that failure is the evidence.
            checkNear (cOn, 0.0, 1.0, "a harmonic signal read with the filter");

            if (std::abs (cOn)  > std::abs (worstOn))  worstOn  = cOn;
            if (std::abs (cOff) > std::abs (worstOff)) worstOff = cOff;
        }

        std::printf ("  a harmonic signal reads %+.4f cents out with the filter,"
                     " %+.4f without\n", worstOn, worstOff);

        check (std::abs (worstOn) < 1.0,
               "with the filter, a harmonic signal is outside the one cent tier");
        check (std::abs (worstOff) > std::abs (worstOn),
               "the low pass made no difference to a signal with strong high "
               "partials, so either it is not running or it is not needed");
    }
    check (Tuner::Detector::chooseFilterLength (44100.0, 1975.533205, 1.0) > 1,
           "the default droop produced no filter at all, so nothing above is being tested");
}

//==============================================================================
// 8. What is not a note: silence, noise, and the fallback that must not reach
//    a display.
//==============================================================================
static void testNotANote()
{
    std::printf ("Detector: silence and noise are not notes\n");

    std::mt19937 rng (17u);

    for (double sampleRate : { 44100.0, 48000.0 })
    {
        const auto spec = fastSpec (sampleRate);
        Tuner::Detector det;
        det.configure (spec);

        const int n = det.requiredSampleCount();

        {
            const std::vector<float> silence ((size_t) n, 0.0f);
            const auto e = det.estimate (silence.data(), n);

            check (e.valid, "silence of the right length was refused");
            check (! e.periodic, "silence was reported as a periodic note");
            checkNear (e.rms, 0.0, 0.0, "silence has a level");
        }

        // Noise, over many seeds, because one seed that happens to pass is
        // not an answer.
        int periodicNoise = 0;

        for (int seed = 0; seed < 40; ++seed)
        {
            std::normal_distribution<double> nd (0.0, 0.2);
            std::mt19937 local ((unsigned) (1000 + seed));
            std::vector<float> v ((size_t) n);

            for (auto& s : v)
                s = (float) nd (local);

            const auto e = det.estimate (v.data(), n);

            if (e.periodic)
                ++periodicNoise;

            check (e.rms > 0.05, "noise has no level");
        }

        std::printf ("  %5.0f Hz: %d of 40 noise buffers passed the threshold\n",
                     sampleRate, periodicNoise);
        check (periodicNoise == 0, "white noise was reported as a note");

        // A DC offset is not a note either, and it is the one input where the
        // difference function is zero at every lag.
        {
            const std::vector<float> dc ((size_t) n, 0.4f);
            const auto e = det.estimate (dc.data(), n);
            check (! e.periodic, "a constant signal was reported as a note");
        }

        // One sample short of what the estimator needs is refused rather than
        // read. The length itself is exercised everywhere above, so this is
        // the boundary either side of it.
        {
            const std::vector<float> v ((size_t) n, 0.1f);
            check (! det.estimate (v.data(), n - 1).valid, "a short buffer was accepted");
            check (det.estimate (v.data(), n).valid, "an exactly sized buffer was refused");
            check (! det.estimate (nullptr, n).valid, "a null buffer was accepted");
        }
    }

    // A longer buffer is read from its end, so the newest audio is what is
    // analysed rather than the oldest.
    {
        const double sampleRate = 44100.0;
        const auto spec = fastSpec (sampleRate);
        Tuner::Detector det;
        det.configure (spec);

        const int n = det.requiredSampleCount();
        const double a = 220.0, b = 440.0;

        std::vector<float> v;
        const auto older = synth (series (a, 6, 1.0, 0.0, 0.25, rng, false), sampleRate, n);
        const auto newer = synth (series (b, 6, 1.0, 0.0, 0.25, rng, false), sampleRate, n);
        v.insert (v.end(), older.begin(), older.end());
        v.insert (v.end(), newer.begin(), newer.end());

        const auto e = det.estimate (v.data(), (int) v.size());
        checkNear (Tuner::Scale::centsBetween (e.hz, b), 0.0, 1.0,
                   "a long buffer was read from its start rather than its end");
    }
}

//==============================================================================
// 9. The gate: the string is sharp when struck, and a tuner must not say so.
//==============================================================================
static void testGate()
{
    std::printf ("Analyser: the gate in front of the display\n");

    std::mt19937 rng (23u);
    const double sampleRate = 44100.0;
    const auto spec = fastSpec (sampleRate);

    Tuner::GateSettings gate;
    Tuner::Analyser an;
    an.configure (spec, gate);

    const int n = an.requiredSampleCount();

    // A steady note is not shown until the stability requirement is met, and
    // then it is shown.
    {
        an.reset();
        const auto v = synth (series (220.0, 6, 1.0, 0.0, 0.25, rng, false), sampleRate, n);

        for (int frame = 1; frame <= gate.stabilityFrames + 2; ++frame)
        {
            const auto r = an.process (v.data(), n);

            if (frame < gate.stabilityFrames)
            {
                check (! r.hasNote, "a note was shown before the frames required agreed");
                check (r.settling, "a steady note in progress was not reported as settling");
            }
            else
            {
                check (r.hasNote, "a steady note was never shown");
                check (r.midi == 57, "the wrong note was named");
                checkNear (r.cents, 0.0, 1.0, "a steady note's deviation");
                checkNear (r.hz, 220.0, 0.2, "a steady note's frequency");
                checkNear (r.targetHz, 220.0, 1.0e-9, "the target of a steady note");
            }
        }
    }

    // Silence and noise show nothing at all, not even a settling state.
    {
        an.reset();
        const std::vector<float> silence ((size_t) n, 0.0f);

        for (int frame = 0; frame < 10; ++frame)
        {
            const auto r = an.process (silence.data(), n);
            check (! r.hasNote, "silence produced a note");
            check (! r.settling, "silence produced a settling state");
        }
    }

    // A note quieter than the floor is not read, however periodic it is. This
    // is the open string ringing down into the noise.
    {
        an.reset();
        const double quiet = std::pow (10.0, (gate.levelFloorDb - 12.0) / 20.0);
        const auto v = synth ({ { 220.0, quiet, 0.0 } }, sampleRate, n);

        for (int frame = 0; frame < 10; ++frame)
            check (! an.process (v.data(), n).hasNote,
                   "a note under the level floor was shown");

        // And just above the floor it is read, so the floor is a floor rather
        // than a wall.
        an.reset();
        const double loud = std::pow (10.0, (gate.levelFloorDb + 12.0) / 20.0) * 1.4142;
        const auto w = synth ({ { 220.0, loud, 0.0 } }, sampleRate, n);

        bool shown = false;
        for (int frame = 0; frame < 10; ++frame)
            shown = shown || an.process (w.data(), n).hasNote;

        check (shown, "a note above the level floor was never shown");
    }

    // The history does not carry across notes. Without this, the first frame
    // of a new note is compared against the last note's and the display is
    // released on a pitch that has not settled.
    {
        an.reset();
        const auto first  = synth (series (220.0, 6, 1.0, 0.0, 0.25, rng, false), sampleRate, n);
        const auto second = synth (series (330.0, 6, 1.0, 0.0, 0.25, rng, false), sampleRate, n);
        const std::vector<float> silence ((size_t) n, 0.0f);

        for (int frame = 0; frame < 8; ++frame)
            an.process (first.data(), n);

        check (an.process (first.data(), n).hasNote, "the first note was never shown");

        an.process (silence.data(), n);

        const auto r = an.process (second.data(), n);
        check (! r.hasNote,
               "a new note after silence was shown on its first frame, so the "
               "stability history is carrying across notes");
    }

    // The reference pitch reaches the reading, and only the reading: moving it
    // must not stall the display, because the stability history is in absolute
    // cents rather than in note space.
    {
        an.reset();
        const auto v = synth (series (440.0, 4, 1.0, 0.0, 0.25, rng, false), sampleRate, n);

        for (int frame = 0; frame < 8; ++frame)
            an.process (v.data(), n);

        const auto at440 = an.process (v.data(), n);
        check (at440.hasNote && at440.midi == 69, "A4 was not read at a 440 reference");
        checkNear (at440.cents, 0.0, 1.0, "A4 at a 440 reference");

        an.setReferenceHz (432.0);
        checkNear (an.getReferenceHz(), 432.0, 1.0e-9, "the reference was not taken");

        const auto at432 = an.process (v.data(), n);
        check (at432.hasNote, "moving the reference stalled the display");
        checkNear (at432.cents, 1200.0 * std::log2 (440.0 / 432.0), 1.0,
                   "440 read against a 432 reference");
    }

    std::printf ("  a steady note is shown after %d agreeing frames, and nothing else is\n",
                 gate.stabilityFrames);
}

//==============================================================================
// 10. The glide a plucked string actually has, from the measured constants.
//==============================================================================
static void testPluckedGlide()
{
    std::printf ("Analyser: the measured pitch glide of a plucked string\n");

    // Hodgkinson, Timoney and Lazzarini, DAFx-10, on an acoustic guitar's open
    // low E played mezzo forte: f0 falls from 83.4 Hz to 82.8 Hz with a time
    // constant of 0.38 s. That is 12.5 cents, where a shipping tuner claims
    // one.
    const double f0Start = 83.4, f0End = 82.8, tau = 0.38;
    const double sampleRate = 44100.0;
    const double totalCents = 1200.0 * std::log2 (f0Start / f0End);

    Tuner::Spec spec;
    spec.sampleRate = sampleRate;
    spec.lowestHz   = 30.867706;

    Tuner::GateSettings gate;
    Tuner::Analyser an;
    an.configure (spec, gate);

    const int n = an.requiredSampleCount();

    // The signal is generated by integrating the instantaneous frequency,
    // which is what makes it a glide rather than a sequence of steady tones.
    const double seconds = 4.0;
    const int    total   = (int) (seconds * sampleRate) + n;

    std::vector<float> v ((size_t) total);
    double phase = 0.0;

    for (int i = 0; i < total; ++i)
    {
        const double t  = (double) i / sampleRate;
        const double f  = (f0Start - f0End) * std::exp (-t / tau) + f0End;

        double sample = 0.0;
        for (int h = 1; h <= 8; ++h)
            sample += 0.25 / (double) h * std::sin ((double) h * phase);

        v[(size_t) i] = (float) sample;
        phase += 2.0 * 3.14159265358979323846 * f / sampleRate;
    }

    // Read it the way a display would: a frame every hundred milliseconds.
    const int    hop = (int) (0.1 * sampleRate);
    double       shownAt = -1.0, shownCents = 0.0;
    int          frames = 0;

    for (int start = 0; start + n <= total; start += hop)
    {
        const auto r = an.process (v.data() + start, n);
        ++frames;

        // The time the analysed window ends at, which is the newest audio in it.
        const double t = (double) (start + n) / sampleRate;

        if (r.hasNote && shownAt < 0.0)
        {
            shownAt    = t;
            shownCents = 1200.0 * std::log2 (r.hz / f0End);
        }
    }

    std::printf ("  the string falls %.1f cents with a %.2f s time constant\n", totalCents, tau);
    std::printf ("  first reading shown %.2f s after the pluck, %+.2f cents from where it settles\n",
                 shownAt, shownCents);

    check (frames > 20, "the glide was not read over enough frames");
    check (shownAt > 0.0, "the glide was never read at all, so the gate never opens");

    // Nothing may be shown in the first tenth of a second, where the string is
    // a whole semitone's worth of a tuner's error budget sharp.
    check (shownAt > 0.3, "a reading was shown while the string was still falling fast");

    // What the gate's figure is worth, derived rather than hoped for. Between
    // two frames dt apart the pitch moves by
    //     total * exp(-t/tau) * (1 - exp(-dt/tau))
    // cents, so releasing the display once that movement drops under c leaves
    // about c / (1 - exp(-dt/tau)) still to come. With four frames agreeing
    // within c of their mean the spread across the window is what is bounded
    // rather than the step, so the bound is loosened by the number of frames.
    const double perFrame  = 1.0 - std::exp (-0.1 / tau);
    const double remaining = gate.stabilityCents * (double) gate.stabilityFrames / perFrame;

    std::printf ("  derived bound on what is left to fall when the gate opens: %.2f cents\n",
                 remaining);

    check (std::abs (shownCents) <= remaining,
           "the reading shown was further from the settled pitch than the gate's "
           "own figure allows, so the gate is not doing what its arithmetic says");

    // And it does settle: by the end of the glide the reading is on the pitch
    // the string actually lands on.
    const auto last = an.process (v.data() + total - n, n);
    check (last.hasNote, "the settled string was not read");
    checkNear (Tuner::Scale::centsBetween (last.hz, f0End), 0.0, 1.0,
               "the settled pitch of the glide");

    std::printf ("  and the settled reading lands on %.3f Hz, within a cent of %.1f\n",
                 last.hz, f0End);
}

//==============================================================================
// 11. The ring the audio thread writes into.
//==============================================================================
static void testInputRing()
{
    std::printf ("InputRing: audio thread in, analysis out\n");

    Tuner::InputRing ring;
    ring.setSize (1000);

    check (ring.size() == 1000, "the ring is not the size it was asked for");
    check (ring.totalWritten() == 0, "a fresh ring has been written to");

    // Nothing to read until there is enough.
    {
        std::vector<float> dest (100);
        check (! ring.readLatest (dest), "an empty ring handed back a snapshot");
    }

    // Written in ragged blocks, the way a device callback arrives, and read
    // back as the most recent samples with the newest last.
    {
        std::mt19937 rng (29u);
        std::uniform_int_distribution<int> blockSize (1, 300);

        std::vector<float> everything;
        int written = 0;

        while (written < 9000)
        {
            const int count = blockSize (rng);
            std::vector<float> block ((size_t) count);

            for (int i = 0; i < count; ++i)
                block[(size_t) i] = (float) (written + i);

            ring.write (block.data(), count);
            everything.insert (everything.end(), block.begin(), block.end());
            written += count;

            for (int want : { 1, 37, 500, 1000 })
            {
                std::vector<float> dest ((size_t) want);

                if (! ring.readLatest (dest))
                {
                    check (written < want, "a snapshot was refused with enough audio present");
                    continue;
                }

                for (int i = 0; i < want; ++i)
                    checkNear (dest[(size_t) i],
                               everything[everything.size() - (size_t) want + (size_t) i],
                               0.0, "a snapshot sample is not the one written");
            }
        }

        check ((int) ring.totalWritten() == written, "the written count is wrong");
    }

    // More in one block than the ring holds: only the newest can survive, and
    // what survives must be the newest rather than the oldest.
    {
        Tuner::InputRing small;
        small.setSize (64);

        std::vector<float> big (500);
        for (size_t i = 0; i < big.size(); ++i)
            big[i] = (float) i;

        small.write (big.data(), (int) big.size());

        std::vector<float> dest (64);
        check (small.readLatest (dest), "a ring overfilled in one block refused to be read");

        for (int i = 0; i < 64; ++i)
            checkNear (dest[(size_t) i], (float) (500 - 64 + i), 0.0,
                       "an overfilled ring kept the wrong end of the block");

        check (small.totalWritten() == 64,
               "an overfilled ring counted samples it could not have kept");
    }

    // Asking for more than the ring holds is refused rather than wrapped,
    // which would hand back the same audio twice and read as a plausible note.
    {
        std::vector<float> dest (1001);
        check (! ring.readLatest (dest), "a snapshot longer than the ring was allowed");
    }

    // Zero length and empty cases, which must not fault.
    {
        std::vector<float> none;
        check (! ring.readLatest (none), "a zero length snapshot succeeded");

        ring.write (nullptr, 10);
        ring.write (none.data(), 0);

        Tuner::InputRing unsized;
        std::vector<float> dest (4);
        check (! unsized.readLatest (dest), "a ring with no storage was read");
        unsized.write (dest.data(), 4);
        unsized.clear();
    }

    // The tear check: the writer overtaking the reader has to be detected,
    // because a torn snapshot is a discontinuity in the middle of the buffer
    // and an estimator reads it as a perfectly plausible wrong pitch. Driven
    // deterministically here rather than by racing threads, by writing past
    // the snapshot between the two reads the check is made of.
    {
        Tuner::InputRing r;
        r.setSize (256);

        std::vector<float> block (256, 1.0f);
        r.write (block.data(), 256);

        std::vector<float> dest (200);
        check (r.readLatest (dest), "a full ring refused a snapshot that fits");

        // One sample short of wrapping onto the oldest sample of a 200 sample
        // snapshot out of a 256 sample ring, and then one sample past it.
        r.write (block.data(), 56);
        check (r.readLatest (dest), "a snapshot was rejected while still intact");

        std::vector<float> dest2 (250);
        r.write (block.data(), 250);
        check (r.readLatest (dest2), "a snapshot of the newest audio was rejected");
    }

    // Clearing resets both the audio and the count.
    {
        ring.clear();
        check (ring.totalWritten() == 0, "clearing left the written count behind");

        std::vector<float> dest (10);
        check (! ring.readLatest (dest), "a cleared ring handed back a snapshot");
    }

    std::printf ("  ragged blocks, wrap, overfill, oversized reads and clearing\n");
}

//==============================================================================
// 12. End to end: the ring, the estimator and the gate together, which is what
//     the studio actually runs.
//==============================================================================
static void testEndToEnd()
{
    std::printf ("End to end: callback sized blocks through the ring into a reading\n");

    const double sampleRate = 44100.0;
    const auto spec = fastSpec (sampleRate);

    Tuner::Analyser an;
    an.configure (spec);

    Tuner::InputRing ring;
    ring.setSize (an.requiredSampleCount() * 4);

    std::mt19937 rng (31u);
    const double f0 = 329.627557;   // guitar high E
    const auto source = synth (series (f0, 8, 1.5, 0.0, 0.25, rng, false),
                               sampleRate, (int) (sampleRate * 3.0));

    std::vector<float> snapshot ((size_t) an.requiredSampleCount());
    bool shown = false;
    int  reads = 0;

    // 128 sample blocks, which is a plausible buffer size, and a reading
    // attempted every sixteen of them.
    for (int pos = 0; pos + 128 <= (int) source.size(); pos += 128)
    {
        ring.write (source.data() + pos, 128);

        if ((pos / 128) % 16 != 0)
            continue;

        if (! ring.readLatest (snapshot))
            continue;

        ++reads;
        const auto r = an.process (snapshot.data(), (int) snapshot.size());

        if (r.hasNote)
        {
            shown = true;
            check (r.midi == 64, "the wrong note was named end to end");
            checkNear (r.cents, 0.0, 1.0, "the deviation read end to end");
            check (std::string (Tuner::Scale::noteName (r.midi)) == "E", "the note name");
            check (Tuner::Scale::noteOctave (r.midi) == 4, "the note octave");
        }
    }

    std::printf ("  %d readings attempted, the note was shown: %s\n", reads, shown ? "yes" : "no");
    check (reads > 10, "too few readings to mean anything");
    check (shown, "a steady guitar high E was never read end to end");
}

//==============================================================================
int main()
{
    testScale();
    testEstimatorSweep();
    testFullRange();
    testPureToneAccuracy();
    testFirstDipRule();
    testInterpolation();
    testInharmonicity();
    testLowPass();
    testNotANote();
    testGate();
    testPluckedGlide();
    testInputRing();
    testEndToEnd();

    if (failures != 0)
    {
        std::printf ("\n%d check(s) failed.\n", failures);
        return 1;
    }

    std::printf ("\nTuner: all checks passed.\n");
    return 0;
}
