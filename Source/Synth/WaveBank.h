#pragma once
#include "WaveTable.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

//==============================================================================
/** The wavetables the instrument ships with.

    A wavetable oscillator with nothing to play is a filing cabinet. This is
    the content: a set of tables, each a row of waveforms to sweep through,
    built when the instrument starts rather than loaded from files, so a fresh
    install has everything and there is nothing to download and nothing to
    scan for.

    **Every table is generated from a definition, not sampled from somewhere.**
    That is worth more than it sounds. A sampled wavetable is a recording of
    one particular instance of a waveform at one particular pitch, and its
    harmonic content is whatever the recording happened to have, including the
    aliasing of whatever made it. A generated one is exact: the harmonics are
    computed, so the mipmap can be band limited properly and the table is the
    same at every sample rate.

    Most of these are built by writing the waveform down as a function of
    phase and taking its spectrum. That is the general method and it covers
    anything with a shape: hard sync, wavefolding, bit reduction. The analysis
    uses sixteen thousand points per cycle, which is far more than the four
    hundred odd harmonics a table keeps, so the folding in the analysis itself
    lands eighty decibels below anything retained. `WaveBankTest` measures that
    rather than assuming it.

    A few are written as harmonics directly, where the series is exactly known
    and computing it is both simpler and more accurate than sampling a shape
    and transforming it back.

    **What is deliberately missing.** A vowel table and a bell table both want
    numbers that are measured facts about the world: formant frequencies, and
    the partial ratios of a tuned bell. CLAUDE.md says not to implement from
    memory anything with an established definition, and those two have one. So
    they are not here yet rather than here with plausible numbers in them. A
    wavetable called Vowel that does not match what a vowel is would be the
    exact failure that section of CLAUDE.md is about: present, plausible, and
    wrong, and wrong in a way that only somebody who knows what a formant
    sounds like would ever report.

    No JUCE.
*/
namespace Synth
{

//==============================================================================
namespace WaveShapes
{
    inline constexpr double pi = 3.14159265358979323846;

    /** How finely a shape is sampled before its spectrum is taken.

        Sixteen thousand points per cycle gives harmonics up to 8192, and a
        table keeps at most 431 of them. Anything above 8192 folds down in the
        analysis, and for a waveform falling off at one over h the energy
        landing on a harmonic that is kept comes from harmonic 15953 upward,
        which is 84 dB down. Measured in the test rather than assumed.
    */
    inline constexpr int analysisPoints = 16384;

    /** The spectrum of any waveform that can be written as a function of
        phase.

        The general method. Everything with a shape rather than a known series
        goes through here: hard sync, folding, quantising, anything at all.
    */
    template <typename Shape>
    Spectrum spectrumOf (Shape&& shape, int harmonics)
    {
        RealFFT fft (analysisPoints);

        std::vector<float> cycle ((std::size_t) analysisPoints);

        for (int i = 0; i < analysisPoints; ++i)
            cycle[(std::size_t) i] = (float) shape ((double) i / (double) analysisPoints);

        std::vector<std::complex<double>> bins ((std::size_t) (analysisPoints / 2 + 1));
        fft.forward (cycle.data(), bins.data());

        Spectrum out;

        for (int h = 1; h <= harmonics && h < (int) bins.size(); ++h)
        {
            // The forward transform is unscaled, so a cosine of amplitude A
            // lands as a bin of A times the length over two. Undoing that
            // here keeps amplitudes meaning the same thing whether a spectrum
            // came from a shape or was written down directly.
            const auto bin = bins[(std::size_t) h] * (2.0 / (double) analysisPoints);

            out.amplitude.push_back ((float) std::abs (bin));
            out.phase.push_back ((float) std::arg (bin));
        }

        return out;
    }

    //==========================================================================
    // The waveforms whose series is exactly known. Written as harmonics
    // because the series is simpler and more accurate than sampling a shape
    // and transforming it back, and because a discontinuity written as a
    // shape has to be sampled very finely to come back clean.

    /** Every harmonic at one over its number, which is a sawtooth. The
        quarter turn puts the edge at the start of the cycle. */
    inline Spectrum saw (int harmonics)
    {
        Spectrum s;

        for (int h = 1; h <= harmonics; ++h)
        {
            s.amplitude.push_back ((float) (1.0 / (double) h));
            s.phase.push_back ((float) (-pi * 0.5));
        }

        return s;
    }

    /** A pulse of the given width.

        A pulse that is high for a fraction d of the cycle has harmonic h at

            2 sin(pi h d) / (pi h)

        with that harmonic turned by -pi h d. At d of a half this is the odd
        harmonics at one over h and nothing else, which is a square, so a
        square is this at 0.5 rather than a separate case.

        Both halves of that matter and the first version had both wrong.

        The amplitude was sin(pi h d) over pi h d, which is the same shape
        across the harmonics for a fixed width and so looks right, but carries
        a factor of one over 2d that depends on the width. A table sweeping
        from a square to an eight percent pulse came out with the narrow end
        six times louder, and since a table is normalised as a whole, that
        made the wide end six times quieter. Sweeping the width was a fade.

        The turn was a quarter back for every harmonic, which is what makes a
        sawtooth and is not what makes a pulse: the correct turn grows with
        both the harmonic and the width. At d of a half it is right for
        harmonic 1 and wrong for 3 and 7, so the "square" came out as a
        waveform with a square's amplitudes and some of its harmonics
        inverted, which is a different shape and a different sound. It still
        looked like a plausible waveform, which is why it needed a test
        against the series rather than a look.
    */
    inline Spectrum pulse (int harmonics, double duty)
    {
        if (duty < 0.001) duty = 0.001;
        if (duty > 0.999) duty = 0.999;

        Spectrum s;

        for (int h = 1; h <= harmonics; ++h)
        {
            const double x = pi * (double) h * duty;
            const double sine = std::sin (x);

            s.amplitude.push_back ((float) (2.0 * std::fabs (sine) / (pi * (double) h)));

            // The sign is carried as half a turn on top of the pulse's own.
            s.phase.push_back ((float) (-x + (sine < 0.0 ? pi : 0.0)));
        }

        return s;
    }

    /** Odd harmonics at one over h squared, alternating, which is a
        triangle. */
    inline Spectrum triangle (int harmonics)
    {
        Spectrum s;

        for (int h = 1; h <= harmonics; ++h)
        {
            if (h % 2 == 0)
            {
                s.amplitude.push_back (0.0f);
                s.phase.push_back (0.0f);
                continue;
            }

            s.amplitude.push_back ((float) (1.0 / ((double) h * (double) h)));
            s.phase.push_back ((float) (((h / 2) % 2 == 0 ? 0.0 : pi) - pi * 0.5));
        }

        return s;
    }

    //==========================================================================
    // Blending two spectra, which is how a table morphs between two known
    // waveforms rather than between two arbitrary ones.

    inline Spectrum blend (const Spectrum& a, const Spectrum& b, double amount)
    {
        if (amount < 0.0) amount = 0.0;
        if (amount > 1.0) amount = 1.0;

        const std::size_t n = a.amplitude.size() > b.amplitude.size()
                            ? a.amplitude.size() : b.amplitude.size();

        Spectrum out;

        for (std::size_t i = 0; i < n; ++i)
        {
            const double amplitudeA = i < a.amplitude.size() ? (double) a.amplitude[i] : 0.0;
            const double amplitudeB = i < b.amplitude.size() ? (double) b.amplitude[i] : 0.0;

            const double phaseA = i < a.phase.size() ? (double) a.phase[i] : 0.0;
            const double phaseB = i < b.phase.size() ? (double) b.phase[i] : 0.0;

            // Blended as the real and imaginary parts rather than as
            // amplitude and angle. Interpolating an angle goes the wrong way
            // round whenever two phases straddle the turn, and the harmonic
            // sweeps through the whole waveform on its way there, which comes
            // out as a lurch part way along the position control.
            const double realPart = amplitudeA * std::cos (phaseA) * (1.0 - amount)
                                  + amplitudeB * std::cos (phaseB) * amount;

            const double imaginaryPart = amplitudeA * std::sin (phaseA) * (1.0 - amount)
                                       + amplitudeB * std::sin (phaseB) * amount;

            out.amplitude.push_back ((float) std::hypot (realPart, imaginaryPart));
            out.phase.push_back ((float) std::atan2 (imaginaryPart, realPart));
        }

        return out;
    }
}

//==============================================================================
/** Everything the instrument ships with, as a catalogue. */
class WaveBank
{
public:
    struct Entry
    {
        const char* id;         /**< stable, saved in projects, never reused */
        const char* name;
        const char* category;
        int         frames;
    };

    static const std::vector<Entry>& catalogue()
    {
        static const std::vector<Entry> all
        {
            { "analogue",  "Analogue",  "Classic", 16 },
            { "pulse",     "Pulse",     "Classic", 16 },
            { "harmonics", "Harmonics", "Harmonic", 16 },
            { "odd",       "Odd",       "Harmonic", 16 },
            { "comb",      "Comb",      "Harmonic", 16 },
            { "phase",     "Phase",     "Digital", 16 },
            { "sync",      "Sync",      "Digital", 16 },
            { "bits",      "Bits",      "Digital", 16 },
            { "fold",      "Fold",      "Digital", 16 },
            { "cluster",   "Cluster",   "Dense",   16 },
        };

        return all;
    }

    static int count() { return (int) catalogue().size(); }

    /** The index for an id, or -1.

        The same contract as the parameter tables and the modulation matrix:
        a project stores the id, so the order here can change and reordering
        a menu cannot silently repoint every saved patch at another waveform.
    */
    static int indexFromId (const char* id) noexcept
    {
        if (id == nullptr)
            return -1;

        const auto& all = catalogue();

        for (int i = 0; i < (int) all.size(); ++i)
            if (std::strcmp (all[(std::size_t) i].id, id) == 0)
                return i;

        return -1;
    }

    static const char* idAt (int index) noexcept
    {
        return index >= 0 && index < count() ? catalogue()[(std::size_t) index].id : "";
    }

    //==========================================================================
    /** The recipe for one table, as one spectrum per frame.

        Each of these is a definition rather than a recording, and each says
        what it is. Where a shape was invented here rather than taken from
        somewhere established, the comment says so plainly instead of implying
        an authority that is not there.
    */
    static std::vector<Spectrum> framesFor (int index)
    {
        const int harmonics = WaveTable::mostHarmonics;

        std::vector<Spectrum> frames;

        if (index < 0 || index >= count())
            return frames;

        const int wanted = catalogue()[(std::size_t) index].frames;
        const char* id = catalogue()[(std::size_t) index].id;

        using namespace WaveShapes;

        for (int f = 0; f < wanted; ++f)
        {
            const double t = wanted > 1 ? (double) f / (double) (wanted - 1) : 0.0;

            if (std::strcmp (id, "analogue") == 0)
            {
                // The four shapes a subtractive synth is built on, in the
                // order that makes the sweep go from hollow to bright to
                // thin: triangle, saw, square, narrow pulse.
                const auto tri = triangle (harmonics);
                const auto sawtooth = saw (harmonics);
                const auto square = pulse (harmonics, 0.5);
                const auto narrow = pulse (harmonics, 0.08);

                if (t < 1.0 / 3.0)
                    frames.push_back (blend (tri, sawtooth, t * 3.0));
                else if (t < 2.0 / 3.0)
                    frames.push_back (blend (sawtooth, square, (t - 1.0 / 3.0) * 3.0));
                else
                    frames.push_back (blend (square, narrow, (t - 2.0 / 3.0) * 3.0));
            }
            else if (std::strcmp (id, "pulse") == 0)
            {
                // Half down to a sliver. Geometric rather than linear,
                // because the audible change from 50 to 40 percent is about
                // the same as from 10 to 8, and a linear sweep spends most of
                // the control doing almost nothing.
                frames.push_back (pulse (harmonics, 0.5 * std::pow (0.06, t)));
            }
            else if (std::strcmp (id, "harmonics") == 0)
            {
                // Climbs the harmonic series: the first frame is a sine, and
                // each one after it moves the energy up to the next harmonic
                // with its neighbours fading in and out around it. Sweeping
                // the position is a formant moving up through the spectrum.
                Spectrum s;

                const double centre = 1.0 + t * 23.0;

                for (int h = 1; h <= harmonics; ++h)
                {
                    // Not divided by the harmonic number. Doing that is the
                    // natural rolloff of an ordinary waveform and is wrong
                    // here, because the band is the whole sound rather than
                    // part of one: it made the frame sitting on harmonic 24
                    // twenty seven decibels quieter than the frame sitting on
                    // harmonic 1, so sweeping the table faded it out.
                    const double distance = std::fabs ((double) h - centre);
                    const double level = distance >= 3.0 ? 0.0 : 1.0 - distance / 3.0;

                    s.amplitude.push_back ((float) level);
                    s.phase.push_back ((float) (-pi * 0.5));
                }

                frames.push_back (s);
            }
            else if (std::strcmp (id, "odd") == 0)
            {
                // Odd harmonics only, with the series getting longer along
                // the row. A hollow, clarinet shaped family at one end and a
                // hard square at the other.
                Spectrum s;

                // One to thirty two. The first version ran to five hundred,
                // where the rolloff is already past every harmonic loud
                // enough to hear, so the last third of the control did
                // nothing at all.
                const double reach = std::pow (2.0, t * 5.0);

                for (int h = 1; h <= harmonics; ++h)
                {
                    if (h % 2 == 0)
                    {
                        s.amplitude.push_back (0.0f);
                        s.phase.push_back (0.0f);
                        continue;
                    }

                    const double rolloff = 1.0 / (1.0 + std::pow ((double) h / reach, 4.0));

                    s.amplitude.push_back ((float) (rolloff / (double) h));
                    s.phase.push_back ((float) (-pi * 0.5));
                }

                frames.push_back (s);
            }
            else if (std::strcmp (id, "comb") == 0)
            {
                // A sawtooth with a comb through it, the teeth moving apart
                // along the row. Not an established waveform: it is a shape
                // chosen here because sweeping it sounds like a phaser frozen
                // into the waveform itself, which a filter cannot do.
                Spectrum s;

                // From two upward, never one. At one the notch lands on every
                // harmonic at once and the whole frame is silent, which is a
                // table whose position control starts at no sound at all.
                const double teeth = 2.0 + t * 11.0;

                for (int h = 1; h <= harmonics; ++h)
                {
                    const double notch = std::fabs (std::sin (pi * (double) h / teeth));

                    s.amplitude.push_back ((float) (notch / (double) h));
                    s.phase.push_back ((float) (-pi * 0.5));
                }

                frames.push_back (s);
            }
            else if (std::strcmp (id, "phase") == 0)
            {
                // A sine modulating its own phase, with the index climbing
                // along the row. This is phase modulation written as a
                // waveform: the spectrum is the Jacobi-Anger expansion, which
                // is where the metallic, bell adjacent families come from.
                // Taken as a shape rather than as Bessel functions because
                // the shape is unambiguous and the test can check the two
                // agree at the points where the expansion is simple.
                const double index = t * 8.0;

                frames.push_back (spectrumOf ([index] (double phase)
                {
                    return std::sin (2.0 * pi * phase + index * std::sin (2.0 * pi * phase));
                }, harmonics));
            }
            else if (std::strcmp (id, "sync") == 0)
            {
                // Hard sync: a sawtooth running faster than the note, reset
                // to the start of its cycle every time the note's own cycle
                // comes round. The ratio climbs along the row, which is the
                // sync sweep every aggressive lead is made of.
                const double ratio = 1.0 + t * 7.0;

                frames.push_back (spectrumOf ([ratio] (double phase)
                {
                    const double inner = phase * ratio;
                    return 2.0 * (inner - std::floor (inner)) - 1.0;
                }, harmonics));
            }
            else if (std::strcmp (id, "bits") == 0)
            {
                // A sine quantised to fewer and fewer steps along the row.
                // Quantising is exact, so this is a definition rather than an
                // approximation of one: the last frames are the handful of
                // levels a very early sampler had.
                // Sixteen levels down to two. The first version started at
                // sixty four, and the difference between quantising a sine to
                // sixty four levels and to fifty one is nothing anybody can
                // hear, so the first third of the control was dead.
                const double steps = std::pow (2.0, 1.0 + (1.0 - t) * 3.0);

                frames.push_back (spectrumOf ([steps] (double phase)
                {
                    const double sine = std::sin (2.0 * pi * phase);
                    return std::floor (sine * steps + 0.5) / steps;
                }, harmonics));
            }
            else if (std::strcmp (id, "fold") == 0)
            {
                // A sine driven into a wavefolder: anything past the limit
                // comes back the other way rather than flattening. The drive
                // climbs along the row. Folding is a triangle wave applied to
                // the signal, which is the standard way to write it and is
                // exact rather than a model of a particular circuit.
                const double drive = 1.0 + t * 7.0;

                frames.push_back (spectrumOf ([drive] (double phase)
                {
                    const double x = drive * std::sin (2.0 * pi * phase);

                    // Triangle fold: period four, rising from -1 at -1.
                    const double wrapped = x - 4.0 * std::floor ((x + 2.0) / 4.0) - 2.0;
                    return wrapped < 0.0 ? -2.0 - wrapped : 2.0 - wrapped;
                }, harmonics));
            }
            else if (std::strcmp (id, "cluster") == 0)
            {
                // Dense and deliberately not a tidy series: a handful of
                // partials well up the spectrum, moving apart along the row,
                // over a quiet fundamental. Invented here rather than taken
                // from anywhere, for drones and for the kind of sustained
                // metallic texture a filter cannot produce from a sawtooth.
                Spectrum s;

                const double spread = 1.0 + t * 3.0;

                for (int h = 1; h <= harmonics; ++h)
                {
                    double level = h == 1 ? 0.35 : 0.0;

                    for (int partial = 1; partial <= 6; ++partial)
                    {
                        const double centre = 2.0 + (double) (partial * partial) * spread;
                        const double distance = std::fabs ((double) h - centre);

                        if (distance < 1.0)
                            level += (1.0 - distance) / (double) partial;
                    }

                    s.amplitude.push_back ((float) level);
                    s.phase.push_back ((float) (-pi * 0.5));
                }

                frames.push_back (s);
            }
        }

        return frames;
    }

    //==========================================================================
    /** Builds every table. Allocates and takes a few hundred transforms, so
        it belongs at startup and not on the audio thread. */
    void buildAll()
    {
        tables.clear();
        tables.resize ((std::size_t) count());

        for (int i = 0; i < count(); ++i)
            tables[(std::size_t) i].build (framesFor (i));
    }

    /** Null for an index that does not exist, which a patch referring to a
        table from a later version will hand over. */
    const WaveTable* table (int index) const noexcept
    {
        if (index < 0 || index >= (int) tables.size())
            return nullptr;

        const auto& found = tables[(std::size_t) index];

        return found.isBuilt() ? &found : nullptr;
    }

    std::size_t memoryBytes() const noexcept
    {
        std::size_t total = 0;

        for (const auto& t : tables)
            total += t.memoryBytes();

        return total;
    }

private:
    std::vector<WaveTable> tables;
};

} // namespace Synth
