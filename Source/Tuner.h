#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

//==============================================================================
/** A chromatic tuner: what note is being played, and how far off it is.

    Three separate jobs live in here, and they are separate because each one
    fails differently.

    `Tuner::Scale` is the equal tempered arithmetic: a note's frequency, the
    nearest note to a frequency, and the deviation in cents. Pure formulas,
    checkable against closed form.

    `Tuner::Detector` is the pitch estimator, which is YIN. It answers "what is
    the period of this buffer", and it either answers or says that the buffer
    is not periodic enough to have an answer.

    `Tuner::Gate` decides whether a player should be shown a number at all.
    That sounds like polish and is the half that decides whether this agrees
    with the tuning pedal on the floor. See the long comment above it.

    `Tuner::InputRing` is the plumbing: the audio thread writes samples into it
    without allocating or locking, and the analysis reads a snapshot from
    another thread. The estimator costs millions of operations per reading and
    cannot run in the device callback.

    None of this includes JUCE, so all of it is tested by `Tests/TunerTest.cpp`.

    ---------------------------------------------------------------------------
    THE SOURCE

    Alain de Cheveigne and Hideki Kawahara, "YIN, a fundamental frequency
    estimator for speech and music", Journal of the Acoustical Society of
    America 111(4), April 2002, pp. 1917-1930, DOI 10.1121/1.1458024.

    Implemented from the paper's own equations and its own constants, not from
    anybody's code. Equation numbers below are the paper's.

    Why a time domain estimator rather than the tallest peak in a spectrum,
    which is the obvious thing to reach for: a plucked string's fundamental can
    be weak or entirely absent and the pitch is still at the fundamental. Wolfe,
    "Musical sounds and musical signals": "If the fundamental is removed, the
    combined wave still has a period 1/f1, and still has the same pitch." A
    spectral peak picker reads the loudest partial and reports an octave up.
    The difference function dips at the period whether or not there is any
    energy at that frequency. Worse for this application specifically, McLeod's
    thesis records that for complex tones below 200 Hz "the pitch is mainly
    determined by the fourth and fifth harmonics", and every note on a bass and
    the bottom four strings of a guitar sits in that band.

    The paper's reported gross error rates are on speech corpora. The paper
    says plainly that YIN "has been only informally evaluated on music", so
    there is no published figure for a guitar or a bass and none is claimed
    here or in the test.
*/
namespace Tuner
{

//==============================================================================
/** Equal temperament, and the deviation of a frequency from the nearest note.

    The formulas are Joe Wolfe's, School of Physics, University of New South
    Wales, "Note names, MIDI numbers and frequencies":

        f(m)  = 2^((m - 69)/12) * 440 Hz
        m(f)  = 12 * log2(f / 440 Hz) + 69

    cited to that page rather than to the MIDI specification, which is behind a
    membership login and was not read.

    ISO 16, "Acoustics: Standard tuning frequency", specifies A = 440 Hz and was
    last confirmed in 2022. Its full text is paywalled; two catalogue entries
    agree on the value and on the half hertz tolerance it allows, which is
    itself about 2 cents at A4, so the standard is looser than this tuner.

    The reference is adjustable because every shipping tuner's is. There is no
    standard for the *range*, only for the 440, so the range here is the widest
    and finest of the products whose own documentation could be read: 410 to 480
    comes from Korg's CA-50 specification, which is the widest of the pure
    tuners, and the tenth of a hertz step from Logic Pro's Tuner and Peterson's
    StroboStomp HD, which are the finest.
*/
struct Scale
{
    static constexpr double standardReferenceHz = 440.0;
    static constexpr double minReferenceHz      = 410.0;
    static constexpr double maxReferenceHz      = 480.0;
    static constexpr double referenceStepHz     = 0.1;

    // The reference pitch is A4, which is MIDI note 69.
    static constexpr int referenceMidi = 69;

    static constexpr int minMidi = 0;
    static constexpr int maxMidi = 127;

    static double clampReference (double hz) noexcept
    {
        return std::min (maxReferenceHz, std::max (minReferenceHz, hz));
    }

    /** The reference snapped to the step a tuner's control actually moves in,
        so that a stored value and a displayed one cannot disagree in the last
        digit. */
    static double quantiseReference (double hz) noexcept
    {
        const double steps = std::round (clampReference (hz) / referenceStepHz);
        return clampReference (steps * referenceStepHz);
    }

    static double noteHz (int midi, double referenceHz = standardReferenceHz) noexcept
    {
        return referenceHz * std::pow (2.0, (double) (midi - referenceMidi) / 12.0);
    }

    /** Where a frequency falls on the keyboard, as a real number: 69.5 is
        halfway between A4 and A#4. */
    static double fractionalMidi (double hz, double referenceHz = standardReferenceHz) noexcept
    {
        if (! (hz > 0.0) || ! (referenceHz > 0.0))
            return 0.0;

        return (double) referenceMidi + 12.0 * std::log2 (hz / referenceHz);
    }

    struct Note
    {
        int    midi     = 0;        // nearest equal tempered note
        double cents    = 0.0;      // deviation from it, -50 to +50, positive is sharp
        double hz       = 0.0;      // what was measured
        double targetHz = 0.0;      // what the nearest note sits at
        bool   valid    = false;
    };

    /** The deviation is against the **nearest note**, not against the
        reference. Reporting 1200*log2(hz/referenceHz) is a different and much
        larger number, and getting those two confused is the kind of wrong that
        still looks like a tuner. */
    static Note describe (double hz, double referenceHz = standardReferenceHz) noexcept
    {
        Note n;

        if (! (hz > 0.0))
            return n;

        const double ref  = clampReference (referenceHz);
        const double real = fractionalMidi (hz, ref);

        // std::round, so that a frequency exactly halfway between two notes
        // reports as the one above at -50 rather than the one below at +50.
        // std::floor here instead is the classic version of this bug: it reads
        // every note up to 100 cents flat.
        const double nearest = std::round (real);

        n.midi     = (int) std::lround (nearest);
        n.cents    = 100.0 * (real - nearest);
        n.hz       = hz;
        n.targetHz = noteHz (n.midi, ref);
        n.valid    = n.midi >= minMidi && n.midi <= maxMidi;

        return n;
    }

    /** Cents between two frequencies. */
    static double centsBetween (double hz, double againstHz) noexcept
    {
        if (! (hz > 0.0) || ! (againstHz > 0.0))
            return 0.0;

        return 1200.0 * std::log2 (hz / againstHz);
    }

    /** Sharps rather than flats, which is what a chromatic tuner's display
        does, and the octave in scientific pitch notation where A4 is 440: MIDI
        69 is A4, so MIDI 60 is C4 and the octave turns over at C. */
    static const char* noteName (int midi) noexcept
    {
        static const char* const names[12] =
            { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

        if (midi < minMidi || midi > maxMidi)
            return "";

        return names[(size_t) (midi % 12)];
    }

    static int noteOctave (int midi) noexcept
    {
        // Floor division, because MIDI 0 to 11 is octave -1 and integer
        // division towards zero would call it 0.
        return (int) std::floor ((double) midi / 12.0) - 1;
    }
};

//==============================================================================
/** How much of the keyboard the estimator searches, and the window that
    follows from it.

    The lowest note decides everything else. B0 is the low B of a five string
    bass and is also what Korg's own tuners are built around: the GA-50 manual
    gives its bass range as B0 to B5, printing the low end as 23.12 Hz, which
    is B0 taken down the five semitones its flat tuning feature offers
    (30.8677 * 2^(-5/12) = 23.1247, matching Korg's figure).

    The highest is B6, which is where Korg's guitar range stops.
*/
struct Spec
{
    double sampleRate = 44100.0;

    // B0 and B6 at A4 = 440.
    double lowestHz  = 30.867706;
    double highestHz = 1975.533205;

    /** The paper's absolute threshold, from step 4. Also readable, in the
        paper's words, as the proportion of aperiodic power the estimator will
        tolerate, which is what makes it usable as the gate below. */
    double threshold = 0.1;

    /** The integration window, in periods of the lowest note.

        Three sources agree on the floor and they reconcile exactly. The paper:
        "the integration window must be no shorter than the largest expected
        period", and "a larger window leads to fewer errors at the expense of
        reduced temporal resolution". De Cheveigne and Baskind, Eurospeech 2003:
        "time resolution is optimal (one can't go below 2T)". Boersma 1993 asks
        for at least three periods of the minimum pitch per window.

        Two is taken, which puts three periods in the buffer, because the buffer
        the estimator has to hold is W + tauMax rather than W. Temporal
        resolution is free in a tuner: 97 ms at 44.1 kHz is nothing next to the
        time somebody takes to turn a machine head. */
    int windowPeriods = 2;

    /** The paper low passes the signal before analysis, by "convolution with a
        1-kHz square window", and that is kept here. The cutoff is not.

        A square window of length sampleRate/1000 has its first null at 1 kHz,
        and against its own response that puts a note at the top of this
        tuner's range about 28 dB down, which is throwing away the signal the
        tuner is there to read. The length is chosen from the highest note
        being searched instead, as the largest window whose droop at that note
        stays inside `lowPassDroopDb`. That is a departure from the paper and a
        decision of this project's own, so it is written here rather than
        implied.

        The paper's warning in the other direction is respected: the cutoff is
        never set from the *detected* pitch, which would be a feedback path, and
        "setting the cutoff below the F0 would lead to failure".

        What the filtering is for: the paper's own parabolic interpolation is
        accurate "unless the signal contains strong high-frequency components
        (in practice, above about one-quarter of the sampling rate)", which a
        bright pick attack is, and a real string's high partials are stretched
        away from integer multiples by more than a cent above about the tenth
        (computed from the measured inharmonicity of an electric guitar low E,
        B = 1.1e-4, in Fastl and Volk's table). Removing them removes the
        question.

        A square window is symmetric, so it is linear phase and delays every
        frequency by the same (N-1)/2 samples. That is why it is safe here: a
        filter that delayed frequencies differently would move the period it is
        being used to measure.

        WHAT THE FILTER IS AND IS NOT WORTH, MEASURED

        On a perfect harmonic series this estimator reads the period exactly,
        at every partial count from 5 to 30 and at every roll off tried: the
        error is 0.00 cents and the filter changes nothing. So the filter is
        not there for harmonic signals and the error on a real string is not
        the estimator's.

        It is inharmonicity, and how much of it reaches the reading depends
        entirely on how much energy sits in the high partials. Measured on a
        low E at the coefficient Fastl and Volk publish for one, B = 1.1e-4:

        | partials | amplitude | droop 1 dB (N=5) | droop 24 dB (N=20) |
        |---|---|---|---|
        |  5 | 1/h   |  +1.05 cents |  +1.01 cents |
        | 10 | 1/h   |  +3.62 cents |  +3.18 cents |
        | 20 | 1/h   | +11.66 cents |  +6.56 cents |
        |  5 | 1/h^2 |  +0.32 cents |  +0.31 cents |
        | 20 | 1/h^2 |  +0.95 cents |  +0.65 cents |
        | 30 | 1/h^2 |  +0.93 cents |  +0.65 cents |

        Two things follow. Filtering harder monotonically reduces the bias, so
        the filter does the job it is here for. And it cannot remove the bias,
        because the signal genuinely has no single period: on a 1/h spectrum
        reaching the twentieth partial, twenty four decibels of droop still
        leaves six and a half cents.

        One decibel is taken, which keeps the top of the tuner's range usable,
        and the reason that is the right trade rather than a compromise is that
        the bias is under a cent on any spectrum whose partials fall at 1/h^2,
        which is what a plucked string looks like once the pick noise has gone.
        The remaining bias is also not a fault to be corrected towards: Fastl
        and Volk's listening tests found that "strictly harmonic as well as
        slightly inharmonic spectra produce essentially the same pitch height",
        so an estimator weighted towards the partials a listener weights is
        reading the pitch that is actually heard. */
    bool   lowPass       = true;
    double lowPassDroopDb = 1.0;
};

//==============================================================================
/** YIN.

    Holds its own scratch buffers, sized once by `configure`, so `estimate`
    allocates nothing. It is not safe to call from the audio thread anyway,
    because it costs on the order of four million operations per reading, but a
    reading that allocated would also stall whatever thread is running the
    user interface.
*/
class Detector
{
public:
    struct Estimate
    {
        /** True when a dip fell below the threshold, which is the paper's step
            4 succeeding. False means the fallback was used, and a tuner should
            not show the fallback: see the gate. */
        bool   periodic      = false;
        double periodSamples = 0.0;
        double hz            = 0.0;

        /** One minus the depth of the chosen dip in the normalised difference
            function, so 1 is perfectly periodic and 0 is noise. The paper's
            own reading of its threshold as a proportion of aperiodic power is
            what makes this meaningful rather than decorative. */
        double clarity = 0.0;

        /** Root mean square of the analysed samples, before filtering. */
        double rms = 0.0;

        /** The period the "deepest dip wins" rule would have reported.

            Kept because it is the difference between implementing the paper's
            step 4 and implementing the thing everybody writes instead. On a
            signal with a subharmonic the global minimum sits an octave down
            and this says so, which is what lets the test prove the first dip
            rule is doing something rather than merely being present. */
        double globalMinimumPeriodSamples = 0.0;

        bool   valid = false;   // false means the buffer was too short
    };

    void configure (const Spec& spec)
    {
        s = spec;

        s.sampleRate = std::max (8000.0, s.sampleRate);
        s.lowestHz   = std::max (1.0, s.lowestHz);
        s.highestHz  = std::max (s.lowestHz * 2.0, s.highestHz);

        // A quarter of the sample rate is where the paper says the method
        // itself becomes unreliable: "tends ... to fail for F0's beyond one
        // quarter of the sampling rate". Nothing a tuner needs is anywhere
        // near it, but the search range is clamped rather than trusted.
        s.highestHz = std::min (s.highestHz, s.sampleRate * 0.25);

        s.threshold     = std::min (1.0, std::max (1.0e-6, s.threshold));
        s.windowPeriods = std::max (1, s.windowPeriods);

        // The longest lag that is SEARCHED, which is the period of the lowest
        // note, and then two more lags that are only ever COMPUTED.
        //
        // Those two are not slack. A dip is recognised by comparing a lag
        // against both its neighbours, and interpolated from both of them, so
        // the dip belonging to the lowest note in the range needs a lag beyond
        // it to exist. Without them the lowest note is the one note the
        // estimator cannot see as a dip at all: it falls through to the global
        // minimum fallback, and a tuner that does not display its fallback
        // then shows nothing on the open B of a five string bass. Measured
        // before it was fixed, which is the only reason it is written down.
        searchMaxLag  = (int) std::ceil (s.sampleRate / s.lowestHz);
        tauMaxSamples = searchMaxLag + 2;

        // Two for the same reason at the other end: the local minimum test at
        // tauMin reads tau-1, and so does the interpolation.
        tauMinSamples = std::max (2, (int) std::floor (s.sampleRate / s.highestHz));

        if (tauMinSamples > searchMaxLag - 1)
            tauMinSamples = std::max (2, searchMaxLag - 1);

        windowSamples = s.windowPeriods * tauMaxSamples;

        filterLength = s.lowPass ? chooseFilterLength (s.sampleRate, s.highestHz, s.lowPassDroopDb)
                                 : 1;

        // The analysis needs W + tauMax samples, because d(tau) at the longest
        // lag reads x up to index W-1+tauMax. Allocating only W and running
        // the lags out to tauMax is a read of tauMax samples past the end, and
        // this project has already shipped the one sample version of that
        // fault in a delay line. The filter eats another N-1 at the front,
        // because it is run causally rather than being allowed to taper into
        // the edges of the window.
        analysedSamples = windowSamples + tauMaxSamples;
        requiredSamples = analysedSamples + filterLength - 1;

        filtered.assign ((size_t) analysedSamples, 0.0);
        diff.assign ((size_t) tauMaxSamples + 1, 0.0);
        norm.assign ((size_t) tauMaxSamples + 1, 0.0);
    }

    int requiredSampleCount() const noexcept { return requiredSamples; }
    int windowSampleCount()   const noexcept { return windowSamples; }
    int maxLagSamples()       const noexcept { return tauMaxSamples; }
    int searchMaxLagSamples() const noexcept { return searchMaxLag; }
    int minLagSamples()       const noexcept { return tauMinSamples; }
    int lowPassLength()       const noexcept { return filterLength; }
    const Spec& spec()        const noexcept { return s; }

    /** `x` must hold at least `requiredSampleCount()` samples; the most recent
        ones are the ones analysed, so a longer buffer is read from its end. */
    Estimate estimate (const float* x, int n) noexcept
    {
        Estimate e;

        if (x == nullptr || n < requiredSamples || tauMaxSamples <= 2)
            return e;

        const float* in = x + (n - requiredSamples);

        // ---- the low pass, and the level, which is measured before it ----
        double sumSquares = 0.0;

        for (int i = 0; i < analysedSamples; ++i)
        {
            double acc = 0.0;

            for (int k = 0; k < filterLength; ++k)
                acc += (double) in[i + k];

            filtered[(size_t) i] = acc / (double) filterLength;

            // The level is the unfiltered signal's, because a level gate is
            // about whether the player is playing, and the filter's job is to
            // remove content the estimator should not see rather than content
            // the gate should not count.
            const double raw = (double) in[i + filterLength - 1];
            sumSquares += raw * raw;
        }

        e.rms = std::sqrt (sumSquares / (double) analysedSamples);

        // ---- step 2, Eq. (6): the difference function ----
        // Every lag from 1 to tauMax, not just the ones inside the search
        // range, because the cumulative mean in Eq. (8) runs from j = 1. A
        // normalisation taken over the search range alone is a different
        // function and does not behave like the paper's.
        diff[0] = 0.0;   // Eq. (6) at tau = 0 is identically zero

        for (int tau = 1; tau <= tauMaxSamples; ++tau)
        {
            double acc = 0.0;

            for (int j = 0; j < windowSamples; ++j)
            {
                const double d = filtered[(size_t) j] - filtered[(size_t) (j + tau)];
                acc += d * d;
            }

            diff[(size_t) tau] = acc;
        }

        // ---- step 3, Eq. (8): the cumulative mean normalised difference ----
        // "starts at 1 rather than 0", which is the whole point: d(0) is zero
        // and would otherwise be the deepest dip in the function, and a search
        // that fell into it would report an arbitrarily high frequency. This is
        // the step that guards against reading too HIGH. It is not what guards
        // against reading an octave down; that is the threshold below.
        norm[0] = 1.0;
        double running = 0.0;

        for (int tau = 1; tau <= tauMaxSamples; ++tau)
        {
            running += diff[(size_t) tau];

            const double mean = running / (double) tau;

            // A silent buffer makes every difference zero. Reporting clarity on
            // silence would be meaningless, so it is reported as the least
            // periodic thing there is and the level gate deals with it.
            norm[(size_t) tau] = mean > 0.0 ? diff[(size_t) tau] / mean : 1.0;
        }

        // ---- step 4: the absolute threshold, and the FIRST dip ----
        // "choose the smallest value of tau that gives a minimum of d' deeper
        // than that threshold. If none is found, the global minimum is chosen
        // instead."
        //
        // Smallest lag, not deepest dip, and this is the only thing standing
        // between this tuner and reading an octave down. The paper's reason is
        // that "one of the higher-order dips of the difference function is
        // deeper than the period dip", and the higher order dips sit at
        // multiples of the period. Taking the global minimum here is the
        // implementation that passes every obvious test and then reports an
        // octave down on a real guitar.
        int    chosen     = -1;
        double chosenDepth = 0.0;

        for (int tau = tauMinSamples; tau <= searchMaxLag; ++tau)
        {
            if (! (norm[(size_t) tau] < norm[(size_t) (tau - 1)]
                   && norm[(size_t) tau] <= norm[(size_t) (tau + 1)]))
                continue;

            // Step 5, first half: "the ordinate of the interpolated minimum is
            // used in the dip-selection process". So the depth compared against
            // the threshold is the interpolated one, not the sampled one.
            const double depth = interpolatedDepth (norm, tau);

            if (depth < s.threshold)
            {
                chosen      = tau;
                chosenDepth = depth;
                break;
            }
        }

        // The deepest dip, which is both the paper's fallback when nothing
        // clears the threshold and, reported alongside, the evidence that the
        // first dip rule above chose differently.
        int deepest = tauMinSamples;

        for (int tau = tauMinSamples; tau <= searchMaxLag; ++tau)
            if (norm[(size_t) tau] < norm[(size_t) deepest])
                deepest = tau;

        e.globalMinimumPeriodSamples = interpolatedLag (diff, deepest);

        if (chosen < 0)
        {
            // The fallback the paper specifies, so that something is always
            // returned. `periodic` stays false and the gate does not display it.
            chosen      = deepest;
            chosenDepth = norm[(size_t) chosen];
            e.periodic  = false;
        }
        else
        {
            e.periodic = true;
        }

        // ---- step 5, second half: the lag comes from the RAW difference ----
        // "the estimate obtained in this way is slightly biased. To avoid this
        // bias, the abscissa of the corresponding minimum of the raw
        // difference function d(t) is used instead."
        //
        // So the two outputs of the interpolation come from two different
        // functions: the depth from d', just above, and the period from d,
        // here. The paper reports the bias and does not state its cause, so no
        // cause is claimed here.
        //
        // This is not a refinement. One cent is 0.31 of a sample of lag at
        // guitar low E and 0.83 at the bottom of the range, so without
        // sub-sample interpolation the lag quantisation alone is 3.2 and 1.2
        // cents, which is worse than every shipping tuner.
        e.periodSamples = interpolatedLag (diff, chosen);
        e.hz            = e.periodSamples > 0.0 ? s.sampleRate / e.periodSamples : 0.0;
        e.clarity       = std::min (1.0, std::max (0.0, 1.0 - chosenDepth));
        e.valid         = true;

        return e;
    }

    /** The largest square window whose attenuation at `atHz` is still within
        `droopDb`, which is what sets the low pass. Exposed so the test can
        check the response rather than the loop.

        A length N square window averaged to unity gain has the response
        |sin(pi f N / sr)| / (N |sin(pi f / sr)|), which is the Dirichlet
        kernel. N = 1 is no filtering at all. */
    static int chooseFilterLength (double sampleRate, double atHz, double droopDb) noexcept
    {
        if (! (sampleRate > 0.0) || ! (atHz > 0.0) || atHz >= sampleRate * 0.5)
            return 1;

        const double limit = std::pow (10.0, -std::max (0.0, droopDb) / 20.0);

        int best = 1;

        // The first null sits at sr/N, so there is nothing to be had from
        // lengths beyond the point where the null reaches the frequency being
        // protected. Bounded rather than open ended so this cannot run away on
        // an absurd sample rate.
        const int ceiling = std::max (1, (int) std::floor (sampleRate / atHz));

        for (int n = 1; n <= ceiling; ++n)
            if (squareWindowResponse (n, atHz, sampleRate) >= limit)
                best = n;

        return best;
    }

    static double squareWindowResponse (int n, double hz, double sampleRate) noexcept
    {
        if (n <= 1)
            return 1.0;

        const double w = 3.14159265358979323846 * hz / sampleRate;
        const double den = std::sin (w);

        // At DC the kernel is 1 by continuity, and the quotient is 0/0.
        if (std::abs (den) < 1.0e-15)
            return 1.0;

        return std::abs (std::sin (w * (double) n) / ((double) n * den));
    }

private:
    /** Three point parabola through a local minimum, returning the
        interpolated ordinate: the depth at the vertex. */
    static double interpolatedDepth (const std::vector<double>& f, int tau) noexcept
    {
        const double a = f[(size_t) (tau - 1)], b = f[(size_t) tau], c = f[(size_t) (tau + 1)];
        const double den = a - 2.0 * b + c;

        if (std::abs (den) < 1.0e-30)
            return b;

        const double shift = 0.5 * (a - c) / den;

        // The vertex of the parabola through those three points, which is
        // b - (a-c)^2 / (8*den) written in terms of the shift already computed.
        return b - 0.25 * (a - c) * shift;
    }

    /** The same parabola's abscissa: where the minimum actually sits, in
        samples of lag. */
    static double interpolatedLag (const std::vector<double>& f, int tau) noexcept
    {
        const double a = f[(size_t) (tau - 1)], b = f[(size_t) tau], c = f[(size_t) (tau + 1)];
        const double den = a - 2.0 * b + c;

        if (std::abs (den) < 1.0e-30)
            return (double) tau;

        // A true local minimum of a smooth function sampled at whole lags puts
        // the vertex within half a sample of the smallest of the three, and
        // exactly half a sample away when the two lowest samples are equal.
        // That boundary is reached in practice rather than in theory: a period
        // of exactly 1000.5 samples makes d(1000) and d(1001) equal, the
        // vertex lands on +0.5, and rounding decides which side of the
        // comparison it falls on. Rejecting the shift there threw away the
        // whole sub-sample estimate and read 0.87 cents flat, measured.
        //
        // So it is clamped rather than rejected. Outside the half sample band
        // the three points are not bracketing a minimum, and the edge of the
        // band is a better answer than discarding the interpolation.
        const double shift = std::min (0.5, std::max (-0.5, 0.5 * (a - c) / den));

        return (double) tau + shift;
    }

    Spec s;

    int tauMinSamples   = 2;
    int tauMaxSamples   = 0;
    int searchMaxLag    = 0;
    int windowSamples   = 0;
    int analysedSamples = 0;
    int requiredSamples = 0;
    int filterLength    = 1;

    std::vector<double> filtered, diff, norm;
};

//==============================================================================
/** Whether to show the player a number at all.

    This is the half that decides whether this tuner agrees with the tuning
    pedal on the floor, and it is the half with no standard behind it.

    THE PROBLEM, WHICH IS MEASURED AND NOT A MATTER OF TASTE

    A plucked steel string is sharp when it is struck and falls while it rings.
    Hodgkinson, Timoney and Lazzarini, "A Model of Partial Tracks for
    Tension-Modulated Steel-String Guitar Tones", DAFx-10, fit the fundamental
    of an acoustic guitar's open low E, played mezzo forte, to

        f0(t) = (f0_0 - f0_inf) * exp(-t / tau) + f0_inf

    and measured f0_0 = 83.4 Hz, f0_inf = 82.8 Hz, tau = 0.38 s. The mechanism
    is tension modulation: transverse vibration raises the string's tension,
    and so its pitch.

    That is a fall of 12.5 cents (1200*log2(83.4/82.8)), with 0.6 cents of it
    still left a second after the pluck. Boss and Korg both claim plus or minus
    one cent. So the instrument moves twelve times the entire error budget of a
    shipping tuner during the note, and a tuner that displays its first reading
    reads sharp on every note.

    NO MANUFACTURER DOCUMENTS WHAT TO DO ABOUT IT

    The Boss TU-3 and TU-3W manuals, the Korg GA-50 manual and CA-50 and
    Pitchblack X specifications, Peterson's StroboStomp HD manual, Apple's
    Logic Pro Tuner help and Cockos's ReaTune documentation were all read.
    None states a level gate, a hold time, a stability criterion or any
    handling of the attack. The only acknowledgements are indirect: Peterson
    tells the player to pluck with a thumb rather than a plectrum and that
    "it's OK if the strobe image doesn't completely stop"; Korg specifies one
    tuner's range on a sine wave and another's "for guitar and bass sounds";
    Apple says only "play a single note".

    So this is a decision rather than a standard, which `CLAUDE.md` asks to be
    said plainly rather than implied. Each of its three parts is here for a
    reason, and one of them is the paper's rather than ours:

    1. PERIODICITY, which comes free from the paper. Its threshold is readable
       as the proportion of aperiodic power the estimator will tolerate. During
       an attack, and on noise, nothing falls below it and the estimator is in
       its global minimum fallback. That fallback exists so the estimator
       always returns something; a tuner does not have to show it. So "nothing
       below the threshold" is displayed as "no note", never as a number.

    2. LEVEL, because an open string ringing down ends in noise and a tuner
       should stop rather than chase it. Boersma's method has a silence
       threshold for the same job, at 0.05 in his worked example, but that is a
       speech analysis constant and is not presented here as a tuner one.

    3. STABILITY across consecutive frames, which is this tuner's substitute
       for the sixth step of the paper that is not implemented. The paper's own
       step 6 is a time continuity heuristic for a corpus where F0 moves
       continuously through speech; a tuner has the better conditioned version
       of the same idea available, because the note it is looking at is meant
       to be steady.

    WHERE THE STABILITY FIGURE COMES FROM, AND WHAT IT COSTS

    This arithmetic is worth keeping, because it says something no tuner's
    manual does. On the measured glide, the pitch moves between two frames dt
    apart by

        12.5 * exp(-t/0.38) * (1 - exp(-dt/0.38))  cents

    so at 100 ms spacing the movement between adjacent frames is 0.231 of
    whatever is still left to fall. Releasing the display once that movement
    drops below c cents therefore still leaves about c/0.231, call it four and
    a half times c, to come. Getting within one cent of where the string will
    settle means waiting for the movement to fall under about a quarter of a
    cent, which on these constants is roughly nine tenths of a second after the
    pluck.

    That is not a defect to be tuned out. It is how long the string takes, and
    it is about how long a tuning pedal appears to take. A tighter figure here
    does not make the tuner faster, it makes it read sharp.
*/
struct GateSettings
{
    /** Below this the tuner says nothing. In dBFS, so it is absolute: a guitar
        through an interface at a sensible input gain sits far above it, and a
        note ringing into the noise floor falls under it. Chosen rather than
        sourced. */
    double levelFloorDb = -55.0;

    /** How many consecutive readings must agree, and how closely, before a
        number is shown. Four frames within a quarter of a cent, for the reason
        in the comment above. */
    int    stabilityFrames = 4;
    double stabilityCents  = 0.25;
};

/** The estimator, the gate and the reference pitch together: what a readout
    actually talks to.

    Not thread safe, and not meant to be: one of these belongs to whatever
    runs the display, and it reads samples out of an `InputRing` that the audio
    thread fills.
*/
class Analyser
{
public:
    struct Reading
    {
        bool   hasNote  = false;    // false means show dashes, not a number
        int    midi     = 0;
        double cents    = 0.0;
        double hz       = 0.0;
        double targetHz = 0.0;
        double clarity  = 0.0;
        double levelDb  = -200.0;

        /** True when the estimator found a periodic buffer but the pitch is
            still moving, which is the first part of a plucked note. Worth
            distinguishing from silence so a display can say "listening"
            rather than nothing at all. */
        bool   settling = false;
    };

    void configure (const Spec& spec, const GateSettings& gate = {})
    {
        detector.configure (spec);
        g = gate;
        g.stabilityFrames = std::max (1, g.stabilityFrames);
        g.stabilityCents  = std::max (0.0, g.stabilityCents);
        reset();
    }

    void setReferenceHz (double hz) noexcept
    {
        const double next = Scale::quantiseReference (hz);

        // The history is in note space, so moving the reference would make
        // every stored frame disagree with the new one and stall the display
        // for a few frames for no reason. Cents are reference independent, so
        // the history is kept in cents of absolute pitch instead and this is
        // only here to document that the choice was deliberate.
        referenceHz = next;
    }

    double getReferenceHz() const noexcept { return referenceHz; }

    void reset() noexcept
    {
        history.clear();
        last = {};
    }

    int requiredSampleCount() const noexcept { return detector.requiredSampleCount(); }
    const Detector& getDetector() const noexcept { return detector; }

    Reading process (const float* x, int n) noexcept
    {
        const auto e = detector.estimate (x, n);

        Reading r;

        if (! e.valid)
        {
            // Too few samples to say anything. Not the same as silence, and
            // not a reason to throw away a history that was building.
            return last;
        }

        r.levelDb = e.rms > 0.0 ? 20.0 * std::log10 (e.rms) : -200.0;
        r.clarity = e.clarity;

        const bool loudEnough = r.levelDb >= g.levelFloorDb;

        if (! (e.periodic && loudEnough && e.hz > 0.0))
        {
            // Either silence, or noise, or an attack that has not resolved into
            // a pitch yet. The history is cleared rather than kept, because the
            // next note to arrive is a different note and comparing its first
            // frame against the last note's would release the display
            // immediately on a pitch that has not settled.
            history.clear();
            last = r;
            return r;
        }

        // Kept in absolute cents rather than as a frequency, so that the
        // stability test is a test in the units it is specified in: a quarter
        // of a cent means the same thing at the bottom of a bass as at the top
        // of a guitar, where a quarter of a hertz does not.
        const double absoluteCents = 1200.0 * std::log2 (e.hz);

        history.push_back (absoluteCents);

        if ((int) history.size() > g.stabilityFrames)
            history.erase (history.begin());

        r.settling = true;

        if ((int) history.size() >= g.stabilityFrames)
        {
            double sum = 0.0;

            for (auto c : history)
                sum += c;

            const double mean = sum / (double) history.size();
            bool steady = true;

            for (auto c : history)
                if (std::abs (c - mean) > g.stabilityCents)
                    steady = false;

            if (steady)
            {
                // The newest frame rather than the mean. The gate has already
                // established that all of them agree to within a quarter of a
                // cent, so the mean buys nothing, and on a pitch that is still
                // creeping the mean is the stalest of the readings rather than
                // the closest to where the string is going.
                const auto note = Scale::describe (e.hz, referenceHz);

                r.hasNote  = note.valid;
                r.midi     = note.midi;
                r.cents    = note.cents;
                r.hz       = note.hz;
                r.targetHz = note.targetHz;
                r.settling = ! note.valid;
            }
        }

        last = r;
        return r;
    }

private:
    Detector     detector;
    GateSettings g;
    double       referenceHz = Scale::standardReferenceHz;

    std::vector<double> history;
    Reading             last;
};

//==============================================================================
/** Samples from the audio thread to whatever analyses them.

    The writer is the device callback, so it allocates nothing and locks
    nothing: it copies into a preallocated array and publishes one counter.
    The reader is the thread running the display, and it takes a snapshot of
    the most recent samples.

    There is no lock, so a reader can in principle be overtaken by the writer
    partway through its copy. That is detected rather than ignored: the write
    counter is read before and after, and a snapshot the writer has run over is
    reported as a failure instead of being handed back as audio. A torn
    snapshot is a discontinuity in the middle of the buffer, which an
    estimator reads as a perfectly plausible and completely wrong pitch, so
    this cannot be left to chance.

    Make the ring several times the snapshot length and it effectively never
    happens: at 44.1 kHz a 97 ms snapshot out of a 400 ms ring gives the
    reader three tenths of a second to copy four thousand samples.
*/
class InputRing
{
public:
    /** Message thread. Allocates, so never call this while the callback is
        running without the usual handover. */
    void setSize (int samples)
    {
        buffer.assign ((size_t) std::max (1, samples), 0.0f);
        written.store (0, std::memory_order_release);
    }

    int size() const noexcept { return (int) buffer.size(); }

    /** Audio thread. No allocation, no lock, no logging. */
    void write (const float* x, int n) noexcept
    {
        if (buffer.empty() || x == nullptr || n <= 0)
            return;

        const int    cap = (int) buffer.size();
        const std::uint64_t start = written.load (std::memory_order_relaxed);

        // More samples at once than the ring holds means only the last `cap`
        // can survive, and reading the earlier ones would be wasted work.
        if (n > cap)
        {
            x += (n - cap);
            n = cap;
        }

        int pos = (int) (start % (std::uint64_t) cap);

        for (int i = 0; i < n; ++i)
        {
            buffer[(size_t) pos] = x[i];

            if (++pos >= cap)
                pos = 0;
        }

        // Release, so that a reader which sees this counter also sees every
        // sample written before it.
        written.store (start + (std::uint64_t) n, std::memory_order_release);
    }

    /** Message thread. Fills `dest` with the most recent `dest.size()`
        samples, newest last.

        Returns false when there is not that much audio yet, or when the writer
        overtook the copy. Either way `dest` must not be used. */
    bool readLatest (std::vector<float>& dest) const noexcept
    {
        const int cap  = (int) buffer.size();
        const int want = (int) dest.size();

        if (cap <= 0 || want <= 0 || want > cap)
            return false;

        const std::uint64_t before = written.load (std::memory_order_acquire);

        if (before < (std::uint64_t) want)
            return false;

        const std::uint64_t first = before - (std::uint64_t) want;
        int pos = (int) (first % (std::uint64_t) cap);

        for (int i = 0; i < want; ++i)
        {
            dest[(size_t) i] = buffer[(size_t) pos];

            if (++pos >= cap)
                pos = 0;
        }

        const std::uint64_t after = written.load (std::memory_order_acquire);

        // The oldest sample copied is still intact only if the writer has not
        // advanced far enough to have wrapped onto it. Comparing against the
        // ring's capacity rather than against `before` is what makes this a
        // tear check and not a "did anything arrive" check: audio arriving
        // during the copy is normal and harmless.
        return after - first <= (std::uint64_t) cap;
    }

    void clear() noexcept
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        written.store (0, std::memory_order_release);
    }

    /** How many samples have ever been written, which is what tells a display
        whether the tuner has been fed at all. */
    std::uint64_t totalWritten() const noexcept
    {
        return written.load (std::memory_order_acquire);
    }

private:
    std::vector<float>         buffer;
    std::atomic<std::uint64_t> written { 0 };
};

} // namespace Tuner
