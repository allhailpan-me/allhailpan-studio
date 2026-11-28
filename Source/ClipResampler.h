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
    a linear time invariant filter: **it produces no new frequencies at all.**
    That is not a measurement and is not quoted as one, it is what the form of
    the expression says; measured through the float buffers the studio actually
    uses, the floor of the measurement arrives first, at about -156 dB. The
    whole defect is a static tilt, because linear
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

    The worst realisable case, and the one the tap count is chosen by, is a
    44.1 kHz session, where the output Nyquist is 22.05 kHz and so only 2.05
    kHz separates the top of the protected band from the place the cutoff has
    to sit. Two conversions can put an alias in there: an **88.2 kHz file** and
    a **clip played fast**. Measured as the worst output level over every place
    an alias can land between 10 and 20 kHz, against a source tone at full
    scale, and in both cases the worst place is the top of the band:

    | taps | 88.2 kHz into 44.1 kHz | a 44.1 kHz clip at 1.2x |
    |---|---|---|
    | 64 | -54.75 dB | -54.75 dB |
    | 84, which is what the formula asks for | -120.36 dB | **-120.08 dB** |
    | **96** | **-121.87 dB** | **-121.08 dB** |
    | 112 | -124.00 dB | -123.27 dB |

    **The formula's answer lands 0.08 dB inside the specification it was
    given**, which is to say exactly on it. That is what the MathWorks page
    that states the formula means when it says in the same breath that a filter
    designed this way "will meet the specifications approximately, but you
    should verify this".

    So **84 taps would have been adequate and 96 is a judgement, recorded as
    one**: twelve more taps is 14 per cent more arithmetic, bought for a
    decibel of margin against an empirical formula whose own documentation asks
    to be checked. A figure that moves across the specification when the test
    tone moves by three hertz is not a figure to ship on. Nothing measured says
    96 is necessary, and that is said here rather than left to look like it
    does.

    What is measured, and is the reason not to go lower, is that **the next step
    down is a cliff rather than a slope**: 64 taps measures -54.75 dB, which is
    65 dB worse than 84. The transition width is inversely proportional to the
    length, so a kernel a third shorter does not have a third of the rejection,
    it has the rejection of a filter whose stopband has not started yet by the
    time the alias arrives.

    It is also not the dramatic failure the research that preceded this code
    reported, and the correction is kept rather than quietly applied. That
    research measured 84 taps at 79 dB, for a 48 kHz source with a tone at
    24.1 kHz. **A 48 kHz file cannot contain a 24.1 kHz tone**: generating one
    produces its own alias at 23.9 kHz, which lands somewhere else entirely
    against the stopband edge. So the sentence "the formula is 41 dB
    optimistic" was measuring a signal that cannot occur. The conclusion, that
    the formula has to be verified rather than trusted, survived. The number
    did not, and the number was the part that sounded convincing.

    For the same reason the old read is quoted on the same case rather than on
    a different one: linear interpolation lets the 24.1 kHz tone through at
    **-3.32 dB**, which is to say almost completely, folded onto 20 kHz.

    And the passband, which is the other thing the length has to buy, is
    comfortable at either: reading a 44.1 kHz file into a 48 kHz session, 20
    kHz measures -0.1652 dB at 48 taps, -0.0159 at 64 and 0.0000 at 84 and
    above. **The passband would have been satisfied by 64 taps. The decimation
    is what sets the length**, which is worth knowing before anybody tries to
    shorten it on the grounds that the response looks flat.

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
    rather than assumed. The loss at 20 kHz reading a 44.1 kHz file into a
    48 kHz session, measured on this kernel:

    | taps | cutoff at 0.90 Nyquist | at 0.95 | at Nyquist |
    |---|---|---|---|
    | 32 | -6.74 dB | -2.73 dB | -0.77 dB |
    | 96 | -8.35 dB | -0.25 dB | -0.00003 dB |

    So the transition band is bought with taps rather than with passband,
    because passband is the thing being fixed. The constructor takes a cutoff
    fraction for no other reason than that this table should be re-runnable: an
    earlier draft of this header quoted three figures from a measurement that
    nothing in the repository could reproduce, which is exactly the kind of
    sentence this project keeps finding in its own comments.

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
    combination argument does not hold. Measured, the residual is -114.95 dB at
    a step of 1.088435, which is a 48 kHz file in a 44.1 kHz session, and
    -115.55 dB at 2.176871, which is a 96 kHz one. **Not "better at larger
    steps", which an earlier draft said on the strength of the whole number
    steps:** those are exact or near it, because the taps land back on the
    lattice, and every step above one that is not a whole number sits at about
    -115 dB whatever its size. The explicit amplitude factor does the real work
    there. Said here rather than claimed exact.

    ## Why a sinc of a whole number of samples is written as exactly zero

    `sin(pi x) / (pi x)` evaluated at `x = 48` does not give zero in floating
    point, it gives about -1.6e-14. Written that way, a read landing exactly on
    a stored sample would come back with fourteen digits of its neighbours
    added to it. Every table entry at a whole number of samples is therefore set
    to its exact value, which is one at the centre and zero everywhere else, so
    **the tap set at phase zero is exactly an impulse and a read at an exactly
    integer position, at any step of one or less, returns the stored sample bit
    for bit.** `ClipResamplerTest` asserts that over randomised positions and
    steps.

    **What that does not buy, and an earlier draft of this comment claimed it
    did: a bounce of an untouched take does not reproduce the file bit for
    bit.** The engine's read position comes from a beat accumulated one
    `beatsPerSample` at a time, so it lands near a whole sample rather than on
    one. An audit simulated that accumulation: at 120 bpm and 48 kHz a clip at
    the first bar is bit exact for 47154 output samples in 48000 and never more
    than 3.7e-9 out, and at 128 bpm with the clip at beat 64 it is bit exact for
    284 in 48000 and up to 3.7e-6 out, which is -108.7 dB. That is inaudible
    and far below a sixteen bit word, and it is not a null. The old two point
    read was equally inexact for the same reason, so nothing regressed, but the
    guarantee belongs to the kernel and not to the product, and saying otherwise
    would be a claim somebody could check and find false.

    ## One read for playback and for the bounce

    Three of the studios whose documentation was read do the opposite. Ableton
    says plainly that its export path uses a different and better resampler
    from its realtime path and that its high quality clip read is a per clip
    opt in with a CPU cost; Ardour and Logic both document a quality choice at
    import. `CLAUDE.md`'s rule goes the other way and wins here, because
    `renderAudioClips` is called by both paths and anything added to one and
    not the other makes a bounce sound different from what was heard.

    The cost is what makes that affordable rather than merely principled, and
    it is quoted rather than waved at. Measured, one sounding stereo clip at a
    step of one is about 1.1 per cent of a core at 48 kHz, which is 65 times
    what the two point read cost. Per sounding clip, not per clip in the
    project. **At twenty four tracks of audio playing at once that is roughly a
    quarter of a 128 sample callback before a single plugin runs**, and nearer
    half on a slow laptop, so it is a real budget rather than free. The probe
    names the two things that would halve it, a constant tap set for a whole
    block at a step of one and a fixed point table index, and neither was
    needed to afford this.

    ## Two things this changes rather than fixes, both named rather than found

    **A read can come out above the input's peak.** Linear interpolation was a
    convex combination of two samples, so it could never leave the input's
    range. A windowed sinc overshoots. Measured on this kernel, a full scale
    square wave reads 2.15 dB above full scale, and the worst case signal, the
    sign of each tap, reads 9.4 dB above it, which is `Kernel::phaseGain`.

    This is correct bandlimited reconstruction rather than a fault, and it is
    the same fact `TruePeak.h` exists for: the continuous waveform a converter
    reconstructs from a square wave really does overshoot, and the largest
    sample in a file is not its peak. But the consequence is real and belongs
    where somebody will find it. A master limited to exactly full scale,
    imported at 44.1 kHz into a 48 kHz session, now reads above full scale on
    its insert. The float mixer does not care, and a bounce to a fixed point
    word length can clip where it did not before, so the same project bounced
    before and after this change differs at the top. Nothing in the studio
    needs to do anything about it, and nothing pretends it did not happen.

    **A trimmed clip carries a little of what was trimmed off.** The read is
    non causal by the kernel's half width, which at a step of one is 48 source
    samples, about a millisecond. So a clip trimmed to cut a count in click or
    a false start now carries the sinc tail of that material into its first
    output samples: a full scale transient ten samples before the trim arrives
    at roughly -24 dB, and at the trim point itself a step from full scale to
    silence reads about half amplitude. Again this is what a bandlimited
    reconstruction of that file is, and every studio does it; a comp join is
    crossfaded so the folder case is masked, and `ClipFade`'s de-click fade
    covers the clip case wherever it is set. Written down because it changes
    what a hard trim means.

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
    half width below was verified by measurement instead, because the formula's
    own answer meets the specification by 0.08 dB, which for an empirical
    formula is no margin at all. It is kept so that the design can be stated in
    the terms the sources state it in, and so the probe can show how little
    room the formula leaves.
*/
inline double kaiserTaps (double attenuationDb, double transitionCyclesPerSample) noexcept
{
    if (transitionCyclesPerSample <= 0.0)
        return 0.0;

    return (attenuationDb - 7.95) / (14.36 * transitionCyclesPerSample);
}

//==============================================================================
/** How far the read position advances per output sample, in source samples.

    One line, and it is here rather than in the engine for the reason
    `CLAUDE.md` gives about `LatencyGraph.h`: it is the quantity that decides
    the cutoff, so getting it wrong is the fault the whole of this header
    exists to prevent, and a decision moved back into the engine stops being
    testable. An audit found exactly that: inverting this expression in
    `AudioEngine.cpp` left every one of the suite's programs green while a
    96 kHz file read into a 48 kHz session passed its content above 24 kHz
    straight through, folded, at full level.

    `clipRate` is the clip's stretch ratio in source seconds per output second,
    which is `AudioClipRT::rate`.
*/
inline double stepFor (double clipRate, double sourceRate, double deviceRate) noexcept
{
    return deviceRate > 0.0 ? clipRate * sourceRate / deviceRate : 0.0;
}

/** The same, for a warped clip, whose rate is the slope of the segment being
    read rather than one figure for the whole clip.

    `sourceSecondsPerBeat` is `WarpSegment::slope` and `beatsPerSample` is the
    transport's own, so the two arguments carry the conversion between the
    arrangement's time base and the file's. Kept beside `stepFor` because the
    two have to agree: a warped clip whose segments all have the slope an
    unwarped clip would have must read at the same step, and `ClipResamplerTest`
    asserts that rather than leaving it to inspection.
*/
inline double stepForWarpSlope (double sourceSecondsPerBeat, double beatsPerSample,
                                double sourceRate) noexcept
{
    return sourceSecondsPerBeat * beatsPerSample * sourceRate;
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
        one or less.

        **Verified rather than derived, and 96 rather than 84 is a judgement
        about margin rather than a measured necessity.** The Kaiser length
        formula asks for 84 at 120 dB, and 84 measures -120.08 dB on the worst
        case in the studio, which meets the specification by 0.08 dB. 96
        measures -121.08 dB. The whole argument, including the figure an
        earlier draft of this file got wrong, is in the header comment under
        "The tap count", and the tables are in
        `Tests/ClipResampleProbe.cpp`. **Do not shorten this from the passband,
        which 64 taps already satisfies: the decimation is what sets the
        length, and 64 taps measures -54.75 dB.** */
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
        samples per output sample. Smith's method needs a limit chosen
        somewhere, because the tap count grows as the step does and a clip read
        four times fast already costs four times the taps.

        Beyond this the cutoff stops coming down, so the read aliases. **The
        engine can ask for more than this, by about a factor of three**, and an
        audit established that rather than it being assumed otherwise: a clip's
        stretch is clamped to [0.1, 10] in `Project.h`, so `rate` reaches 10,
        and `followTempo` sets it from a tempo ratio with no clamp at that site
        at all, which puts the step at 8 for a 96 kHz file at four times speed
        and around 12 for a tempo matched one. A warp segment's slope is
        clamped below and not above and can do the same.

        So this is where the protection stops and not where the engine stops,
        and a clip at eight times speed aliases at full level. It is still an
        improvement on what it replaced, which aliased at every step, and
        clamping the stretch to four is a change in `Project.h`, which an open
        pull request holds. Recorded here rather than described as a limit the
        studio observes. */
    static constexpr double maxTrackedStep = 4.0;

    int    crossings = 0;
    int    entriesPerCrossing = 0;
    double attenuationDb = 0.0;
    double cutoffFraction = 1.0;
    double beta = 0.0;

    std::vector<float> taps;        /**< right wing, normalised per phase */
    std::vector<float> differences; /**< taps[j+1] - taps[j], last entry zero */

    Kernel()
        : Kernel (defaultCrossings, defaultEntriesPerCrossing, defaultAttenuationDb)
    {
    }

    /** `cutoff` is the cutoff as a fraction of the source's Nyquist, and the
        shipped value is 1, which is Smith's rule exactly. It is a parameter
        only so that the figures the header quotes for backing it off can be
        measured again rather than taken on trust: a number whose measurement
        cannot be re-run is a number nobody can revisit, which is what
        `Tests/WaveTableProbe.cpp` exists for in the instrument. Nothing in the
        studio passes anything but 1. */
    Kernel (int halfWidthCrossings, int perCrossing, double stopbandDb, double cutoff = 1.0)
    {
        crossings          = halfWidthCrossings < 1 ? 1 : halfWidthCrossings;
        entriesPerCrossing = perCrossing < 1 ? 1 : perCrossing;
        attenuationDb      = stopbandDb;
        cutoffFraction     = cutoff > 0.0 ? cutoff : 1.0;
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

            if (cutoffFraction == 1.0 && j % entriesPerCrossing == 0)
            {
                // Only a zero of the sinc when the cutoff is at the Nyquist,
                // which is the shipped case. A backed off cutoff puts its zeros
                // somewhere else and has to be evaluated.
                sinc = j == 0 ? 1.0 : 0.0;
            }
            else if (j == 0)
            {
                sinc = 1.0;
            }
            else
            {
                const double x = 3.14159265358979323846 * cutoffFraction
                               * (double) j / (double) entriesPerCrossing;
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

    /** The sum of the magnitudes of one phase's taps, which is that phase's
        gain against the worst case input and therefore the most a read can
        ever produce from full scale material.

        It is not one. A windowed sinc overshoots, and that is the one
        user visible difference between this read and the lerp it replaced that
        is a change rather than a fix: linear interpolation is a convex
        combination of two samples, so it can never leave the input's range,
        and this read can. Measured on the shipped kernel, a full scale square
        wave reads up to 2.1 dB above full scale and the worst case signal, the
        sign of each tap, reads 9.4 dB above it.

        That is correct bandlimited reconstruction rather than a fault: the
        continuous waveform a converter would produce from a square wave really
        does overshoot, which is the same fact `TruePeak.h` exists for. But it
        means a master that was limited to exactly full scale can read above it
        here, so a bounce to a fixed point word length can clip where it did
        not before, and the same project bounced before and after this change
        differs at the top. Exposed rather than left implicit because it is the
        bound `ClipResamplerTest` checks against, and an audit found the test
        checking against 1.5 with a comment that named this quantity. */
    double phaseGain (int phase) const noexcept
    {
        const int count = entryCount();
        double sum = 0.0;

        for (int i = -crossings + 1; i <= crossings; ++i)
        {
            const int entry = std::abs (i * entriesPerCrossing - phase);

            if (entry >= 0 && entry < count)
                sum += std::abs ((double) taps[(std::size_t) entry]);
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
    //
    // The range covers every tap that can contribute, and at a whole number
    // step it is exactly those taps: one contributes while
    // `|i - offset| < crossings / scale`, which with `offset` in [0, 1) is `i`
    // from `-half + 1` to `half`. At any other step `half` is a ceiling and the
    // range is a superset by a tap or two, which the guard below trims.
    //
    // `std::floor` rather than a truncation towards zero, because truncating
    // puts `offset` in (-1, 1) for a position just before the start of the file
    // and shifts the window by one, dropping a tap at the far end of the
    // kernel. **That is worth a part in ten million, not a correctness
    // requirement**, and it is written down that way deliberately: an audit
    // measured the difference at -144.5 dB, and `CLAUDE.md` already carries one
    // case here of a margin written up as a requirement, in
    // `Tuner::InputRing`. Flooring is still the right thing to write, because
    // it is what the arithmetic means and it costs nothing.
    for (int i = -half + 1; i <= half; ++i)
    {
        const int sample = centre + i;

        if (sample < 0 || sample >= frames)
            continue;

        const double distance = std::abs ((double) i - offset) * tableScale;
        const int    entry    = (int) distance;

        // **This is load bearing, and an earlier draft of this comment said it
        // could not fire.** It fires one or two times per output sample on the
        // most ordinary conversion the studio does. `half` is a ceiling, so
        // whenever `crossings / scale` is not a whole number the loop runs over
        // a tap or two that cannot contribute, and this is what trims them:
        // reading a 48 kHz file in a 44.1 kHz session produces table indices up
        // to 24931 against a table of 24577 entries, at every fractional
        // offset. Deleting it reads up to 1416 bytes past the end of a heap
        // vector, on the audio thread. The suite does catch its deletion,
        // through `_GLIBCXX_ASSERTIONS` rather than through any measurement.
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
