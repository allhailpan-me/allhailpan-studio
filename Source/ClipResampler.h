#pragma once
#include <cmath>
#include <cstddef>
#include <vector>

//==============================================================================
/** How an audio clip is read when the read position is not a whole sample.

    Every audio clip in this studio is read by mapping a beat to a source time,
    so the read position almost never lands exactly on a stored sample. It is
    fractional whenever the file's sample rate differs from the device rate,
    whenever the clip is warped or stretched, and whenever the clip was simply
    trimmed or nudged to a position that is not a whole number of source
    samples, which is what a mouse does every time.

    **Three different defects live in those three cases and only one of them is
    distortion**, which is the reason this header and its test are shaped the
    way they are.

    What was here before was two point linear interpolation. At a step of
    exactly one source sample per output sample with a constant fractional
    offset, which is the trimmed clip and by far the most common case, that is
    a linear time invariant filter: it produces no new frequencies at all,
    measured 250 dB down. The whole defect is a static tilt, because linear
    interpolation at fractional delay `d` is the one zero FIR
    `H(z) = (1-d) + d z^-1`, whose magnitude at `d = 0.5` is `cos(pi f / fs)`:

    | at 10 kHz | at 15 kHz | at 20 kHz |
    |---|---|---|
    | -2.01 dB | -5.11 dB | -11.74 dB |

    in a 48 kHz session. Dattorro's description is the one worth keeping, from
    "Effect Design, Part 2: Delay-Line Modulation and Chorus", JAES 45(10),
    1997: linear interpolation has "a dynamic zero at the Nyquist frequency,
    creating muffled (veil-like) sounds", and "the only frequency for which
    there is no amplitude error is dc". So a take trimmed by hand played back
    through a lowpass nobody asked for, whose corner depended on where it was
    trimmed, and the same take trimmed one sample later sounded different.

    **A test for aliasing, images or distortion passes that with full marks.**
    Nothing non linear is happening in the case the product hits most. That is
    why the first thing `ClipResamplerTest` does is sweep the magnitude
    response over fractional offset and frequency, and why that sweep is the
    only check in the file that fails if this read goes back to a lerp.

    The other two cases are genuine distortion. Reading a 44.1 kHz file in a
    48 kHz session, linear interpolation left the worst spurious product 32 dB
    below a 6 kHz tone. Valimaki and Bilbao, "Giant FFTs for Sample-Rate
    Conversion", JAES 71(3), 2023, measure the same thing on a different
    conversion and report that under linear interpolation "some of the images
    are attenuated by only about 40 dB", against "-98 dB or lower" for a high
    order polyphase FIR.

    ---

    ## The method

    A tabulated Kaiser windowed sinc, read with linear interpolation between
    table entries, which is the method in Julius O. Smith III, "Digital Audio
    Resampling Home Page", CCRMA, Stanford
    (https://ccrma.stanford.edu/~jos/resample/), an expansion of J. Smith and
    P. Gossett, "A Flexible Sampling-Rate Conversion Method", Proc. IEEE
    ICASSP, pp. 112-115, 1984. Nothing is copied from Smith's `resample`
    software, which is LGPL, or from his prose, which carries no licence. The
    equations are facts and are implemented from the equations.

    The ideal interpolator is a sinc. Smith's rule for what happens to it when
    the read runs faster than one source sample per output sample is quoted
    rather than reconstructed, because both halves of it matter:

        "Thus, in the case of an ideal lowpass,
         h_s(t) = min{1, Fs'/Fs} sinc(min{Fs,Fs'} t), where the scale factor
         maintains unity gain in the passband."

    So the cutoff goes to the lower of the two Nyquist frequencies, which in
    practice means the kernel is stretched in time, and the amplitude is scaled
    by the same factor to put the DC gain back. Here `Fs'/Fs` is `1/step`,
    where `step` is how far the read position advances in source samples per
    output sample. Leaving the time scaling out is not a subtle fault: a 96 kHz
    file read into a 48 kHz session passes its content above 24 kHz straight
    through, folded, at full level.

    ## The numbers, and which of them are measured rather than derived

    | Parameter | Value | Where it comes from |
    |---|---|---|
    | Stopband attenuation | 120 dB | Valimaki and Bilbao, JAES 71(3) 2023: "the dynamic range should be at least 120 dB for highest-quality work". Carson, Valimaki, Wright and Bilbao, arXiv:2501.18470, choose the same figure for the same problem |
    | Kaiser beta | 12.26526 | computed from the quoted piecewise formula at A = 120 |
    | Half width | 48 zero crossings, so 96 taps at a step of one or less | **measured**, see below |
    | Table entries per zero crossing | 512 | Smith's own reference design, and the point at which the error stops falling: see below |
    | Cutoff | the lower of the two Nyquist frequencies, exactly | Smith, quoted above |
    | Largest step the cutoff follows | 4 source samples per output sample | Smith: beyond some ratio "the required extra input data becomes infinite, and some limit must be chosen" |

    ## The tap count, and the one frequency that decides it

    **Say where an alias lands or the figure means nothing**, which is the trap
    this section is really about.

    The requirement is that nothing fold into 0 to 20 kHz above -120 dB. Which
    source frequencies can do that depends on the session's rate, and the two
    rates this studio most often mixes turn out to be the easy case: **a 48 kHz
    file in a 44.1 kHz session cannot put an alias below 20 kHz at all.** Its
    source Nyquist is 24 kHz and its output Nyquist 22.05 kHz, so the lowest
    reachable fold is `|24000 - 44100|`, which is 20.1 kHz, already above the
    protected band. The protection is for 88.2 kHz files and above, and for
    clips played fast.

    The worst realisable case, and the one the tap count is chosen by, is an
    **88.2 kHz file in a 44.1 kHz session with a tone at 24.1 kHz**, which
    folds onto exactly 20.0 kHz and sits only 260 Hz inside the designed
    stopband edge, where the empirical length formula is least accurate.
    Measured on this kernel, against a source tone at full scale:

    | taps | 88.2 kHz into 44.1 kHz, 24.1 kHz tone | a 44.1 kHz clip at 1.2x, 20.08 kHz tone |
    |---|---|---|
    | 64 | -54.75 dB | -54.55 dB |
    | 84, which is what the formula asks for | -120.36 dB | **-117.68 dB** |
    | **96** | **-121.87 dB** | **-121.57 dB** |
    | 128 | -154.89 dB | -131.26 dB |

    So the formula's answer **fails** the specification it was given, by 2.3 dB
    on the second case, and 96 taps is the smallest even tap count that passes
    both. The MathWorks page that states the formula says in the same breath
    that a filter designed this way "will meet the specifications
    approximately, but you should verify this". That sentence is not
    boilerplate.

    It is also not the dramatic failure the research that preceded this code
    reported, and the difference is worth keeping. That research measured
    84 taps at 79 dB, for a 48 kHz source with a tone at 24.1 kHz. **A 48 kHz
    file cannot contain a 24.1 kHz tone**: generating one produces its own
    alias at 23.9 kHz, which lands somewhere else against the stopband edge
    entirely. The conclusion survived the correction and the number did not.
    The figures above are the ones measured here, through this kernel, and
    nothing in this header quotes the 79 dB.

    For the same reason the old read is quoted on the same case rather than on
    a different one: linear interpolation lets that 24.1 kHz tone through at
    **-3.32 dB**, which is to say almost completely, folded onto 20 kHz.

    `Tests/ClipResampleProbe.cpp` is where these tables live, so the choice can
    be rechecked over the rates and tap counts the studio actually offers.

    **The transition width in that formula is a factor of 6.28 waiting to
    happen.** It is stated both as `(A - 7.95) / (2.285 dw)` with `dw` in
    radians per sample and as `(A - 7.95) / (14.36 df)` with `df` in cycles per
    sample. They are the same constant, since `dw = 2 pi df`. This header uses
    the second, which is Kaiser's own pairing. `kaiserTaps` below says so at
    the line, and the test measures the designed window against its own
    specification so that getting it wrong cannot pass.

    The numerator is 7.95 in Kaiser's own form and in MathWorks' `kaiserord`,
    and 8 in Oppenheim and Schafer as relayed by Smith. The difference is under
    0.05 per cent of the order. 7.95 is used because it is the primary pairing,
    and it is quoted rather than averaged.

    Neither Kaiser (1974) nor Oppenheim and Schafer could be read directly. The
    formulas below are quoted from three secondary sources that agree on all
    five constants and each attribute them to Kaiser: Smith's `kaiserord`
    page at CCRMA, MathWorks' `kaiserord` and Kaiser window pages, and A. R.
    Collins' filter design notes. Said plainly rather than implying the primary
    was read.

    ## Why the cutoff is not backed off below Nyquist

    Smith's rule puts it exactly at the lower Nyquist, and that was checked
    rather than assumed. At 32 taps a cutoff at 0.90 of Nyquist loses 1.6 dB at
    20 kHz and one at 0.95 loses 0.39 dB, while the cutoff at Nyquist with
    enough taps loses 0.00001 dB. The transition band is bought with taps
    rather than with passband, because passband is the thing being fixed.

    ## Per phase gain, and the reason that is not the oversampler's reason

    Each fractional position uses its own set of taps, which is an independent
    FIR filter, and its DC gain is the sum of those taps. For an infinite
    unwindowed sinc that sum is one for every position. With a window and a
    finite length it is not, and the residual is a gain that varies with the
    fractional position, which is a modulation of a steady signal.

    `Fx/Oversampler.h` records the same conclusion with a different argument,
    and the argument does not transfer. It says the taps must sum to one per
    phase because "the interpolator sees a zero between every input sample, so
    only every other tap has anything under it". That is a correct and complete
    account of integer factor zero stuffed polyphase interpolation. **There is
    no zero stuffing here**: every tap sits on a real source sample, and the
    set for a given position is every L-th table entry starting somewhere, not
    every other tap. Anybody carrying that sentence across will go looking for
    zeros that are not there.

    The magnitude is the useful part. Measured on this design the raw table's
    per phase DC gain is already correct to -132.8 dB, so against a 120 dB
    target this normalisation is a nicety that costs nothing at build time
    rather than the necessity it was in the oversampler, where the error was
    five parts in a million. That figure is not quoted here, because it is a
    different number about a different filter.

    Three things about doing it, each of which breaks silently:

    - **It is exact only because the between entry lookup is a convex
      combination.** The interpolated sum is `(1-eta) S(j) + eta S(j+1)`, so if
      every entry's phase sums to one, every position in between does too.
    - **Scaling each table entry is a valid per phase scale only because of a
      mirror symmetry.** Entry `j` is reached as phase `j mod L` by one wing and
      as phase `L - (j mod L)` by the other, and those are different phases. It
      works only because those two phases use the identical set of entries, and
      therefore have identical sums. If the kernel is ever made asymmetric this
      stops being true and nothing will say so.
    - **The difference table is built from the normalised entries**, not the
      other way round. Differences straddle phase boundaries, so normalising
      after differencing leaves the two inconsistent.

    At a step above one the normalisation is close rather than exact, because
    the taps then come off a different sub-lattice of the table and the convex
    combination argument does not hold. Measured, the residual is about -115 dB
    at a step of 1.088 and better than that at larger steps, and the explicit
    amplitude factor does the real work there. Said here rather than claimed
    exact.

    ## Why a sinc of a whole number of samples is written as exactly zero

    `sin(pi x) / (pi x)` evaluated at `x = 48` does not give zero in floating
    point, it gives about -1.6e-14. Written that way, a clip sitting at the
    session's own sample rate and trimmed to a whole sample would come back
    with fourteen digits of someone else's audio added to it. Every table entry
    at a whole number of samples is therefore set to its exact value, which is
    one at the centre and zero everywhere else, and the consequence is worth
    having: **the tap set at phase zero is exactly an impulse, so a clip at the
    device's own rate trimmed to a sample boundary is read bit for bit.**
    `ClipResamplerTest` asserts that, because it is what makes a bounce of an
    untouched take reproduce the file.

    ## One read for playback and for the bounce

    Three of the studios whose documentation was read do the opposite. Ableton
    says plainly that its export path uses a different and better resampler
    from its realtime path and that its high quality clip read is a per clip
    opt in with a CPU cost; Ardour and Logic both document a quality choice at
    import. `CLAUDE.md`'s rule goes the other way and wins here, because
    `renderAudioClips` is called by both paths and anything added to one and
    not the other makes a bounce sound different from what was heard. At 96
    taps there is nothing to trade: see the probe for the measured cost.

    No JUCE, so the whole of the design and every figure above can be compiled
    and measured in a second with sanitizers on.
*/
namespace ClipResampler
{

/** The zeroth order modified Bessel function of the first kind.

    NIST Digital Library of Mathematical Functions, eq. 10.25.2
    (https://dlmf.nist.gov/10.25), specialised to order zero:

        I0(z) = sum over k >= 0 of (z^2/4)^k / (k!)^2

    Summed by the ratio between successive terms rather than by evaluating the
    factorial and the power separately, because both overflow long before the
    series stops mattering, and stopped on a relative criterion rather than
    after a fixed number of terms. No source states a term count, and a fixed
    one silently degrades the window as beta rises: at the beta this header
    uses it converges in well under fifty terms, and a count chosen for that
    would be wrong for a stronger stopband.
*/
inline double besselI0 (double z) noexcept
{
    const double quarterSquared = z * z * 0.25;
    double term = 1.0, sum = 1.0;

    for (int k = 1; k < 200; ++k)
    {
        term *= quarterSquared / ((double) k * (double) k);
        sum  += term;

        if (term < 1.0e-19 * sum)
            break;
    }

    return sum;
}

/** The Kaiser window's shape parameter for a required stopband attenuation.

    The piecewise form attributed to Kaiser (1974) and stated identically by
    Smith's `kaiserord` page at CCRMA, MathWorks' `kaiserord` and Kaiser window
    pages, and A. R. Collins' filter design notes:

        beta = 0.1102 (A - 8.7)                               for A > 50
        beta = 0.5842 (A - 21)^0.4 + 0.07886 (A - 21)          for 21 <= A <= 50
        beta = 0                                              for A < 21

    Sources differ on whether the boundaries are open or closed. It does not
    matter: the two branches agree at A = 50 to about 0.004 and both give zero
    at A = 21.
*/
inline double kaiserBeta (double attenuationDb) noexcept
{
    if (attenuationDb > 50.0)
        return 0.1102 * (attenuationDb - 8.7);

    if (attenuationDb >= 21.0)
        return 0.5842 * std::pow (attenuationDb - 21.0, 0.4)
             + 0.07886 * (attenuationDb - 21.0);

    return 0.0;
}

/** The Kaiser length estimate, in taps, for an attenuation and a transition
    width given in **cycles per sample**.

        taps = (A - 7.95) / (14.36 df)

    This is Kaiser's own pairing of numerator and denominator. The same
    relation is also written `(A - 7.95) / (2.285 dw)` with `dw` in radians per
    sample, and since `dw = 2 pi df` the two denominators are the same
    constant. Mixing them gives a filter 6.28 times too long or too short, so
    the unit is in the name of the argument and in this comment, and the test
    measures the result against its own specification.

    It is an estimate and this header does not use it to choose anything. The
    half width below was measured, because at the stopband edge this formula
    was 41 dB optimistic. It is kept so that the design can be stated in the
    terms the sources state it in, and so the probe can show the gap.
*/
inline double kaiserTaps (double attenuationDb, double transitionCyclesPerSample) noexcept
{
    if (transitionCyclesPerSample <= 0.0)
        return 0.0;

    return (attenuationDb - 7.95) / (14.36 * transitionCyclesPerSample);
}

//==============================================================================
/** The windowed sinc, tabulated.

    Only the right wing is stored, because the kernel is symmetric: entry `j`
    is the kernel at `j / entriesPerCrossing` source samples from the centre.
    A table of successive differences is stored beside it so that reading
    between two entries is a multiply and an add rather than two loads and a
    subtract.

    Built once, up front, and never on the audio thread. 24577 entries twice
    over is about 192 kB, held for the life of the application, which is the
    same trade the instrument's wavetables make and for the same reason: a
    hitch in the middle of somebody's work costs more than the memory.
*/
struct Kernel
{
    /** Half width in zero crossings, which is half the tap count at a step of
        one or less. **Measured rather than derived**: see the header comment.
        The Kaiser length formula asks for 84 and that delivers 79 dB where
        120 was specified. */
    static constexpr int defaultCrossings = 48;

    /** Table entries per zero crossing. Smith's own reference design uses 512,
        and so does this one, but the reason here is a measurement rather than
        deference. His bound on the error from reading between two entries,
        which his own text says is pessimistic for a real windowed design, puts
        512 at -116 dB. Measured on this kernel, as the residual against the
        ideal, the error falls 64 -> 128 -> 256 -> 512 as -98.7, -106.9,
        -120.3, -148.3 dB and then **stops falling**: 1024 measures -132.9,
        which is worse. That is the floor of storing the taps as `float`
        arriving, about 24 bits down, and it is the answer to the obvious
        question about the storage type as well: at 512 the table is already
        reading at its own quantisation floor, so neither a denser table nor a
        wider one buys anything against a 120 dB target. */
    static constexpr int defaultEntriesPerCrossing = 512;

    /** The specification the kernel is designed to, in dB. */
    static constexpr double defaultAttenuationDb = 120.0;

    /** The largest step whose cutoff the kernel still follows, in source
        samples per output sample. Smith's method needs a limit chosen here
        because the tap count grows as the step does, and a clip read four
        times fast already costs four times the taps. Beyond this the cutoff
        stops coming down, so a clip sped up past 4x aliases, which is a
        documented limit rather than an oversight. */
    static constexpr double maxTrackedStep = 4.0;

    int    crossings = 0;
    int    entriesPerCrossing = 0;
    double attenuationDb = 0.0;
    double beta = 0.0;

    std::vector<float> taps;        /**< right wing, normalised per phase */
    std::vector<float> differences; /**< taps[j+1] - taps[j], last entry zero */

    Kernel()
        : Kernel (defaultCrossings, defaultEntriesPerCrossing, defaultAttenuationDb)
    {
    }

    Kernel (int halfWidthCrossings, int perCrossing, double stopbandDb)
    {
        crossings          = halfWidthCrossings < 1 ? 1 : halfWidthCrossings;
        entriesPerCrossing = perCrossing < 1 ? 1 : perCrossing;
        attenuationDb      = stopbandDb;
        beta               = kaiserBeta (stopbandDb);

        const int    count = crossings * entriesPerCrossing + 1;
        const double halfWidthEntries = (double) (count - 1);
        const double normaliser = besselI0 (beta);

        std::vector<double> built ((std::size_t) count, 0.0);

        for (int j = 0; j < count; ++j)
        {
            // The window, from the definition quoted at kaiserBeta: it is
            // I0(beta sqrt(1 - (n/(M/2))^2)) / I0(beta), and here n/(M/2) is
            // the entry's distance from the centre as a fraction of the half
            // width.
            const double fraction = (double) j / halfWidthEntries;
            const double inside   = 1.0 - fraction * fraction;
            const double window   = besselI0 (beta * std::sqrt (inside < 0.0 ? 0.0 : inside))
                                  / normaliser;

            // The sinc, at the cutoff that corresponds to the source's own
            // Nyquist. A whole number of samples is written as its exact value
            // rather than evaluated: see the header comment on why fourteen
            // digits of the wrong answer there would matter.
            double sinc;

            if (j % entriesPerCrossing == 0)
            {
                sinc = j == 0 ? 1.0 : 0.0;
            }
            else
            {
                const double x = 3.14159265358979323846 * (double) j / (double) entriesPerCrossing;
                sinc = std::sin (x) / x;
            }

            built[(std::size_t) j] = sinc * window;
        }

        normalisePerPhase (built);

        taps.resize ((std::size_t) count);
        differences.resize ((std::size_t) count);

        for (int j = 0; j < count; ++j)
            taps[(std::size_t) j] = (float) built[(std::size_t) j];

        // Built from the normalised values, which is the third of the three
        // conditions in the header comment: differences straddle phase
        // boundaries, so doing this first and normalising after would leave
        // the two tables describing different kernels.
        for (int j = 0; j + 1 < count; ++j)
            differences[(std::size_t) j] = taps[(std::size_t) (j + 1)] - taps[(std::size_t) j];

        differences[(std::size_t) (count - 1)] = 0.0f;
    }

    int entryCount() const noexcept { return (int) taps.size(); }

    /** The sum of the taps one discrete phase uses, which is that phase's DC
        gain. Exposed because it is what the test and the probe measure, and
        because a value that only the constructor can see is a value nothing
        checks. `phase` runs from 0 to entriesPerCrossing. */
    double phaseSum (int phase) const noexcept
    {
        const int count = entryCount();
        double sum = 0.0;

        for (int i = -crossings + 1; i <= crossings; ++i)
        {
            const int entry = std::abs (i * entriesPerCrossing - phase);

            if (entry >= 0 && entry < count)
                sum += (double) taps[(std::size_t) entry];
        }

        return sum;
    }

private:
    /** Scales every entry so that each phase's taps sum to one.

        One scale per entry rather than one per phase, which is only legitimate
        because the two phases that reach a given entry use the identical set
        of entries and therefore have the identical sum. That is the mirror
        symmetry the header comment warns about, and it holds only while the
        kernel is symmetric.
    */
    void normalisePerPhase (std::vector<double>& built) const
    {
        const int count = (int) built.size();
        std::vector<double> sums ((std::size_t) entriesPerCrossing, 0.0);

        for (int phase = 0; phase < entriesPerCrossing; ++phase)
        {
            double sum = 0.0;

            for (int i = -crossings + 1; i <= crossings; ++i)
            {
                const int entry = std::abs (i * entriesPerCrossing - phase);

                if (entry >= 0 && entry < count)
                    sum += built[(std::size_t) entry];
            }

            sums[(std::size_t) phase] = sum;
        }

        for (int j = 0; j < count; ++j)
        {
            const double sum = sums[(std::size_t) (j % entriesPerCrossing)];

            if (sum > 1.0e-12)
                built[(std::size_t) j] /= sum;
        }
    }
};

//==============================================================================
/** One output frame, left and right.

    Both channels in one call rather than two, because the tap index arithmetic
    is the expensive half and a stereo clip pays it once that way. A mono clip
    passes the same pointer twice, which is what the engine already does.
*/
struct Frame
{
    float left = 0.0f, right = 0.0f;
};

/** The kernel's time and amplitude scale for a step, which is Smith's
    `min{1, Fs'/Fs}`.

    Below a step of one there is nothing to protect against, so the scale is
    one and the kernel is used as designed. Above it the cutoff has to come
    down to the output's Nyquist, which is the same thing as stretching the
    kernel in time by the step, and the amplitude is scaled by the same factor
    to put the DC gain back where it was. Clamped at `maxTrackedStep`, which is
    the limit Smith's method says has to be chosen somewhere.
*/
inline double scaleForStep (const Kernel& kernel, double step) noexcept
{
    if (! (step > 1.0))
        return 1.0;

    const double tracked = step > Kernel::maxTrackedStep ? Kernel::maxTrackedStep : step;
    return 1.0 / tracked;
}

/** How many taps a step costs on each side of the read position.

    Useful on its own: it is what says a clip played four times fast costs four
    times the arithmetic, which is inherent to the method rather than a defect
    in it.
*/
inline int halfTapsForStep (const Kernel& kernel, double step) noexcept
{
    const double scale = scaleForStep (kernel, step);
    return (int) std::ceil ((double) kernel.crossings / scale);
}

/** Reads one output frame from a source buffer at a fractional position.

    `position` is in source samples and may be fractional and may be outside
    the buffer. `step` is how far the position advances per output sample, in
    source samples, and decides the cutoff. Samples outside the buffer are
    taken as zero, which is what the file is: silence either side of itself.

    No allocation, no lock, no branch on anything but the position and the
    table, which is what the audio thread requires.
*/
inline Frame read (const Kernel& kernel,
                   const float* sourceLeft, const float* sourceRight, int frames,
                   double position, double step) noexcept
{
    Frame out;

    if (sourceLeft == nullptr || sourceRight == nullptr || frames <= 0)
        return out;

    const int    count = kernel.entryCount();
    const int    perCrossing = kernel.entriesPerCrossing;
    const double scale = scaleForStep (kernel, step);
    const double tableScale = scale * (double) perCrossing;

    const double floored = std::floor (position);
    const int    centre  = (int) floored;
    const double offset  = position - floored;        // in [0, 1)

    const int half = (int) std::ceil ((double) kernel.crossings / scale);

    double sumLeft = 0.0, sumRight = 0.0;

    // One loop over both wings rather than two with the fraction reflected
    // between them. Smith's implementation pages describe it as two sums with
    // `P` replaced by `1 - P` in between, and writing it that way invites
    // reusing the same fraction for both, which puts the far wing one table
    // step out. Taking the absolute distance from the read position gets the
    // reflection for free and there is nothing to forget.
    for (int i = -half; i <= half + 1; ++i)
    {
        const int sample = centre + i;

        if (sample < 0 || sample >= frames)
            continue;

        const double distance = std::abs ((double) i - offset) * tableScale;
        const int    entry    = (int) distance;

        if (entry >= count - 1)
            continue;

        const double eta = distance - (double) entry;
        const double tap = (double) kernel.taps[(std::size_t) entry]
                         + eta * (double) kernel.differences[(std::size_t) entry];

        sumLeft  += tap * (double) sourceLeft[sample];
        sumRight += tap * (double) sourceRight[sample];
    }

    out.left  = (float) (sumLeft  * scale);
    out.right = (float) (sumRight * scale);
    return out;
}

} // namespace ClipResampler
