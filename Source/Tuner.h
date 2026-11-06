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
    standard for the *range*, only for the 440, so this one is taken from what
    the products' own documentation gives: 410 to 480 is Korg's CA-50
    specification, and the tenth of a hertz step is Logic Pro's Tuner and
    Peterson's StroboStomp HD, which are the finest of them.

    It is not the widest. Peterson's own manual gives the StroboStomp HD as 390
    to 490, wider on both sides, and that is left out deliberately rather than
    overlooked: 410 to 480 spans every historical and regional tuning anybody
    plays at, and a range that reaches 390 makes a mistyped reference land
    further from 440 before the clamp catches it. Written down because the
    research record tabulates the wider figure and a later reader would
    otherwise take this for an oversight.
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
        halfway between A4 and A#4.

        Returns 0, meaning C-1, for a frequency at or below zero, which is a
        plausible looking note rather than an obviously wrong one. Callers go
        through `describe` and check its `valid`, which guards this; it is said
        here because this is public and a plausible wrong note is the shape of
        fault this project keeps meeting. */
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
        // Floor division rather than C++'s truncating division. Over MIDI 0
        // to 127 the two agree, so this is not the guard it looks like: they
        // differ only for a NEGATIVE note number, where truncation would call
        // MIDI -1 octave -1 and the octave below C-1 is -2. `describe` can
        // return a negative note with `valid` false, so a display that ignored
        // `valid` would see the difference.
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

    // The range SEARCHED, which is deliberately wider at the bottom than the
    // range of notes a player is tuning.
    //
    // A tuner whose search stopped at the lowest note it covers could not tell
    // anybody that note was flat, which is the one thing they are looking at
    // it to find out. Measured: with the search floor at B0 exactly, a B0 that
    // is ONE CENT flat already cannot be read, because its period is already
    // longer than the longest lag computed. The reading saturates at the floor
    // and says nearly in tune while the string is out; at 49 cents flat it
    // reads 48 cents sharp of the truth. So this is not headroom against an
    // extreme case: without it the tuner cannot read the lowest note it covers
    // from the moment that note needs tuning, which is the only moment anybody
    // looks.
    //
    // So the floor is five semitones below B0, which is 23.1247 Hz. That is
    // not a round number chosen for headroom: it is Korg's own design target,
    // printed on the GA-1's specification page and in the GA-50 manual as
    // 23.12 Hz, being B0 with the five semitones of flat tuning those
    // instruments offer (30.8677 * 2^(-5/12) = 23.1247). Taking the same floor
    // means a five string bass tuned down to drop A still reads.
    //
    // The ceiling is C8, which is the top of the detection range Boss print
    // for the TU-3 and Korg for the CA-50, and the top note of a piano.
    //
    // It is deliberately above the range this tuner claims to be ACCURATE
    // over, which is `accurateToHz` below, and the reason is a failure rather
    // than a preference. Nothing above the ceiling has a period inside the
    // lags searched, so the first dip that IS inside the range is the one at
    // twice the period, and the estimator reports an exact octave down,
    // steadily, with the threshold satisfied and nothing to say it is wrong.
    // Measured with the ceiling at B6: a 2.5 kHz tone read 1250 Hz and a
    // 3.5 kHz tone read 1750 Hz, both as confident readings a player would
    // believe. A pinch harmonic, a whistle or a squeal does that.
    //
    // Putting the ceiling at C8 does not make that impossible, it moves it
    // above the top note of a piano and above the top of every shipping
    // tuner's published range, which is as far as it can honestly be moved:
    // the first dip rule that protects the bottom of the range is the same
    // rule that exposes the top of it, and no ceiling removes that.
    double lowestHz  = 23.124651;
    double highestHz = 4186.009045;

    /** The top of the range this tuner claims the one cent tier over, and the
        frequency the low pass is designed to pass.

        B6, where Korg's guitar range stops. Two numbers rather than one
        because they answer two different questions, and tying them together
        costs something either way: designing the filter at the ceiling leaves
        almost no filtering, and measured, that is the difference between
        inside the one cent tier and outside it on a signal with strong high
        partials.

        Measured accuracy with the shipped settings, on pure tones: 0.09 cents
        at B6, 0.18 at 2.5 kHz, 0.43 at 3.5 kHz and 0.32 at C8. Put six
        harmonics on the note and the reading degrades much faster above B6,
        reaching 2.9 cents in the top octave, because the partials of a note
        that high carry real energy where a three point parabola has few
        samples of curvature to work with. So the tier is claimed to B6, and
        the span above it is searched so that a note there is named correctly
        rather than an octave out, not so that it is named to within a cent.

        Written to more figures than it looks to need, because B6 is
        1975.5332050245 and a literal rounded to 1975.533205 is BELOW the note
        it names. Every sweep that stops at this frequency then silently drops
        B6, which is how a breakage that made the highest note in the range
        read an octave down went unnoticed through a whole test run.

        ONE MEASURED LIMIT AT THIS END, recorded rather than left to be found.
        A note whose fundamental is missing entirely reads an octave UP in the
        top semitone or so of this span, because the lag at half its period has
        come inside the searched range. Over sixty random amplitude and phase
        draws per case, harmonics two upward with no fundamental read correctly
        every time at 200, 400, 700, 1000, 1300, 1600 and 1800 Hz, once in
        sixty at 1900, and five to ten times in sixty at B6 itself. It is the
        same first dip rule seen from the other side, so it is structural, and
        it is a signal no instrument makes: a note at 1900 Hz with no energy at
        1900 Hz. The case it models, where the fundamental really is weak or
        absent, is a bass or the bottom strings of a guitar, two or three
        octaves below any of this. */
    double accurateToHz = 1975.5332050245;

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
        resolution is free in a tuner: the 130 ms that comes to at 44.1 kHz is
        nothing next to the time somebody takes to turn a machine head.

        What it costs is not free, and is measured rather than assumed: one
        reading over that window is about nine milliseconds of work in a
        release build, which is why this runs on whatever thread drives the
        display and not in the device callback, and why ten readings a second
        is plenty. Raising the search floor shortens the window quadratically:
        a floor of 160 Hz costs 0.22 ms. */
    int windowPeriods = 2;

    /** The paper low passes the signal before analysis, by "convolution with a
        1-kHz square window", and that is kept here. The cutoff is not.

        A square window of length sampleRate/1000 has its first null at 1 kHz,
        and against its own response that puts a note at the top of this
        tuner's range about 28 dB down, which is throwing away the signal the
        tuner is there to read. The length is chosen from `accurateToHz`
        instead, as the largest window whose droop at that frequency stays
        inside `lowPassDroopDb`. That is a departure from the paper and a
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
    allocates nothing. It is not safe to call from the audio thread anyway: Eq.
    (6) runs the window against every lag, which at 44.1 kHz is seven million
    iterations of a subtract and a multiply-add, about nine milliseconds. The
    count is proportional to the square of the sample rate, because both the
    window and the longest lag are a fixed duration, so at 192 kHz it is 138
    million iterations and two tenths of a second. Whatever drives the display
    has to budget for that; `TunerPanel.h` has the measured table and what it
    does about it.
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

        // The filter is designed at the frequency accuracy is claimed to, and
        // that has to stay inside the searched range or it is designed for a
        // note the estimator cannot read anyway.
        s.accurateToHz = std::min (std::max (s.lowestHz, s.accurateToHz), s.highestHz);

        s.threshold     = std::min (1.0, std::max (1.0e-6, s.threshold));
        s.windowPeriods = std::max (1, s.windowPeriods);

        // The longest lag that is SEARCHED, which is the period of the lowest
        // note, and then two more lags that are only ever COMPUTED.
        //
        // One of those two is needed and the other is slack, and it is worth
        // saying which rather than calling both of them necessary. A dip is
        // recognised by comparing a lag against both its neighbours, and
        // interpolated from both of them, so the dip belonging to the lowest
        // note in the range needs the lag above it to exist: the highest index
        // any reader touches is `searchMaxLag + 1`, which is
        // `tauMaxSamples - 1`. Without it the lowest note is the one note the
        // estimator cannot see as a dip at all: it falls through to the global
        // minimum fallback, and a tuner that does not display its fallback
        // then shows nothing on the open B of a five string bass. Measured
        // before it was fixed, which is the only reason it is written down.
        //
        // The second lag is computed every call and read by nothing. It is one
        // lag of arithmetic and it is kept, because two is the figure the
        // measurement above was taken with, but an audit asked and the honest
        // answer is slack rather than requirement.
        searchMaxLag  = (int) std::ceil (s.sampleRate / s.lowestHz);
        tauMaxSamples = searchMaxLag + 2;

        // Two for the same reason at the other end: the local minimum test at
        // tauMin reads tau-1, and so does the interpolation.
        tauMinSamples = std::max (2, (int) std::floor (s.sampleRate / s.highestHz));

        if (tauMinSamples > searchMaxLag - 1)
            tauMinSamples = std::max (2, searchMaxLag - 1);

        windowSamples = s.windowPeriods * tauMaxSamples;

        filterLength = s.lowPass ? chooseFilterLength (s.sampleRate, s.accurateToHz,
                                                      s.lowPassDroopDb)
                                 : 1;

        // The analysis needs W + tauMax samples, because d(tau) at the longest
        // lag reads x up to index W-1+tauMax. Allocating only W and running
        // the lags out to tauMax is a read of tauMax samples past the end, and
        // this project has already shipped the one sample version of that
        // fault in a delay line. The filter eats another N-1, at the NEWEST
        // end rather than the oldest: it averages forward from each output
        // sample, so it needs N-1 samples beyond the last one it produces.
        // Written down the wrong way round first, which is worth recording
        // because this project has twice found a comment in a latency
        // calculation claiming the opposite of the code beside it.
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

    /** Eq. (8)'s output, for the one check that can see a few per cent of
        distortion in the cumulative mean: a sinusoid's difference function has
        an exact closed form, so the normalisation can be compared against it
        term by term rather than through whatever pitch comes out the far end.
        Nothing in the studio reads this. */
    const std::vector<double>& normalisedDifference() const noexcept { return norm; }

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
        // The normalisation is the step that guards against reading too HIGH:
        // d(0) is zero and so would be the deepest dip in the raw function, and
        // dividing by the running mean is what stops a search falling into it.
        // It is not what guards against reading an octave DOWN; that is the
        // threshold below.
        //
        // The paper says d' "starts at 1 rather than 0", and this writes that
        // down, but nothing here reads it: `tauMinSamples` is at least 2, so
        // neither the dip test nor the fallback ever looks at lag 0 or 1.
        // Mutation testing found this to be a dead store, and it is kept
        // because the function is the paper's and a reader comparing the two
        // should find every line of Eq. (8) present. What actually keeps the
        // search off the zero lag dip is that minimum lag.
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
            chosen = deepest;

            // The interpolated depth, as in the branch above, so that clarity
            // is one quantity rather than two under one name.
            chosenDepth = interpolatedDepth (norm, chosen);
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
    whatever is still left to fall.

    The gate asks for more than one such step, and that is what sets the figure.
    WHY THE CRITERION IS A RATE, AND NOT A SPREAD ACROSS FRAMES

    The obvious thing to write, and what this did first, is to ask that the
    last few readings all lie within some number of cents of their mean. It is
    wrong, and the way it is wrong is instructive, because the bound it
    satisfies is real: the spread across a window of `frames - 1` hops is at
    most twice the tolerance, so the amount still to fall is bounded by
    `R(t) * (exp((frames - 1) * hop / tau) - 1)`. That is a true statement and
    it contains `hop`. The criterion has no notion of time, so how close the
    reading is to the truth when the display opens depends entirely on how
    often the caller asks for one.

    An audit measured what that costs, on this very glide, varying nothing but
    the display's frame rate:

    | frames per second | gate opens at | sharp when it opens |
    |---|---|---|
    | 10 | 1.40 s | +0.37 cents |
    | 15 | 1.23 s | +0.57 cents |
    | 20 | 1.05 s | +0.93 cents |
    | 30 | 0.86 s | +1.50 cents |
    | 60 | 0.56 s | +3.32 cents |

    So the whole design, built around the one cent tier, came apart on a choice
    of timer interval, and a readout on an ordinary thirty hertz timer would
    have shipped a tuner reading one and a half cents sharp on every plucked
    note: the fault the gate exists to prevent, arriving through the gate
    itself. It could be held together by a constant in the readout matching an
    assumption in here, which is exactly the arrangement `CLAUDE.md` records
    going wrong before, where the figure lived in the window and nothing
    tested it.

    So measure the thing that actually matters, which is how fast the pitch is
    still moving. On an exponential settle `dR/dt = -R/tau`, so releasing once
    the rate falls below `r` cents per second leaves

        R = r * tau

    still to come, and `tau` belongs to the string rather than to the display.
    The frame rate drops out of the answer. With the measured `tau` of 0.38 s,
    `r` has to be under about 2.6 to land inside the one cent tier; 1.3 is
    taken, and it measures between 0.17 and 0.46 cents remaining across every
    frame rate from 5 to 60 per second.

    What it costs is about a second and a half of waiting, which is slower than
    a pedal appears to be, and the trade is deliberate: the alternative is a
    display that opens earlier on a string that is still falling. A tighter
    figure does not make the tuner faster, it makes it read sharp.

    WHAT THIS GATE CANNOT DO, MEASURED RATHER THAN HOPED

    The spread is taken across the window rather than the first to last
    difference, so that a pitch wandering up and down is not read as settled
    because it happened to come back to where it started. That is the right
    rule, and it is not sufficient, because the readings are a sampling of the
    pitch and the rule can only see what the sampling catches.

    Sample a vibrato at its own rate, or at a multiple of it, and the readings
    land at the same phase every time. They then agree to a hundredth of a
    cent, the spread is nothing, and the display opens bright and settled on a
    note moving by tens of cents. Measured, a 220.000 Hz note with a 20 cent
    vibrato at the reading rate: **19.6 cents sharp**, with every appearance of
    having settled. A singer would flatten a correct note by a fifth of a
    semitone on the strength of it. The turning points of a vibrato are a
    second way in, because the pitch really is momentarily still there.

    Two things reduce it and neither removes it, and both are measured in
    `Tests/TunerTest.cpp` rather than argued for here:

    - The interval between readings is dithered, so no modulation rate can sit
      at one phase for long. See `Pacing`.
    - The rate has to be satisfied on `holdFrames` consecutive readings rather
      than on one, so a coincidence has to repeat. On a settling pluck the rate
      falls monotonically, so this costs exactly the extra readings and nothing
      else.

    Together, at 44.1 and 48 kHz, where a reading costs 9 to 11 ms and the
    reading rate is around 8.5 a second: the worst wrong reading over a sweep
    of the vibrato band at depths of 2, 5 and 20 cents falls from 19.6 cents to
    4.9, and the number of readings shown at all over that sweep falls from
    1749 to 92. The glide gets better at the same time, from 0.284 cents sharp
    to 0.106, because a longer wait is a more settled string. The cost is 0.38 s
    of extra waiting.

    **At 96 and 192 kHz it is not fixed, and that is the honest statement.** A
    reading costs 48 and 197 ms there, so the duty cycle in `Pacing` drops the
    reading rate to 4.5 and 1.1 a second, the window spans several vibrato
    cycles, and the worst wrong reading stays at 19.9 cents. No choice of
    dither or hold count tried moved it: the measurements are in the backlog.

    The fix is not a constant in here. It is to decimate the tap to a fixed
    analysis rate well above the vibrato band, so that the reading rate stops
    depending on what the interface happens to be set to, which is the one
    thing that makes the sampling unable to alias with a vibrato at all. That
    is a filter design job with a specification behind it and it is written up
    as the next step in `Research/BACKLOG.md`.

    What is shipped is a tuner that is accurate to a fraction of a cent on a
    plucked or sung note held steady, which is how a tuner is used, and that
    can be fooled by a deliberate vibrato at a particular rate, more easily the
    higher the sample rate is set. That is worth having and it is worth saying.
*/
struct GateSettings
{
    /** Below this the tuner says nothing. In dBFS, so it is absolute: a guitar
        through an interface at a sensible input gain sits far above it, and a
        note ringing into the noise floor falls under it. Chosen rather than
        sourced. */
    double levelFloorDb = -55.0;

    /** How many consecutive readings the rate is measured across. Four, which
        spans three hops, so at the default frame rate the rate is measured
        over three tenths of a second. */
    int stabilityFrames = 4;

    /** How slowly the pitch must be moving before a number is shown, in cents
        per second.

        A rate rather than a spread, for the reason in the comment above: a
        spread depends on the caller's frame rate and a rate does not. 1.3
        leaves about `r * tau` still to fall, which on the measured tau of
        0.38 s is half a cent. */
    double settleCentsPerSecond = 1.3;

    /** Roughly how often the caller intends to call `Analyser::process`, in
        seconds.

        This used to be the one figure in here the caller had to get right,
        because the span the rate is divided by was this figure times the frame
        count. It is not any more, and the reason is worth recording, because
        the old arrangement was audited twice and passed both times.

        A readout's real interval is not a constant. It is whatever is left of
        the nominal after the reading has been taken, and above 48 kHz a
        reading costs more than the whole interval, so the interval becomes a
        multiple of the reading's own cost instead: measured, 100 ms at 44.1
        and 48 kHz, 192 ms at 96 and 788 ms at 192. The readout that shipped
        passed this constant anyway, so at 96 kHz the gate was told 0.1 when
        the truth was 0.192 and reported every rate 1.92 times too fast, and at
        192 kHz 7.88 times too fast. The direction happened to be the safe one,
        which is exactly why nobody saw it: the gate was stricter than
        specified rather than looser, and the only cost was waiting.

        So `process` now takes the real interval per call and sums the ones in
        its window. This figure is what is assumed when a caller does not
        supply one, and the scale `stallFrames` is expressed in. Nothing in the
        measurement depends on it any more. */
    double frameSeconds = 0.1;

    /** How many consecutive readings must satisfy the rate before a number is
        shown.

        One is the obvious choice and it is too few. The rate is measured from
        a sparse sampling of the pitch, so a modulation close to the reading
        rate can satisfy it by coincidence; asking for the coincidence to
        repeat is what makes that unlikely rather than ordinary. Measured at
        44.1 kHz over a sweep of the vibrato band, three in a row takes the
        worst wrong reading from 19.6 cents to 4.9 and the number of readings
        shown at all from 1749 to 92.

        It is nearly free on what the gate is actually for. On a settling pluck
        the rate falls monotonically, so once it is satisfied it stays
        satisfied and this costs exactly two more readings: 1.40 s to 1.78 s
        before the display opens, and the reading when it does open is closer
        rather than further, 0.284 cents sharp to 0.106. */
    int holdFrames = 3;

    /** How many nominal intervals a gap between readings may reach before the
        window is thrown away rather than measured across.

        A slow frame is ordinary and must not cost the window: a loaded machine
        delivers them regularly, and clearing the history for each one would
        mean the display never opens. A gap of several intervals is a different
        thing, a descheduled thread or a window being reopened, and readings
        either side of one are not a measurement of how fast the string is
        moving now. Measured across one, a real pitch movement divided by a
        real stall reads as settled. Four, which is slow enough that the
        ordinary case survives it and fast enough that a stall of half a second
        at 44.1 kHz does not. */
    double stallFrames = 4.0;
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

        /** How fast the pitch is still moving, in cents per second, or zero
            before there are enough frames to say. What the gate is actually
            testing, reported so a display can show that it is working rather
            than stuck. */
        double movingCentsPerSecond = 0.0;
    };

    void configure (const Spec& spec, const GateSettings& gate = {})
    {
        detector.configure (spec);
        g = gate;

        // Two, not one: a rate cannot be measured from a single reading, and
        // one frame would release the display on the first periodic buffer,
        // which is the middle of the attack.
        g.stabilityFrames = std::max (2, g.stabilityFrames);

        // A rate of zero asks for a pitch that has stopped moving to the last
        // bit, which never happens, so the display would never open.
        g.settleCentsPerSecond = std::max (1.0e-4, g.settleCentsPerSecond);
        g.frameSeconds         = clampFrameSeconds (g.frameSeconds);

        // One is "show it as soon as the rate is satisfied", which is what
        // this did before it was measured. Allowed, because a caller that
        // wants the fastest possible needle should be able to ask for it.
        g.holdFrames = std::max (1, g.holdFrames);

        // Two, because a window has to survive one slow frame: a loaded
        // machine delivers those regularly, and a bound that cleared the
        // history for each one would mean the gate never filled its window.
        g.stallFrames = std::max (2.0, g.stallFrames);
        reset();
    }

    /** Update the caller's nominal pacing without disturbing a window that is
        already filling.

        Separate from `configure` on purpose, which calls `reset`. A readout's
        pacing drifts with load, every readout's does, and throwing the window
        away each time it drifted would mean the display reopened every time
        the machine got busy. Nothing in the measurement depends on this
        figure: the span is summed from the intervals handed to `process`. */
    void setFrameSeconds (double seconds) noexcept
    {
        g.frameSeconds = clampFrameSeconds (seconds);
    }

    double getFrameSeconds() const noexcept { return g.frameSeconds; }

    void setReferenceHz (double hz) noexcept
    {
        const double next = Scale::quantiseReference (hz);

        // Nothing to invalidate. The stability history is in absolute cents,
        // which do not depend on the reference, so moving the reference moves
        // what is displayed and not what the gate is measuring. Had the
        // history been kept in note space, every stored frame would disagree
        // with the next one and the display would stall for a few frames for
        // no reason; it is kept this way to avoid that.
        referenceHz = next;
    }

    double getReferenceHz() const noexcept { return referenceHz; }

    void reset() noexcept
    {
        history.clear();
        satisfiedInARow = 0;
        last = {};
    }

    int requiredSampleCount() const noexcept { return detector.requiredSampleCount(); }
    const Detector& getDetector() const noexcept { return detector; }

    /** With no interval supplied, the nominal one is assumed. Convenient for a
        caller whose pacing really is fixed, and for a test. */
    Reading process (const float* x, int n) noexcept
    {
        return process (x, n, g.frameSeconds);
    }

    /** `elapsedSeconds` is the real time since the previous call, which is the
        only thing that can measure a rate. A caller that has a clock should
        pass what the clock said rather than what it meant to do, because the
        two differ by the cost of the reading and above 48 kHz that cost is
        larger than the interval. */
    Reading process (const float* x, int n, double elapsedSeconds) noexcept
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
            satisfiedInARow = 0;
            last = r;
            return r;
        }

        // Kept in absolute cents rather than as a frequency, so that the
        // stability test is a test in the units it is specified in: a quarter
        // of a cent means the same thing at the bottom of a bass as at the top
        // of a guitar, where a quarter of a hertz does not.
        const double absoluteCents = 1200.0 * std::log2 (e.hz);

        // Clamped rather than trusted, and the direction of each clamp is the
        // whole point of having them. A figure near zero would divide by
        // nearly nothing and report an enormous rate, which only ever holds
        // the display shut. The upper bound is generous because a legitimate
        // interval really is most of a second at 192 kHz, and truncating a
        // long one understates the span, which overstates the rate, which is
        // the safe direction too. Neither clamp can open the gate early.
        const double sincePrevious = std::min (10.0, std::max (1.0e-4, elapsedSeconds));

        if (sincePrevious > g.stallFrames * g.frameSeconds)
        {
            // See GateSettings::stallFrames. The hold count goes with the
            // window: readings from before a stall cannot count towards an
            // agreement about what the string is doing now.
            history.clear();
            satisfiedInARow = 0;
        }

        history.push_back ({ absoluteCents, sincePrevious });

        if ((int) history.size() > g.stabilityFrames)
            history.erase (history.begin());

        r.settling = true;

        if ((int) history.size() < g.stabilityFrames)
            satisfiedInARow = 0;    // no rate to satisfy yet, so nothing is agreed

        if ((int) history.size() >= g.stabilityFrames)
        {
            // How far the pitch travelled across the window, over how long
            // the window really was. The spread of the readings rather than
            // the first to last difference, so that a pitch wandering up and
            // down is not read as settled because it happened to come back to
            // where it started.
            double lowest = history.front().cents, highest = lowest;

            for (const auto& f : history)
            {
                lowest  = std::min (lowest, f.cents);
                highest = std::max (highest, f.cents);
            }

            // Summed from the intervals that were actually measured, rather
            // than the nominal one multiplied by the frame count. The oldest
            // frame's interval belongs to the hop before this window, so it is
            // not in the sum: four readings span three hops.
            double span = 0.0;

            for (std::size_t i = 1; i < history.size(); ++i)
                span += history[i].sincePrevious;

            r.movingCentsPerSecond = (highest - lowest) / std::max (1.0e-9, span);

            const bool rateSatisfied = r.movingCentsPerSecond <= g.settleCentsPerSecond;

            // Counted up rather than latched, and reset the moment the rate
            // fails, so a pitch that keeps crossing the threshold has to start
            // again. See GateSettings::holdFrames for what that is worth.
            satisfiedInARow = rateSatisfied ? satisfiedInARow + 1 : 0;

            if (rateSatisfied && satisfiedInARow >= g.holdFrames)
            {
                // The newest frame rather than the mean. The gate has already
                // established that the pitch is barely moving across the
                // window, so the mean buys nothing, and on a pitch that is
                // still creeping the mean is the stalest of the readings
                // rather than the closest to where the string is going. An
                // audit measured the difference at 0.20 cents on the glide.
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
    /** One reading, with how long it was after the one before it. The interval
        is stored per frame rather than assumed, because that is the only way
        the span can be the real one when the pacing varies. */
    struct Frame
    {
        double cents         = 0.0;
        double sincePrevious = 0.0;
    };

    /** The upper bound is ten seconds rather than one, because a legitimate
        interval at 192 kHz is already most of a second and a device faster
        than the studio's range would be slower still. */
    static double clampFrameSeconds (double seconds) noexcept
    {
        return std::min (10.0, std::max (1.0e-3, seconds));
    }

    Detector     detector;
    GateSettings g;
    double       referenceHz = Scale::standardReferenceHz;

    std::vector<Frame> history;

    /** How many consecutive readings have satisfied the rate. Part of the
        window rather than of the reading, so it is cleared wherever the
        history is. */
    int satisfiedInARow = 0;

    Reading last;
};

//==============================================================================
/** How long to wait before the next reading, and why that is not the same
    length every time.

    This is arithmetic, not plumbing, so it lives here where a test can drive
    tens of thousands of passes through it in a second. `TunerPanel.h` keeps
    only the clock and the sleep. `CLAUDE.md` records twice what happens when a
    figure this load bearing lives in the window instead, which is that nothing
    tests it until somebody is reading a bug report.

    IT HAS TWO JOBS.

    The first is holding the analysis thread to a fraction of a core. A reading
    costs work proportional to the square of the sample rate, because the
    window is a fixed duration and the difference function is quadratic in it.
    Measured, release build:

    | sample rate | buffer         | one reading |
    |---|---|---|
    |  44100 | 5734  (130 ms) |   9 ms |
    |  48000 | 6239  (130 ms) |  11 ms |
    |  96000 | 12473 (130 ms) |  48 ms |
    | 192000 | 24939 (130 ms) | 197 ms |

    So a tenth of a second between readings is not for sale at the top of that
    range at any price, and asking for it anyway would spin a core for as long
    as the tuner is open, in a process that is also running a device callback.
    The wait is therefore the longer of what is left of the nominal interval
    and `dutyMultiple` times what the reading itself cost.

    The second job is the one that was nearly shipped wrong, so it gets the
    space. The gate decides a string has settled from the spread of the last
    few readings, and those readings are a sampling of the pitch at whatever
    rate this returns. A sampling at a fixed interval is blind to any
    modulation whose period divides it: every reading lands at the same phase,
    the four of them agree to a hundredth of a cent, and the display opens
    bright and settled on a note moving by tens of cents. Measured on the
    shipped gate at a fixed interval, a 220.000 Hz note with a 20 cent vibrato
    at the reading rate reads 19.1 cents sharp.

    And the rates it happens at are not remote, because of the first job. Where
    the duty cycle takes over the reading rate is 5.2 a second at 96 kHz and
    1.3 at 192, and 5.2 Hz is the middle of what a singer's vibrato and a
    guitarist's hand both do. At 44.1 and 48 kHz the reading rate is 10 a
    second, which is above the band, which is why every measurement taken while
    this was written missed it.

    So the interval is dithered: each wait is multiplied by a factor drawn from
    `[1, 1 + jitter]`, which scatters the phase the pitch is sampled at so that
    a modulation cannot sit at one phase for long. The draw is splitmix64 with
    the caller's own seed, for the reason `PanOneBreed.h` carries its own
    generator rather than using `<random>`: the standard distributions are not
    specified exactly, so the same seed gives a different sequence on MSVC, and
    a measurement of how well a dither works has to be a measurement of the
    sequence that ships.

    What it costs is responsiveness and nothing else. The mean interval is
    `1 + jitter/2` times the nominal, so at 44.1 kHz the needle responds about
    every 118 ms rather than every 100. It cannot cost accuracy, because the
    gate's criterion is a rate and the real interval is handed to it per
    reading.

    HOW MUCH IT IS WORTH, AND WHAT IS LEFT. On its own, not much: dithering
    alone took the worst wrong reading over a sweep of the vibrato band from
    19.6 cents to 19.6, because the gate is re-evaluated on every reading and
    only has to be fooled once. It is worth having together with
    `GateSettings::holdFrames`, which makes a coincidence have to repeat:
    measured at 44.1 kHz the pair take the worst wrong reading to 4.9 cents and
    the number of readings shown at all over that sweep from 1749 to 92. At 96
    and 192 kHz, where the duty cycle drops the reading rate into the middle of
    the vibrato band, nothing tried moved it off 19.9 cents.

    So this is a mitigation with a measured size and not a fix. The fix is to
    decimate the tap to a fixed analysis rate well above that band, so the
    reading rate stops depending on what the interface happens to be set to,
    which is a filter design job with a specification behind it rather than a
    constant to change. It is written up as the next step in
    `Research/BACKLOG.md`, with these measurements.
*/
class Pacing
{
public:
    struct Settings
    {
        /** What the interval would be if a reading were free. A tenth of a
            second: fast enough that a player sees the needle move, slow enough
            that the analysis costs a few per cent of one core. */
        double nominalSeconds = 0.1;

        /** The multiple of a reading's own cost that the wait must be at
            least, which is what caps this thread's share of a core. Three
            holds it under a quarter. */
        double dutyMultiple = 3.0;

        /** How far an interval may be stretched, as a fraction of itself.

            A quarter of a cycle of phase scatter is enough to stop a
            modulation sitting still against the reading rate. Four tenths is
            taken because the only thing it costs is responsiveness, and the
            thing it is defending against is a confidently wrong number. */
        double jitter = 0.4;
    };

    void configure (const Settings& s, std::uint64_t seed) noexcept
    {
        cfg = s;
        cfg.nominalSeconds = std::max (1.0e-3, cfg.nominalSeconds);
        cfg.dutyMultiple   = std::max (0.0, cfg.dutyMultiple);
        cfg.jitter         = std::min (4.0, std::max (0.0, cfg.jitter));

        // Taken as given, zero included, so that a caller can reproduce a
        // sequence. Zero is not a fixed point of this generator: its state
        // advance is an addition.
        state = seed;
    }

    const Settings& settings() const noexcept { return cfg; }

    /** How long to wait before taking the next reading, in seconds, given what
        the last one cost in seconds. */
    double nextWait (double spentSeconds) noexcept
    {
        const double spent = std::max (0.0, spentSeconds);
        const double base  = std::max (cfg.nominalSeconds - spent, spent * cfg.dutyMultiple);

        // Never zero. A wait of nothing, on a machine where the reading came
        // back in no measurable time at all, is a spin.
        return std::max (1.0e-4, base) * (1.0 + cfg.jitter * nextUnit());
    }

    /** The whole interval one pass occupies: the reading and then the wait.
        What a caller hands the gate when it has no clock of its own. */
    double expectedHop (double spentSeconds) noexcept
    {
        return std::max (0.0, spentSeconds) + nextWait (spentSeconds);
    }

private:
    /** splitmix64, from Steele, Lea and Flood, "Fast splittable pseudorandom
        number generators", OOPSLA 2014. The same generator `PanOneBreed.h`
        uses, and for the same reason. */
    std::uint64_t nextBits() noexcept
    {
        state += 0x9e3779b97f4a7c15ull;
        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    /** Uniform on [0, 1). The top 53 bits over a power of two, so the division
        is exact and every representable value in the interval is reachable. */
    double nextUnit() noexcept
    {
        return (double) (nextBits() >> 11) * (1.0 / 9007199254740992.0);
    }

    Settings      cfg;
    std::uint64_t state = 0;
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

    HOW BIG THE RING HAS TO BE is a correctness requirement rather than a
    performance tip, and it is worth stating as one. The writer publishes its
    counter AFTER copying its block, so the second read understates how far an
    in-progress block has already reached, by up to one block. The check is
    therefore blind to that block, and the requirement is

        capacity >= snapshot + 2 * largest block the callback will deliver

    Measured with two real threads: at a capacity of the snapshot plus 64, with
    512 sample blocks, 38 per cent of the snapshots this accepted were torn. At
    the snapshot plus 512 it was 29 out of 147572, and at four times the
    snapshot it was none out of 412710.

    The engine's ring is 96000 samples, which is half a second at the highest
    rate the studio runs at and two and a sixth seconds at 44.1 kHz, against a
    snapshot of 130 ms at every rate. So it clears the requirement by a wide
    margin, and the reader has between a third of a second and two seconds to
    copy it, for a copy that is a few tens of microseconds.

    The counter is also monotonic and is never reset, which a reader has to know
    rather than discover: "enough has arrived" latches true the first time it
    does, so whether audio is still arriving is a question about the counter
    moving and not about its value. `TunerPanel.h` is where that is answered.
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

//==============================================================================
/** The highest sample rate the studio will open a device at.

    Here rather than in the engine because the ring's capacity is derived from
    it, and the derivation has to be somewhere a test can compile. Changing
    this alone is not enough to support a faster device: see
    `studioRingCapacity` for what else the figure decides, and note that one
    reading's cost grows as the square of this.
*/
inline constexpr double highestStudioSampleRate = 192000.0;

/** The largest block the device callback will be handed in one call.

    Drivers offer powers of two up to 2048 commonly and 4096 or 8192 on a few
    ASIO devices, and nothing in the studio refuses a large one, so the bound
    is the generous end of that rather than the common end. It is a bound and
    not a measurement: a block larger than this does not break the ring's
    write, which keeps only the newest `capacity` samples, it breaks the
    reader's tear check, which is why it has to be bounded here rather than
    discovered at runtime.
*/
inline constexpr int largestStudioBlockSamples = 8192;

/** The smallest ring that can serve an estimator at this rate without the tear
    check being optimistic, given the largest block the callback will deliver.

    This is `InputRing`'s stated requirement, `capacity >= snapshot + 2 * block`,
    as arithmetic instead of prose. It was prose, and the two numbers it relates
    met in `AudioEngine.cpp`, which no test can compile: an audit pointed out
    that halving the engine's constant to a figure that still looked generous
    would have silently started accepting torn snapshots, with nothing to
    error, assert or fail.
*/
inline int requiredRingCapacity (double sampleRate, int maxBlockSamples, const Spec& base = {})
{
    Spec s = base;
    s.sampleRate = sampleRate;

    Detector d;
    d.configure (s);

    return d.requiredSampleCount() + 2 * std::max (0, maxBlockSamples);
}

/** What the studio's own ring is sized to, once, at construction.

    Sized for the worst case the studio can be asked for rather than for the
    device that happens to be open, because resizing it would allocate while
    the audio thread may be writing into it.
*/
inline int studioRingCapacity()
{
    const int required = requiredRingCapacity (highestStudioSampleRate, largestStudioBlockSamples);

    Spec s;
    s.sampleRate = highestStudioSampleRate;

    Detector d;
    d.configure (s);

    // The requirement is what makes the tear check sound. Four times the
    // snapshot is what the measurement in `InputRing`'s comment found tears
    // nothing at all: none in 412710 against 29 in 147572 at one snapshot plus
    // a block. So take the larger of the two, which is correctness first and
    // then the margin a measurement says is worth paying for. At 192 kHz the
    // margin wins; at a hypothetical rate where blocks dominate the
    // requirement would.
    return std::max (required, 4 * d.requiredSampleCount());
}

} // namespace Tuner
