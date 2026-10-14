// Checks Synth/WaveBank.h: the wavetables the instrument ships with.
//
// The oscillator that plays these is checked by WaveTableTest, which proves
// that whatever it is handed comes out without aliasing. This file is about
// what it is handed, and the failures here are a different shape entirely.
// None of them crash and none of them are loud. A table whose position
// control does nothing for its first half, a table that fades to nothing as
// it is swept, a shape whose spectrum is not the spectrum of that shape: all
// of those build, play, and are simply not the instrument anybody meant.
//
// Three things get asserted rather than assumed.
//
// That the general method works. Most of these tables are written as a
// waveform and turned into harmonics by transform, so the scaling and the
// phase convention of that step decide whether every shaped table is right.
// It is checked against the three waveforms whose series is exactly known,
// and against the Jacobi-Anger expansion for the phase modulation table,
// which has a closed form in Bessel functions.
//
// That the analysis is clean. Sampling a shape at sixteen thousand points
// folds everything above the eight thousandth harmonic back down onto the
// ones that are kept. The header claims that lands eighty decibels below
// anything retained, so that is measured.
//
// And that sweeping a table is a change of timbre rather than a change of
// level. This is the one that is easy to get wrong and impossible to see in
// the code: it comes out of the interaction between how a recipe is written
// and how the table is normalised, and the only way to know is to build them
// and look at the numbers.

#include "Synth/WaveBank.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <set>
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
    char buffer[32];
    std::snprintf (buffer, sizeof (buffer), "%.3f", v);
    return buffer;
}

constexpr double pi = 3.14159265358979323846;

/** The waveform a spectrum describes, at one phase, computed directly. */
double waveformAt (const Synth::Spectrum& s, double phase, int limit = 0)
{
    const int highest = limit > 0 ? std::min (limit, (int) s.amplitude.size())
                                  : (int) s.amplitude.size();

    double sum = 0.0;

    for (int h = 1; h <= highest; ++h)
    {
        const double amplitude = (double) s.amplitude[(std::size_t) (h - 1)];
        const double offset = (std::size_t) (h - 1) < s.phase.size()
                            ? (double) s.phase[(std::size_t) (h - 1)] : 0.0;

        sum += amplitude * std::cos (2.0 * pi * (double) h * phase + offset);
    }

    return sum;
}

double peakOf (const Synth::Spectrum& s)
{
    double peak = 0.0;

    for (int i = 0; i < 4096; ++i)
        peak = std::max (peak, std::fabs (waveformAt (s, (double) i / 4096.0)));

    return peak;
}

double rmsOf (const Synth::Spectrum& s)
{
    // Parseval: the mean square of a sum of harmonics is half the sum of the
    // squares of their amplitudes, whatever their phases. No need to render.
    double sum = 0.0;

    for (float a : s.amplitude)
        sum += (double) a * (double) a;

    return std::sqrt (sum * 0.5);
}

//==============================================================================
/** The catalogue is a contract with every project ever saved. */
void theCatalogueIsWellFormed()
{
    std::printf ("catalogue\n");

    std::set<std::string> ids, names;

    for (int i = 0; i < Synth::WaveBank::count(); ++i)
    {
        const auto& entry = Synth::WaveBank::catalogue()[(std::size_t) i];

        if (entry.id == nullptr || entry.id[0] == '\0')
            fail ("entry " + std::to_string (i) + " has no id");

        if (entry.name == nullptr || entry.name[0] == '\0')
            fail ("entry " + std::to_string (i) + " has no name");

        if (entry.category == nullptr || entry.category[0] == '\0')
            fail (std::string (entry.id) + " has no category");

        if (entry.frames < 2)
            fail (std::string (entry.id) + " has fewer than two frames, so its position "
                  "control would do nothing");

        if (! ids.insert (entry.id).second)
            fail ("two entries share the id " + std::string (entry.id));

        if (! names.insert (entry.name).second)
            fail ("two entries share the name " + std::string (entry.name));

        // The round trip a project depends on.
        if (Synth::WaveBank::indexFromId (entry.id) != i)
            fail (std::string (entry.id) + " does not resolve back to its own index");

        if (std::strcmp (Synth::WaveBank::idAt (i), entry.id) != 0)
            fail ("idAt disagrees with the catalogue at " + std::to_string (i));
    }

    // An id from a later version, or a corrupted one, must not resolve to a
    // real table. Silently playing the wrong waveform is worse than saying
    // the patch wants something this build does not have.
    for (const char* unknown : { "", "nope", "Analogue", "analogue2" })
        if (Synth::WaveBank::indexFromId (unknown) != -1)
            fail (std::string ("an unknown id resolved to a table: ") + unknown);

    if (Synth::WaveBank::indexFromId (nullptr) != -1)
        fail ("a null id resolved to a table");

    for (int bad : { -1, Synth::WaveBank::count(), 9999 })
        if (Synth::WaveBank::idAt (bad)[0] != '\0')
            fail ("idAt returned something for index " + std::to_string (bad));
}

/** Every recipe produces the frames it promised, all of them finite. */
void everyRecipeProducesItsFrames()
{
    std::printf ("every recipe builds\n");

    for (int i = 0; i < Synth::WaveBank::count(); ++i)
    {
        const auto& entry = Synth::WaveBank::catalogue()[(std::size_t) i];
        const auto frames = Synth::WaveBank::framesFor (i);

        if ((int) frames.size() != entry.frames)
        {
            fail (std::string (entry.id) + " produced " + std::to_string (frames.size())
                  + " frames rather than " + std::to_string (entry.frames));
            continue;
        }

        for (std::size_t f = 0; f < frames.size(); ++f)
        {
            if (frames[f].amplitude.empty())
            {
                fail (std::string (entry.id) + " frame " + std::to_string (f) + " is empty");
                continue;
            }

            for (std::size_t h = 0; h < frames[f].amplitude.size(); ++h)
                if (! std::isfinite (frames[f].amplitude[h]))
                {
                    fail (std::string (entry.id) + " frame " + std::to_string (f)
                          + " harmonic " + std::to_string (h + 1) + " is not finite");
                    break;
                }

            for (float p : frames[f].phase)
                if (! std::isfinite (p))
                {
                    fail (std::string (entry.id) + " frame " + std::to_string (f)
                          + " has a phase that is not finite");
                    break;
                }

            if (! (rmsOf (frames[f]) > 1e-6))
                fail (std::string (entry.id) + " frame " + std::to_string (f) + " is silent");
        }
    }

    // Out of range indices give nothing rather than reading off the end.
    for (int bad : { -1, Synth::WaveBank::count(), 9999 })
        if (! Synth::WaveBank::framesFor (bad).empty())
            fail ("framesFor returned frames for index " + std::to_string (bad));
}

/** Taking a shape's spectrum gives the same answer as writing the series out.

    The check on the general method. Every shaped table in the bank, which is
    most of them, is only as correct as this step: a scaling or a phase
    convention wrong here puts every one of them slightly off, in a way that
    sounds like a waveform rather than like a bug.
*/
void theTransformAgreesWithTheKnownSeries()
{
    std::printf ("shape to spectrum against the known series\n");

    using namespace Synth::WaveShapes;

    const int harmonics = 200;

    struct Case
    {
        const char* name;
        Synth::Spectrum written;
        double (*shape) (double);
    };

    const Case cases[] =
    {
        // The shapes these series describe, written out. Getting one of
        // these wrong reads as the source being wrong, which cost a round
        // here: the first version of this file had a sawtooth running the
        // other way with its edge half a cycle off, and a triangle whose
        // formula was simply broken, and both were reported as the bank
        // failing.
        //
        // A sum of sines at one over h falls from a half to minus a half
        // across the cycle, with its edge at the start.
        { "sawtooth", saw (harmonics),
          [] (double t) { return 0.5 - t; } },

        // High for the first half of the cycle.
        { "square", pulse (harmonics, 0.5),
          [] (double t) { return t < 0.5 ? 1.0 : -1.0; } },

        // Up from zero to one at a quarter, down through zero to minus one
        // at three quarters, back to zero.
        { "triangle", triangle (harmonics),
          [] (double t)
          {
              if (t < 0.25) return 4.0 * t;
              if (t < 0.75) return 2.0 - 4.0 * t;
              return 4.0 * t - 4.0;
          } },
    };

    for (const auto& c : cases)
    {
        const auto taken = spectrumOf (c.shape, harmonics);

        // Compared as waveforms rather than harmonic by harmonic, because the
        // two can differ by an overall scale and still be the same waveform,
        // and because a phase written one way and measured another is the
        // same sound. The best fitting scale is divided out, so what is left
        // is a difference of shape.
        double cross = 0.0, square = 0.0, energy = 0.0;

        const int points = 2048;

        for (int i = 0; i < points; ++i)
        {
            const double t = (double) i / (double) points;
            const double a = waveformAt (c.written, t, harmonics);
            const double b = waveformAt (taken, t, harmonics);

            cross += a * b;
            square += a * a;
            energy += b * b;
        }

        const double scale = square > 0.0 ? cross / square : 0.0;

        double error = 0.0;

        for (int i = 0; i < points; ++i)
        {
            const double t = (double) i / (double) points;
            const double difference = waveformAt (taken, t, harmonics)
                                    - scale * waveformAt (c.written, t, harmonics);

            error += difference * difference;
        }

        const double dB = 20.0 * std::log10 (std::sqrt (error / energy) + 1e-300);

        std::printf ("    %-9s %s dB\n", c.name, show (dB).c_str());

        // Gibbs ringing at a discontinuity is real and is in both, so this is
        // not machine precision. Forty decibels down is far closer than two
        // different waveforms could ever land.
        if (! (dB < -40.0))
            fail (std::string ("the transform of a ") + c.name
                  + " differs from its own series by " + show (dB) + " dB");
    }
}

/** The phase modulation table is the spectrum the expansion says it is.

    sin(x + I sin x) expands to a sum over every whole number n, positive and
    negative, of J of n at the index times sin of (n plus one) x. Collecting
    that by harmonic matters and is the part that is easy to get wrong: the
    terms with n below minus one fold back onto positive harmonics with their
    sign reversed, so harmonic k carries

        J(k-1, I) + (-1)^k J(k+1, I)

    and not J(k-1, I) on its own. The first version of this check used the
    one term, and reported the bank as wrong by 0.48 at harmonic 1, which is
    exactly the size of the term it had left out.

    Worth having despite that, because it is the strongest check in this file
    on the shape to spectrum path. Unlike a sawtooth it exercises every
    harmonic with a different amplitude, the amplitudes are not monotonic, and
    nothing about it is a simple series, so almost any error in the transform
    shows up somewhere in it.
*/
void phaseModulationMatchesTheExpansion()
{
    std::printf ("phase modulation against the Jacobi-Anger expansion\n");

    const int index = Synth::WaveBank::indexFromId ("phase");

    if (index < 0)
    {
        fail ("there is no phase table to check");
        return;
    }

    const auto frames = Synth::WaveBank::framesFor (index);
    const int count = (int) frames.size();

    double worst = 0.0;
    double worstAt = 0.0;

    for (int f = 0; f < count; ++f)
    {
        // The same sweep the recipe uses.
        const double modulation = (count > 1 ? (double) f / (double) (count - 1) : 0.0) * 8.0;

        for (int h = 1; h <= 40; ++h)
        {
            const double lower = std::cyl_bessel_j ((double) (h - 1), modulation);
            const double folded = std::cyl_bessel_j ((double) (h + 1), modulation);
            const double expected = std::fabs (lower + (h % 2 == 0 ? 1.0 : -1.0) * folded);
            const double got = (std::size_t) (h - 1) < frames[(std::size_t) f].amplitude.size()
                             ? (double) frames[(std::size_t) f].amplitude[(std::size_t) (h - 1)]
                             : 0.0;

            const double difference = std::fabs (got - expected);

            if (difference > worst)
            {
                worst = difference;
                worstAt = modulation;
            }
        }
    }

    std::printf ("    worst harmonic off by %s, at index %s\n",
                 show (worst).c_str(), show (worstAt).c_str());

    if (! (worst < 0.002))
        fail ("the phase table is not the expansion: a harmonic is off by " + show (worst));
}

/** Sampling a shape folds its own high harmonics back down. How far down.

    The header claims the analysis fold lands eighty decibels below anything
    kept. A sawtooth is the test case because it has every harmonic at the
    slowest rolloff of any ordinary waveform, so it folds the most.
*/
void theAnalysisFoldIsFarEnoughDown()
{
    std::printf ("folding in the analysis itself\n");

    using namespace Synth::WaveShapes;

    const int harmonics = Synth::WaveTable::mostHarmonics;

    const auto written = saw (harmonics);
    const auto taken = spectrumOf ([] (double t) { return 2.0 * (t - std::floor (t + 0.5)); },
                                   harmonics);

    // Harmonic by harmonic this time, because folding shows up as individual
    // harmonics being wrong rather than as the waveform drifting.
    double worst = 0.0;
    int worstHarmonic = 0;

    // The written series is a reference up to an overall scale, so match them
    // on the fundamental first.
    const double scale = (double) taken.amplitude[0] / (double) written.amplitude[0];

    for (int h = 1; h <= harmonics; ++h)
    {
        const double want = scale * (double) written.amplitude[(std::size_t) (h - 1)];
        const double got = (double) taken.amplitude[(std::size_t) (h - 1)];

        const double error = std::fabs (got - want) / scale;

        if (error > worst)
        {
            worst = error;
            worstHarmonic = h;
        }
    }

    const double dB = 20.0 * std::log10 (worst + 1e-300);

    std::printf ("    worst harmonic off by %s dB relative to the fundamental, at harmonic %d\n",
                 show (dB).c_str(), worstHarmonic);

    if (! (dB < -80.0))
        fail ("the analysis folds its own harmonics back at " + show (dB) + " dB");
}

/** Sweeping the position changes the timbre, and does not change the level.

    The failure this is for does not look like a failure. A table whose frames
    differ by twenty decibels still plays, still sweeps, still sounds like
    something. It just means that putting an envelope or an LFO on the
    position control is also putting one on the volume, so every patch built
    on that table has a tremolo in it that nobody asked for and nobody can
    turn off.

    Peak and loudness are both measured because they say different things. A
    narrow pulse and a sine can have the same peak and be ten decibels apart
    in loudness, which is a real property of those waveforms and not a fault.
    What is a fault is a table where the two ends are far apart in both.
*/
void sweepingATableIsNotAVolumeRide()
{
    std::printf ("level across each table's frames\n");

    std::printf ("    %-10s %-16s %-16s\n", "table", "peak spread dB", "loudness spread dB");

    double worstPeak = 0.0;
    double worstRms = 0.0;
    std::string worstTable;

    for (int i = 0; i < Synth::WaveBank::count(); ++i)
    {
        const auto& entry = Synth::WaveBank::catalogue()[(std::size_t) i];
        const auto frames = Synth::WaveBank::framesFor (i);

        double lowPeak = 1e9, highPeak = 0.0;
        double lowRms = 1e9, highRms = 0.0;

        for (const auto& frame : frames)
        {
            const double peak = peakOf (frame);
            const double rms = rmsOf (frame);

            lowPeak = std::min (lowPeak, peak);
            highPeak = std::max (highPeak, peak);
            lowRms = std::min (lowRms, rms);
            highRms = std::max (highRms, rms);
        }

        const double peakSpread = 20.0 * std::log10 (highPeak / (lowPeak + 1e-300));
        const double rmsSpread = 20.0 * std::log10 (highRms / (lowRms + 1e-300));

        std::printf ("    %-10s %-16s %-16s\n", entry.id,
                     show (peakSpread).c_str(), show (rmsSpread).c_str());

        if (peakSpread > worstPeak)
        {
            worstPeak = peakSpread;
            worstTable = entry.id;
        }

        worstRms = std::max (worstRms, rmsSpread);
    }

    // Twelve decibels is already a lot: it is the difference between a sound
    // and the same sound at quarter volume. Anything past that is a table
    // whose position control is a fader.
    if (! (worstPeak < 12.0))
        fail ("the frames of " + worstTable + " are " + show (worstPeak)
              + " dB apart in peak, so sweeping it is a volume ride");

    if (! (worstRms < 24.0))
        fail ("a table's frames are " + show (worstRms) + " dB apart in loudness");
}

/** Every frame differs from the one before it.

    A recipe whose parameter does not actually reach some of its frames builds
    perfectly and gives a position control with a dead region in it. Measured
    as the difference between neighbouring frames, which also catches a recipe
    that moves fast at one end and not at all at the other.
*/
void everyFrameIsDifferentFromItsNeighbour()
{
    std::printf ("the position control moves everywhere\n");

    for (int i = 0; i < Synth::WaveBank::count(); ++i)
    {
        const auto& entry = Synth::WaveBank::catalogue()[(std::size_t) i];
        const auto frames = Synth::WaveBank::framesFor (i);

        double smallest = 1e9;
        std::size_t smallestAt = 0;

        for (std::size_t f = 1; f < frames.size(); ++f)
        {
            // Compared as spectra rather than as waveforms, because two
            // frames can have the same harmonics at different phases and
            // sound the same while looking different.
            const auto& a = frames[f - 1];
            const auto& b = frames[f];

            double difference = 0.0, size = 0.0;

            const std::size_t n = std::max (a.amplitude.size(), b.amplitude.size());

            for (std::size_t h = 0; h < n; ++h)
            {
                const double x = h < a.amplitude.size() ? (double) a.amplitude[h] : 0.0;
                const double y = h < b.amplitude.size() ? (double) b.amplitude[h] : 0.0;

                difference += (x - y) * (x - y);
                size += x * x + y * y;
            }

            const double relative = std::sqrt (difference / (size * 0.5 + 1e-300));

            if (relative < smallest)
            {
                smallest = relative;
                smallestAt = f;
            }
        }

        if (! (smallest > 0.01))
            fail (std::string (entry.id) + " barely changes between frames "
                  + std::to_string (smallestAt - 1) + " and " + std::to_string (smallestAt)
                  + ": " + show (smallest));
    }
}

/** The whole bank built, and what it costs. */
/** The harmonics table starts on a single partial.

    Worth its own check for two reasons. The bank has to contain a sine
    somewhere, because it is the most basic waveform there is and is what a
    wavefolder wants in front of it: fold a sine and the result is almost
    entirely odd harmonics, which is the whole character of a west coast
    patch, and fold something that already has an octave in it and the result
    is a mess.

    And because this is exactly where it went wrong. The table's own comment
    said the first frame was a sine while the code gave it harmonics two and
    three at two thirds and a third, because the band was three harmonics
    wide either side and, sitting on the fundamental, three harmonics either
    side reaches upward only. A band measured as a ratio cannot do that.
*/
void theHarmonicsTableStartsOnASine()
{
    std::printf ("the harmonics table's first frame\n");

    const int index = Synth::WaveBank::indexFromId ("harmonics");

    if (index < 0)
    {
        fail ("there is no harmonics table");
        return;
    }

    const auto frames = Synth::WaveBank::framesFor (index);

    if (frames.empty())
    {
        fail ("the harmonics table has no frames");
        return;
    }

    const auto& first = frames.front();

    if (first.amplitude.empty())
    {
        fail ("the first frame has no harmonics at all");
        return;
    }

    double loudestAbove = 0.0;
    int where = 0;

    for (std::size_t h = 1; h < first.amplitude.size(); ++h)
        if ((double) first.amplitude[h] > loudestAbove)
        {
            loudestAbove = (double) first.amplitude[h];
            where = (int) h + 1;
        }

    const double fundamental = (double) first.amplitude[0];

    std::printf ("    fundamental %.4f, loudest harmonic above it %.4f at %d\n",
                 fundamental, loudestAbove, where);

    if (! (fundamental > 0.5))
        fail ("the first frame's fundamental is only " + show (fundamental));

    // A thousandth of the fundamental, which is 60 dB down and is as near a
    // single partial as a generated spectrum gets.
    if (! (loudestAbove < fundamental * 0.001))
        fail ("the first frame is not a sine: harmonic " + std::to_string (where)
              + " is at " + show (loudestAbove / fundamental) + " of the fundamental");

    // And the band climbs by the same ratio every frame, which is what
    // keeps the sweep even. Measured as the centre of gravity of each
    // frame's spectrum and not as its loudest harmonic: a loudest harmonic
    // has to land on a whole number, so at the bottom of the sweep it
    // quantises badly and reads as a jump of a whole octave where the band
    // has moved a third of one.
    std::printf ("    the band's centre of gravity, frame by frame:");

    double previous = 0.0;
    double smallestStep = 1e9, largestStep = 0.0;

    for (const auto& frame : frames)
    {
        double weighted = 0.0, total = 0.0;

        for (std::size_t h = 0; h < frame.amplitude.size(); ++h)
        {
            weighted += (double) (h + 1) * (double) frame.amplitude[h];
            total += (double) frame.amplitude[h];
        }

        const double centroid = total > 0.0 ? weighted / total : 0.0;

        std::printf (" %.1f", centroid);

        if (previous > 0.0)
        {
            const double step = centroid / previous;
            smallestStep = std::min (smallestStep, step);
            largestStep = std::max (largestStep, step);
        }

        previous = centroid;
    }

    std::printf ("\n");

    // Sixteen frames spanning harmonic one to twenty four geometrically is a
    // step of 24 to the power of a fifteenth, which is 1.23. Held loosely,
    // because a band clipped against the fundamental at the bottom of the
    // sweep pulls its own centre of gravity upward.
    if (! (smallestStep > 1.1 && largestStep < 1.45))
        fail ("the band's centre does not climb evenly: steps from "
              + show (smallestStep) + " to " + show (largestStep));
}

void theBankBuildsWithinItsBudget()
{
    std::printf ("the whole bank\n");

    Synth::WaveBank bank;
    bank.buildAll();

    for (int i = 0; i < Synth::WaveBank::count(); ++i)
        if (bank.table (i) == nullptr)
            fail (std::string (Synth::WaveBank::idAt (i)) + " did not build");

    for (int bad : { -1, Synth::WaveBank::count(), 9999 })
        if (bank.table (bad) != nullptr)
            fail ("the bank returned a table for index " + std::to_string (bad));

    const double megabytes = (double) bank.memoryBytes() / (1024.0 * 1024.0);

    std::printf ("    %d tables: %s MB\n", Synth::WaveBank::count(), show (megabytes).c_str());

    // This is resident for as long as the studio is open, on top of whatever
    // the user's own plugins want. A budget stated here is a budget somebody
    // has to argue with before adding the eleventh table.
    if (! (megabytes < 48.0))
        fail ("the bank costs " + show (megabytes) + " MB");
}

} // namespace

int main()
{
    std::printf ("Synth::WaveBank\n\n");

    theCatalogueIsWellFormed();
    everyRecipeProducesItsFrames();
    theTransformAgreesWithTheKnownSeries();
    phaseModulationMatchesTheExpansion();
    theAnalysisFoldIsFarEnoughDown();
    everyFrameIsDifferentFromItsNeighbour();
    sweepingATableIsNotAVolumeRide();
    theHarmonicsTableStartsOnASine();
    theBankBuildsWithinItsBudget();

    std::printf ("\n");

    if (failures > 0)
    {
        std::printf ("%d check(s) failed.\n", failures);
        return 1;
    }

    std::printf ("All checks passed.\n");
    return 0;
}
