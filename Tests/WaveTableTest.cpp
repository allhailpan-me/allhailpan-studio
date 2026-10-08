// Checks Synth/WaveTable.h, where the thing that matters is a negative: the
// oscillator must not produce anything that is not a harmonic of the note.
//
// Aliasing is the failure this file exists for. A waveform carrying harmonics
// above half the sample rate does not lose them, it folds them back down onto
// frequencies that are not multiples of the note. They do not move with the
// note either, so a chromatic run sends them walking the other way, and no
// filter afterwards can take them out because by then they are inside the
// audible band. CLAUDE.md calls it the single thing that makes a soft synth
// sound amateur, and it is invisible to every test that only asks whether a
// number came out.
//
// Two different kinds of check here, because one on its own would not be
// enough.
//
// The first is the design argument, asserted exhaustively rather than
// reasoned about: over millions of combinations of note and sample rate, the
// level the table picks, and the level above it that gets mixed with it, both
// hold fewer harmonics than that note can carry. If that holds everywhere
// then aliasing is impossible by construction, and the proof is a loop rather
// than a paragraph.
//
// The second is the measurement, because a design argument only covers what
// it was written about. A note is rendered, its spectrum taken through a
// window with very low sidelobes, and every bin that is not near a harmonic
// is required to be silent. That catches anything the argument missed: an
// interpolation that smears, a level that was built wrong, a read that walks
// off the end of a frame.
//
// A measurement that cannot fail proves nothing, so the alias detector is
// first pointed at a deliberately naive sawtooth, and is required to catch
// it. If that ever stops failing, this file has stopped working.

#include "Synth/WaveTable.h"

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
    char buffer[32];
    std::snprintf (buffer, sizeof (buffer), "%.4f", v);
    return buffer;
}

constexpr double pi = 3.14159265358979323846;

//==============================================================================
/** A sawtooth as harmonics: every harmonic at one over its number.

    The hardest case the table has, and the one every classic synth is judged
    on. A square or a triangle has gaps in its series or falls off faster;
    a saw has everything, at the slowest rolloff of any ordinary waveform, so
    if anything is going to alias it is this.
*/
Synth::Spectrum sawSpectrum (int harmonics)
{
    Synth::Spectrum s;

    for (int h = 1; h <= harmonics; ++h)
    {
        s.amplitude.push_back ((float) (1.0 / (double) h));

        // Every harmonic a quarter turn back, which turns the cosines the
        // build uses into sines and so puts the tooth's edge exactly at the
        // start of the cycle.
        //
        // That placement is the point. The table stores a guard sample on
        // either side of the cycle so a four point read never has to wrap,
        // and those guards only matter where the waveform is doing something
        // across the join. An earlier version of this file built its saw from
        // cosines with alternating signs, which has the same spectrum and a
        // perfectly smooth join, and three deliberate breakages of the guard
        // samples went unnoticed because there was nothing at the join to
        // break.
        s.phase.push_back ((float) (-pi * 0.5));
    }

    return s;
}

/** The waveform a spectrum describes, computed exactly, with no table and no
    interpolation anywhere in it. The reference everything is measured
    against, and deliberately written from the Spectrum the table was built
    from rather than from a second copy of the formula. */
double exactly (const Synth::Spectrum& s, int harmonics, double phase)
{
    double sum = 0.0;

    const int highest = std::min (harmonics, (int) s.amplitude.size());

    for (int h = 1; h <= highest; ++h)
    {
        const double amplitude = (double) s.amplitude[(std::size_t) (h - 1)];
        const double offset = (std::size_t) (h - 1) < s.phase.size()
                            ? (double) s.phase[(std::size_t) (h - 1)] : 0.0;

        sum += amplitude * std::cos (2.0 * pi * (double) h * phase + offset);
    }

    return sum;
}

/** A table that changes along its row: a sine at one end and a full saw at
    the other, which is the sweep a position control is for. */
std::vector<Synth::Spectrum> morphingFrames (int frames, int harmonics)
{
    std::vector<Synth::Spectrum> all;

    for (int f = 0; f < frames; ++f)
    {
        const double amount = frames > 1 ? (double) f / (double) (frames - 1) : 0.0;

        Synth::Spectrum s;

        for (int h = 1; h <= harmonics; ++h)
        {
            // The fundamental is always there; the rest come in as the
            // position moves, higher harmonics last.
            const double reach = amount * (double) harmonics;
            const double present = h == 1 ? 1.0 : std::clamp (reach - (double) h + 1.0, 0.0, 1.0);

            s.amplitude.push_back ((float) (present / (double) h));
            s.phase.push_back (0.0f);
        }

        all.push_back (std::move (s));
    }

    return all;
}

//==============================================================================
/** How much of a rendered signal sits away from the harmonics of `frequency`,
    relative to the loudest harmonic, in decibels.

    Minus infinity would be a perfectly clean tone. Anything above about -70
    is audible shimmer on a sustained note.

    The frequency deliberately does not land on a bin centre. If it did, every
    aliased partial would fold onto a bin that is also a harmonic, because a
    waveform that repeats exactly still repeats exactly after its harmonics
    have folded, and the measurement would show nothing at all. Off the grid,
    folded partials land between harmonics, where they can be seen.

    The window is Blackman-Harris, whose sidelobes are about 92 dB down, so
    leakage from the real harmonics stays well under the level being looked
    for. Bins within six of a harmonic are skipped, which is its main lobe.
*/
double aliasFloorDb (const std::vector<float>& rendered, double frequency, double sampleRate)
{
    const int n = (int) rendered.size();

    RealFFT fft (n);

    std::vector<float> windowed ((std::size_t) n);

    // Blackman-Harris, four term.
    const double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;

    for (int i = 0; i < n; ++i)
    {
        const double t = 2.0 * pi * (double) i / (double) (n - 1);
        const double w = a0 - a1 * std::cos (t) + a2 * std::cos (2.0 * t) - a3 * std::cos (3.0 * t);
        windowed[(std::size_t) i] = (float) ((double) rendered[(std::size_t) i] * w);
    }

    std::vector<std::complex<double>> bins ((std::size_t) (n / 2 + 1));
    fft.forward (windowed.data(), bins.data());

    const double binHz = sampleRate / (double) n;

    double loudestHarmonic = 0.0;
    double loudestStray = 0.0;

    for (int k = 1; k <= n / 2; ++k)
    {
        const double hz = (double) k * binHz;
        const double magnitude = std::abs (bins[(std::size_t) k]);

        // Which harmonic this bin is nearest, and how far off it is.
        const double harmonic = hz / frequency;
        const double nearest = std::floor (harmonic + 0.5);
        const double offBins = std::fabs (hz - nearest * frequency) / binHz;

        if (nearest >= 1.0 && offBins <= 6.0)
        {
            loudestHarmonic = std::max (loudestHarmonic, magnitude);
            continue;
        }

        // Below the fundamental there is nothing legitimate, so those bins
        // count as stray, except for the handful nearest DC. The window has a
        // main lobe four bins wide and the fundamental of a low note is only
        // twenty or so bins up, so the bottom of the spectrum is the
        // fundamental's own skirt rather than anything the oscillator did.
        // Reading it as a fault made a 37 hertz note look like it was
        // aliasing at -47 dB when the real figure was below -90.
        if (k >= 10)
            loudestStray = std::max (loudestStray, magnitude);
    }

    if (! (loudestHarmonic > 0.0))
        return 0.0;

    return 20.0 * std::log10 ((loudestStray + 1e-300) / loudestHarmonic);
}

//==============================================================================
/** The design argument, asserted rather than reasoned about.

    If both levels that a note can read hold fewer harmonics than that note
    has room for, aliasing is impossible, whatever the rest of the code does
    with them. This is the check that makes the rest of the file a confirmation
    rather than the proof.
*/
void everyLevelIsSafeForEveryNote()
{
    std::printf ("every level a note can read is band limited for that note\n");

    const double rates[] = { 22050.0, 32000.0, 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };

    int worstCase = 0;
    double dullest = 1.0;

    for (double rate : rates)
    {
        const double nyquist = rate * 0.5;

        // Every tenth of a semitone from below the lowest note on a piano to
        // above the top of hearing, which is further than anybody will play
        // and cheap enough to just do.
        for (double semitone = -24.0; semitone <= 138.0; semitone += 0.1)
        {
            const double frequency = 8.1757989156 * std::pow (2.0, semitone / 12.0);

            if (frequency >= nyquist)
                break;

            const double level = Synth::WaveTable::levelFor (frequency, rate);

            const int lower = (int) level;
            const int upper = std::min (lower + 1, Synth::WaveTable::numLevels - 1);

            // Both, because read() mixes them. Checking only the one the
            // level lands in would miss exactly the mistake this design was
            // arranged to avoid.
            for (int which : { lower, upper })
            {
                const int harmonics = Synth::WaveTable::harmonicsAtLevel (which);
                const double top = (double) harmonics * frequency;

                if (top > nyquist)
                {
                    fail ("level " + std::to_string (which) + " carries " + std::to_string (harmonics)
                          + " harmonics, which reaches " + show (top) + " hz at "
                          + show (frequency) + " hz, past Nyquist at " + show (nyquist));
                    ++worstCase;

                    if (worstCase > 5)
                        return;
                }
            }

            // How much brightness the level arrangement gives away. Only
            // where the mipmap is actually the limit: at the very bottom of
            // the range it returns level zero and the limit is the richest
            // table there is, which is a separate decision recorded in the
            // header and not what this is measuring.
            // Strictly inside the range: at the bottom it returns level zero
            // and the limit is the richest table there is, at the top it
            // saturates at a single harmonic and the limit is the sample
            // rate. Both are separate decisions recorded in the header, and
            // neither is what this is measuring.
            if (level > 0.0 && level < (double) (Synth::WaveTable::numLevels - 1))
            {
                const int harmonics = Synth::WaveTable::harmonicsAtLevel (lower);

                // Only where there are enough harmonics for the geometric
                // spacing to be what decides the count. Near the top of the
                // mipmap a level holds two or three harmonics and the whole
                // numbers are what round it down, not the arrangement: a
                // level holding two where it could carry three gives up a
                // partial at 21 kHz on a 7 kHz note, which is above hearing.
                // Measuring that as dullness would be measuring arithmetic
                // that cannot be otherwise. From 32 harmonics upward the
                // rounding is worth under three percent and what is left is
                // the arrangement itself.
                if (harmonics >= 32)
                    dullest = std::min (dullest, (double) harmonics * frequency / nyquist);
            }
        }
    }

    std::printf ("    at worst the top harmonic reaches %s of Nyquist\n", show (dullest).c_str());

    // Four levels per octave gives away 2^(1/4), which is 0.841, and the
    // whole number of harmonics takes a little more.
    if (! (dullest > 0.81))
        fail ("the table is dull: top harmonic only reaches " + show (dullest) + " of Nyquist");
}

/** Levels get smaller going up, and every level has room for its harmonics. */
void levelsAreWellFormed()
{
    std::printf ("levels are well formed\n");

    int previous = Synth::WaveTable::mostHarmonics + 1;

    for (int level = 0; level < Synth::WaveTable::numLevels; ++level)
    {
        const int harmonics = Synth::WaveTable::harmonicsAtLevel (level);
        const int length = Synth::WaveTable::lengthAtLevel (level);

        if (harmonics > previous)
            fail ("level " + std::to_string (level) + " has more harmonics than the one below it");

        previous = harmonics;

        if (harmonics < 1)
            fail ("level " + std::to_string (level) + " has no harmonics at all");

        // The sampling theorem applied to one cycle. A table shorter than
        // this cannot describe its own harmonics, and the ones that do not
        // fit would fold inside the table itself.
        if (length < 2 * harmonics && length < Synth::WaveTable::longestLevel)
            fail ("level " + std::to_string (level) + " is " + std::to_string (length)
                  + " samples, too short for " + std::to_string (harmonics) + " harmonics");

        if ((length & (length - 1)) != 0)
            fail ("level " + std::to_string (level) + " length is not a power of two");
    }

    if (Synth::WaveTable::harmonicsAtLevel (Synth::WaveTable::numLevels - 1) != 1)
        fail ("the top level should be a single harmonic, which is safe at any frequency");
}

//==============================================================================
/** Asking for a known spectrum gives back that spectrum.

    This is the one check on the bin scaling in build(). The derivation says a
    harmonic of amplitude A needs a bin of A times length over two, because
    the inverse transform scales by one over length and the conjugate pair
    contributes twice. Rather than trust that, ask for a single harmonic at a
    known amplitude and phase and measure what comes out.
*/
void aRequestedSpectrumComesBack()
{
    std::printf ("a requested harmonic comes back at its own amplitude and phase\n");

    for (int harmonic : { 1, 2, 7, 32 })
        for (double phase : { 0.0, pi * 0.5, pi })
        {
            Synth::Spectrum s;
            s.amplitude.assign ((std::size_t) harmonic, 0.0f);
            s.phase.assign ((std::size_t) harmonic, 0.0f);
            s.amplitude[(std::size_t) (harmonic - 1)] = 1.0f;
            s.phase[(std::size_t) (harmonic - 1)] = (float) phase;

            Synth::WaveTable table;
            table.build ({ s });

            // Read one whole cycle at the richest level. The table is
            // normalised to a peak of one, and a single harmonic is a cosine,
            // so what comes back should be exactly cos(2 pi h t + phase).
            const int points = 512;

            double worst = 0.0;

            for (int i = 0; i < points; ++i)
            {
                const double t = (double) i / (double) points;
                const double expected = std::cos (2.0 * pi * (double) harmonic * t + phase);
                const double actual = table.read (t, 0.0, 0.0);

                worst = std::max (worst, std::fabs (actual - expected));
            }

            if (! (worst < 2e-3))
                fail ("harmonic " + std::to_string (harmonic) + " at phase " + show (phase)
                      + " came back wrong by " + show (worst));
        }
}

/** The alias detector has to be able to fail, or none of its passes mean
    anything. Pointed at a naive saw, which is nothing but aliasing above a
    few hundred hertz, it has to say so loudly. */
void theDetectorCatchesANaiveSaw()
{
    std::printf ("the alias detector catches a naive sawtooth\n");

    const double rate = 48000.0;
    const double frequency = 2000.0 + 1.0 / 3.0;
    const int points = 16384;

    std::vector<float> rendered ((std::size_t) points);

    double phase = 0.0;
    const double increment = frequency / rate;

    for (int i = 0; i < points; ++i)
    {
        rendered[(std::size_t) i] = (float) (2.0 * phase - 1.0);
        phase += increment;

        if (phase >= 1.0)
            phase -= 1.0;
    }

    const double floorDb = aliasFloorDb (rendered, frequency, rate);

    std::printf ("    naive saw measures %s dB\n", show (floorDb).c_str());

    if (! (floorDb > -40.0))
        fail ("the detector did not notice a naive sawtooth, so it cannot be trusted "
              "to notice a real fault either: it measured " + show (floorDb) + " dB");
}

/** The real check: a full saw played all over the keyboard, with nothing off
    the harmonic grid. */
void nothingAliasesAcrossTheKeyboard()
{
    std::printf ("a full sawtooth does not alias anywhere on the keyboard\n");

    Synth::WaveTable table;
    table.build ({ sawSpectrum (Synth::WaveTable::mostHarmonics) });

    const double rates[] = { 44100.0, 48000.0, 96000.0 };

    // Long enough that the fundamental of the lowest note tested is twenty
    // bins up, which keeps the window's skirt clear of the bins being read
    // as stray.
    const int points = 32768;

    double worst = -1000.0;
    double worstAt = 0.0;
    double worstRate = 0.0;

    for (double rate : rates)
    {
        // Every semitone from two octaves below middle C to the top of the
        // keyboard and beyond, nudged off the bin grid so that folded
        // partials have somewhere visible to land.
        for (int semitone = 24; semitone <= 120; semitone += 1)
        {
            const double frequency = 440.0 * std::pow (2.0, (double) (semitone - 69) / 12.0)
                                   + 0.37;

            if (frequency > rate * 0.45)
                break;

            Synth::WaveTableOscillator osc;
            osc.setTable (&table);
            osc.setSampleRate (rate);
            osc.setFrequency (frequency);
            osc.setPosition (0.0);
            osc.reset();

            std::vector<float> rendered ((std::size_t) points);

            for (int i = 0; i < points; ++i)
                rendered[(std::size_t) i] = osc.nextSample();

            const double floorDb = aliasFloorDb (rendered, frequency, rate);

            if (floorDb > worst)
            {
                worst = floorDb;
                worstAt = frequency;
                worstRate = rate;
            }
        }
    }

    std::printf ("    worst stray partial %s dB, at %s hz and %s hz sample rate\n",
                 show (worst).c_str(), show (worstAt).c_str(), show (worstRate).c_str());

    // Seventy decibels below the loudest harmonic is far below anything
    // audible against the note producing it, and a broken band limit comes in
    // thirty or more decibels above this.
    if (! (worst < -70.0))
        fail ("stray partials reach " + show (worst) + " dB at " + show (worstAt) + " hz");
}

/** Sweeping the position control does not alias either.

    Worth its own check: the position is a crossfade between two frames, and a
    crossfade of two band limited waveforms is band limited, but only if both
    frames came from the same level. Reading one frame from one level and the
    other from another would not, and is an easy mistake to make.
*/
void morphingDoesNotAlias()
{
    std::printf ("sweeping the position control does not alias\n");

    Synth::WaveTable table;
    table.build (morphingFrames (16, Synth::WaveTable::mostHarmonics));

    const double rate = 48000.0;
    const int points = 16384;

    double worst = -1000.0;

    for (double frequency : { 110.37, 440.37, 1200.37, 3000.37 })
        for (double position : { 0.0, 0.17, 0.5, 0.83, 1.0 })
        {
            Synth::WaveTableOscillator osc;
            osc.setTable (&table);
            osc.setSampleRate (rate);
            osc.setFrequency (frequency);
            osc.setPosition (position);
            osc.reset();

            std::vector<float> rendered ((std::size_t) points);

            for (int i = 0; i < points; ++i)
                rendered[(std::size_t) i] = osc.nextSample();

            worst = std::max (worst, aliasFloorDb (rendered, frequency, rate));
        }

    std::printf ("    worst stray partial %s dB\n", show (worst).c_str());

    if (! (worst < -70.0))
        fail ("morphing strays to " + show (worst) + " dB");
}

/** The table read is accurate, not merely free of aliasing.

    These are different claims and the difference is what the design turns on.
    An interpolation error is a periodic distortion, so most of it lands on
    harmonics of the note rather than between them, where the alias check
    above cannot see it. A table read with the wrong interpolator, or built
    too short, stays under that check's threshold while being audibly wrong:
    it comes out as a timbre that is not the one in the table, worst on the
    waveforms with the most going on in them.

    So this compares the table against the waveform it is supposed to be,
    summed harmonic by harmonic with no table and no interpolation involved.
    The difference between the two is the whole error, wherever it lands.

    Read at phases drawn at random across the whole cycle, rather than by
    stepping through it. Stepping seemed more realistic and was worse: at a
    small index step a full cycle of the longest level takes sixty thousand
    samples, so a few thousand samples never reach the wrap at all, and three
    deliberate breakages of the samples stored around the wrap went unnoticed
    because of it. Random phases cover the wrap, and cover the interpolation
    fraction uniformly, which is the thing that actually matters here.

    This is the check that two other deliberate breakages walked straight
    through when it was missing: halving the table length, and replacing the
    cubic with a linear read. Both stayed under the aliasing threshold and
    both put the error up by more than twenty decibels.
*/
void theTableReadsWhatItHolds()
{
    std::printf ("the table reads back the waveform it was built from\n");

    const auto spectrum = sawSpectrum (Synth::WaveTable::mostHarmonics);

    Synth::WaveTable table;
    table.build ({ spectrum });

    // Shape is compared, not level. The table normalises itself by its
    // loudest stored sample and the reference has no such step, and for a
    // sawtooth the continuous peak sits between two stored samples, so the
    // two differ by about a tenth of a percent. That is a scale difference
    // and not a waveform error, but left in it swamps everything: it reads
    // as -60 dB and hides the thing this check is for. So the best fitting
    // scale is divided out and what is left is the departure from the shape.
    // The level itself is checked by normalisationIsWholeTable.

    std::mt19937 rng (2718281828u);
    std::uniform_real_distribution<double> anywhere (0.0, 1.0);

    double worstOverall = -1000.0;
    int worstLevel = 0;
    double worstJoin = -1000.0;

    for (int level : { 0, 1, 5, 9, 13, 20, 30 })
    {
        const int harmonics = Synth::WaveTable::harmonicsAtLevel (level);
        const int length = Synth::WaveTable::lengthAtLevel (level);

        // Two measurements, because they answer different questions.
        //
        // Anywhere in the cycle is what a note actually does: the oscillator
        // walks the phase evenly, so this is the error somebody hears.
        //
        // Crowded into the two samples either side of the join is a hard spot
        // deliberately over weighted about a thousand times. It is where the
        // stored guard samples are the only thing holding the waveform
        // together, and on a sawtooth it is also the steepest part of the
        // waveform, where a cubic has the least to work with. Judging it by
        // the same number as the rest would either let a broken guard through
        // or condemn an interpolator that is doing as well as a cubic can.
        for (int pass = 0; pass < 2; ++pass)
        {
            const bool atTheJoin = pass == 1;

            std::vector<double> wanted, got;
            wanted.reserve (6000);
            got.reserve (6000);

            for (int i = 0; i < 6000; ++i)
            {
                double phase = anywhere (rng);

                if (atTheJoin)
                {
                    const double edge = 2.0 / (double) length;
                    phase = phase < 0.5 ? phase * 2.0 * edge
                                        : 1.0 - (phase - 0.5) * 2.0 * edge;
                }

                wanted.push_back (exactly (spectrum, harmonics, phase));
                got.push_back (table.read (phase, 0.0, (double) level));
            }

            double cross = 0.0, square = 0.0;

            for (std::size_t i = 0; i < wanted.size(); ++i)
            {
                cross += wanted[i] * got[i];
                square += wanted[i] * wanted[i];
            }

            const double scale = square > 0.0 ? cross / square : 0.0;

            double error = 0.0, reference = 0.0;

            for (std::size_t i = 0; i < wanted.size(); ++i)
            {
                const double difference = got[i] - scale * wanted[i];

                error += difference * difference;
                reference += scale * scale * wanted[i] * wanted[i];
            }

            const double dB = 20.0 * std::log10 (std::sqrt (error / reference) + 1e-300);

            if (atTheJoin)
            {
                worstJoin = std::max (worstJoin, dB);
            }
            else if (dB > worstOverall)
            {
                worstOverall = dB;
                worstLevel = level;
            }
        }
    }

    std::printf ("    across the cycle %s dB, worst at level %d\n",
                 show (worstOverall).c_str(), worstLevel);
    std::printf ("    crowded onto the join %s dB\n", show (worstJoin).c_str());

    // Sixteen times oversampled and read with a cubic measures about -95 dB
    // across the cycle. Twice oversampled and read linearly, which is what
    // the sampling theorem alone suggests, measures about -38.
    if (! (worstOverall < -85.0))
        fail ("the table reads back at " + show (worstOverall) + " dB from what it holds");

    // At the join a correct read measures about -60 dB on a sawtooth, and a
    // guard sample stored wrong measures far above that.
    if (! (worstJoin < -50.0))
        fail ("the waveform at the join reads back at " + show (worstJoin) + " dB");
}

/** The position control is exactly a crossfade between its two neighbouring
    frames, and nothing else.

    Exactly, to the last bit, because the read blends the four stored points
    before the cubic rather than blending two cubics, on the grounds that a
    cubic is a weighted sum of its points and the two orders therefore give
    the same answer at a third of the work. That is an optimisation, and this
    is the check that it really is one: a partial blend, where some of the
    points come from one frame and some from the other, is a waveform that is
    in neither table and would pass every other check in this file.
*/
void theFrameBlendIsExactlyLinear()
{
    std::printf ("the position control is exactly a crossfade\n");

    Synth::WaveTable table;
    table.build (morphingFrames (6, 200));

    std::mt19937 rng (161803398u);
    std::uniform_real_distribution<double> unit (0.0, 1.0);

    double worst = 0.0;

    for (int i = 0; i < 40000; ++i)
    {
        const double phase = unit (rng);
        const int frame = (int) (unit (rng) * 4.0);
        const double blend = unit (rng);
        const double level = unit (rng) * 20.0;

        const double mixed = table.read (phase, (double) frame + blend, level);

        const double a = table.read (phase, (double) frame, level);
        const double b = table.read (phase, (double) frame + 1.0, level);
        const double expected = a + (b - a) * blend;

        worst = std::max (worst, std::fabs (mixed - expected));
    }

    std::printf ("    worst departure from a straight crossfade %s\n", show (worst).c_str());

    // Float storage and a double accumulator, so the two orders agree to
    // about a float epsilon rather than exactly.
    if (! (worst < 1e-6))
        fail ("the frame blend departs from a crossfade by " + show (worst));
}

/** Crossing a level boundary is not a step.

    The reason the mipmap has four levels per octave and a crossfade between
    them. A glide, a pitch envelope or a vibrato crosses these boundaries
    constantly, and a step in the waveform at each crossing is the tick that
    gives a wavetable oscillator away.
*/
void levelChangesAreSmooth()
{
    std::printf ("crossing a level boundary is not a step\n");

    Synth::WaveTable table;
    table.build ({ sawSpectrum (Synth::WaveTable::mostHarmonics) });

    const double rate = 48000.0;

    // Walk the frequency across several boundaries and watch what the table
    // gives at one fixed point of the waveform. A step would show up as a
    // jump between neighbouring frequencies.
    double worstJump = 0.0;
    double worstNear = 0.0;

    for (double frequency = 100.0; frequency < 8000.0; frequency *= 1.0005)
    {
        const double here = Synth::WaveTable::levelFor (frequency, rate);
        const double next = Synth::WaveTable::levelFor (frequency * 1.0005, rate);

        const double a = table.read (0.3, 0.0, here);
        const double b = table.read (0.3, 0.0, next);

        const double jump = std::fabs (b - a);

        if (jump > worstJump)
        {
            worstJump = jump;
            worstNear = frequency;
        }
    }

    std::printf ("    largest step %s, near %s hz\n",
                 show (worstJump).c_str(), show (worstNear).c_str());

    // Two thousandths of full scale between neighbouring frequencies half a
    // cent apart. A hard switch between octave tables, which is what the
    // reference design does and warns about, measures two orders of magnitude
    // above this.
    if (! (worstJump < 0.002))
        fail ("level change steps by " + show (worstJump) + " near " + show (worstNear) + " hz");
}

/** Position 0 is the first frame and position 1 is the last, with a blend in
    between rather than a jump at either end. */
void positionReachesBothEnds()
{
    std::printf ("the position control reaches both ends of the table\n");

    const int frames = 8;

    Synth::WaveTable table;
    table.build (morphingFrames (frames, 64));

    if (table.getNumFrames() != frames)
        fail ("the table did not keep all its frames");

    // The first frame is a sine and the last is a full saw, so the two ends
    // have to look quite different from each other.
    double difference = 0.0;

    for (int i = 0; i < 256; ++i)
    {
        const double t = (double) i / 256.0;
        difference = std::max (difference,
                               (double) std::fabs (table.read (t, 0.0, 0.0)
                                                 - table.read (t, (double) (frames - 1), 0.0)));
    }

    if (! (difference > 0.2))
        fail ("both ends of the table read the same, so the position control does nothing");

    // And the middle is between them rather than being either one.
    for (int i = 0; i < 64; ++i)
    {
        const double t = (double) i / 64.0;

        const double a = table.read (t, 0.0, 0.0);
        const double b = table.read (t, (double) (frames - 1), 0.0);
        const double mid = table.read (t, (double) (frames - 1) * 0.5, 0.0);

        const double lo = std::min (a, b) - 0.35;
        const double hi = std::max (a, b) + 0.35;

        if (! (mid >= lo && mid <= hi))
            fail ("the middle of the table is outside both of its ends at phase " + show (t));
    }
}

/** The table is normalised, and normalised once for the whole thing.

    Per level or per frame would make a note change loudness as it is played
    up the keyboard, or as the position control is swept, which would make a
    modulated position a volume ride as well as a timbre one. */
void normalisationIsWholeTable()
{
    std::printf ("normalisation\n");

    Synth::WaveTable table;
    table.build (morphingFrames (8, 256));

    double peak = 0.0;

    for (int level = 0; level < Synth::WaveTable::numLevels; level += 3)
        for (int frame = 0; frame < 8; ++frame)
            for (int i = 0; i < 512; ++i)
                peak = std::max (peak, std::fabs ((double) table.read ((double) i / 512.0,
                                                                       (double) frame,
                                                                       (double) level)));

    if (! (peak > 0.9 && peak <= 1.0001))
        fail ("the table peaks at " + show (peak) + " rather than at one");

    // A sine frame should be quieter than a saw frame, because they were
    // scaled by the same number. If each frame had been normalised on its own
    // they would both peak at one and this would fail.
    double sinePeak = 0.0, sawPeak = 0.0;

    for (int i = 0; i < 512; ++i)
    {
        sinePeak = std::max (sinePeak, std::fabs ((double) table.read ((double) i / 512.0, 0.0, 0.0)));
        sawPeak  = std::max (sawPeak,  std::fabs ((double) table.read ((double) i / 512.0, 7.0, 0.0)));
    }

    if (! (sinePeak < sawPeak * 0.95))
        fail ("every frame peaks at the same height, so they were normalised separately");
}

/** A whole mipmap has to be affordable, or the instrument cannot ship ten of
    them. The variable length per level is what buys this, and a change that
    quietly undid it would otherwise only show up as memory use on somebody
    else's machine. */
void theMipmapFitsInABudget()
{
    std::printf ("memory\n");

    Synth::WaveTable table;
    table.build (morphingFrames (16, Synth::WaveTable::mostHarmonics));

    const double megabytes = (double) table.memoryBytes() / (1024.0 * 1024.0);

    std::printf ("    sixteen frames, %d levels: %s MB\n",
                 Synth::WaveTable::numLevels, show (megabytes).c_str());

    // Every level at the longest length would be 37 x 8195 x 16 floats, which
    // is 18.5 MB, so the shrinking is worth about five times. Most tables
    // will carry eight frames rather than sixteen and cost half of this.
    if (! (megabytes < 4.5))
        fail ("the mipmap costs " + show (megabytes) + " MB, so the levels are not shrinking");
}

/** Anything a modulation source can hand the oscillator has to be survivable.

    Phase modulation can push the phase anywhere at all, including far
    negative, and a position or a level can arrive out of range from an
    envelope that overshoots. None of that may read off the end of a table.
    The sanitizers are what actually catch that; this is the loop that gives
    them something to catch. */
void nothingUnreasonableBreaksIt()
{
    std::printf ("out of range input\n");

    Synth::WaveTable table;
    table.build (morphingFrames (4, 128));

    // A phase outside the cycle wraps round it rather than being pushed back
    // to the start. This matters the moment one oscillator modulates
    // another's phase: the offset routinely carries the phase several whole
    // cycles past the end, and a read that clamped instead of wrapping would
    // flatten the waveform to its first sample exactly when the modulation is
    // deepest, which is the setting the sound is being made at.
    {
        std::mt19937 rng (12345);
        std::uniform_real_distribution<double> unit (0.0, 1.0);

        double worst = 0.0;

        for (int i = 0; i < 20000; ++i)
        {
            const double phase = unit (rng);
            const double turns = std::floor (unit (rng) * 2000.0) - 1000.0;

            worst = std::max (worst, (double) std::fabs (table.read (phase, 1.0, 3.0)
                                                       - table.read (phase + turns, 1.0, 3.0)));
        }

        if (! (worst < 1e-5))
            fail ("a phase a whole number of cycles along reads differently, by " + show (worst));
    }

    std::mt19937 rng (31415926);
    std::uniform_real_distribution<double> wild (-1e6, 1e6);
    std::uniform_real_distribution<double> small (-3.0, 3.0);

    for (int i = 0; i < 200000; ++i)
    {
        const double phase = (i % 2 == 0) ? wild (rng) : small (rng);
        const double position = small (rng) * 4.0;
        const double level = small (rng) * (double) Synth::WaveTable::numLevels;

        const float value = table.read (phase, position, level);

        if (! std::isfinite (value))
        {
            fail ("read returned something that is not finite");
            break;
        }
    }

    // An oscillator with no table at all is silent rather than a crash: a
    // patch can reference a table that failed to build.
    Synth::WaveTableOscillator orphan;
    orphan.setSampleRate (48000.0);
    orphan.setFrequency (440.0);

    for (int i = 0; i < 64; ++i)
        if (orphan.nextSample() != 0.0f)
        {
            fail ("an oscillator with no table made a sound");
            break;
        }

    // An empty table is the same.
    Synth::WaveTable empty;
    empty.build ({});

    if (empty.isBuilt())
        fail ("a table built from nothing claims to be built");

    if (empty.read (0.5, 0.0, 0.0) != 0.0f)
        fail ("an empty table made a sound");
}

/** Phase modulation moves the waveform without moving the pitch.

    The distinction the oscillator is built around: the offset is added before
    reading and not accumulated. Adding it to the increment instead would send
    the pitch wandering off with the modulator, which is the classic way to
    build an FM oscillator that will not stay in tune. */
void phaseModulationDoesNotDetune()
{
    std::printf ("phase modulation does not move the pitch\n");

    Synth::WaveTable table;
    table.build ({ sawSpectrum (64) });

    const double rate = 48000.0;
    const double frequency = 220.0;
    const int points = 16384;

    Synth::WaveTableOscillator osc;
    osc.setTable (&table);
    osc.setSampleRate (rate);
    osc.setFrequency (frequency);
    osc.reset();

    std::vector<float> rendered ((std::size_t) points);

    // Driven hard by a modulator at a different frequency.
    double modulatorPhase = 0.0;
    const double modulatorIncrement = 330.0 / rate;

    for (int i = 0; i < points; ++i)
    {
        const double offset = 2.0 * std::sin (2.0 * pi * modulatorPhase);
        rendered[(std::size_t) i] = osc.nextSample (offset);

        modulatorPhase += modulatorIncrement;

        if (modulatorPhase >= 1.0)
            modulatorPhase -= 1.0;
    }

    // The fundamental has to still be at 220, not somewhere near it.
    RealFFT fft (points);
    std::vector<std::complex<double>> bins ((std::size_t) (points / 2 + 1));

    std::vector<float> windowed ((std::size_t) points);

    for (int i = 0; i < points; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * pi * (double) i / (double) (points - 1));
        windowed[(std::size_t) i] = (float) ((double) rendered[(std::size_t) i] * w);
    }

    fft.forward (windowed.data(), bins.data());

    const double binHz = rate / (double) points;

    // Look for the strongest bin anywhere near the fundamental.
    int bestBin = 0;
    double best = 0.0;

    const int from = (int) (150.0 / binHz);
    const int to = (int) (300.0 / binHz);

    for (int k = from; k <= to; ++k)
    {
        const double magnitude = std::abs (bins[(std::size_t) k]);

        if (magnitude > best)
        {
            best = magnitude;
            bestBin = k;
        }
    }

    const double found = (double) bestBin * binHz;

    if (! (std::fabs (found - frequency) < 2.0 * binHz))
        fail ("under heavy modulation the fundamental moved from " + show (frequency)
              + " to " + show (found) + " hz");
}

} // namespace

int main()
{
    std::printf ("Synth::WaveTable\n\n");

    levelsAreWellFormed();
    everyLevelIsSafeForEveryNote();
    aRequestedSpectrumComesBack();
    theDetectorCatchesANaiveSaw();
    nothingAliasesAcrossTheKeyboard();
    morphingDoesNotAlias();
    theTableReadsWhatItHolds();
    theFrameBlendIsExactlyLinear();
    levelChangesAreSmooth();
    positionReachesBothEnds();
    normalisationIsWholeTable();
    theMipmapFitsInABudget();
    phaseModulationDoesNotDetune();
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
