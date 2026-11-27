#pragma once

#include "RealFFT.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

/*
    What a mix measures across the spectrum, in fractional octave bands.

    The mix report already says how loud the master is, how far its peaks really
    go, how much dynamic range is left and whether it survives a fold to mono.
    It had nothing to say about where in the spectrum the weight sits, which is
    the first question anybody asks about a mix they cannot quite place, and
    which is the one check that finds the two faults monitoring cannot: energy
    below what the speakers reproduce at all, and a resonance sitting exactly
    where the room already flatters it.

    This is the arithmetic of that display and nothing else. No JUCE, so the
    whole chain from a buffer of samples to a band level can be run in a test
    in a second with sanitizers on, which matters here more than usual: every
    mistake available in this file produces a perfectly plausible looking
    spectrum, and three of them are a fixed number of decibels that nobody
    could spot by eye.

    ------------------------------------------------------------------------
    THE BANDS ARE IEC 61260-1:2014's. THE FILTERS ARE NOT.
    ------------------------------------------------------------------------

    The band centres and edges below are the standard's, cited clause by
    clause. The *filters* are not: this sums FFT bins inside a band's edges,
    which is a brick wall, and IEC 61260-1 specifies tolerance masks on filter
    skirts. Neither IEC 61260-1 nor ANSI S1.11 mentions the FFT at all, and
    ANSI/ASA S1.11-2004 clause 1.3 leaves the realisation to the manufacturer
    provided the finished instrument conforms. So this cannot claim conformance
    and does not. Logic's MultiMeter is the one analyser on this project's
    reference list that claims to comply with IEC 1260, and it is a filter
    bank rather than a transform.

    Said out loud because "IEC 61260 bands" reads like a conformance claim and
    is not one, and because somebody will one day compare this against a
    measurement microphone's analyser and need to know which half is the
    standard's.

    ------------------------------------------------------------------------
    THE DISPLAY TILT DEFAULTS TO ZERO, AND THAT IS THE WHOLE POINT
    ------------------------------------------------------------------------

    Voxengo SPAN and FabFilter Pro-Q 4 both document a default display slope of
    4.5 dB per octave, pivoting, in both manuals' words, "around 1 kHz". It is
    the obvious number to copy and it would have been wrong here by 45 dB from
    end to end.

    Those two displays are per FFT bin, which is constant bandwidth: Pro-Q's
    Resolution is in FFT points and its curve is per bin, and Apple says the
    same of Logic's Channel EQ analyser, that its bands are "spaced linearly in
    frequency, so the highest octave has far more bands than the lowest". On a
    constant bandwidth display pink noise falls at 3.01 dB per octave, and
    3.01 dB per octave of tilt is exactly what flattens it. The documented 4.5
    is that 3.01 plus about 1.49 dB per octave of deliberate preference, which
    is what Pro-Q's manual means when it says the result "resembles how
    loudness is perceived".

    A constant ratio band display summed as power already reads pink noise
    flat, and exactly rather than nearly. Pink noise has power density c/f, so
    a band holds the integral of c/f from f1 to f2, which is c * ln(f2/f1), and
    f2/f1 is G^(1/b) for every band whatever its centre. So every band holds the
    same power, and measured across 20 Hz to 20 kHz the spread between them is
    a few times 1e-14 dB, which is floating point residue and nothing else. No
    figure is quoted to two places here because the residue is not reproducible
    between two ways of writing the same expression, and quoting it as if it
    were would claim more than can be had.

    So the band summing and the 3.01 dB per octave part of the conventional
    4.5 are the same correction applied twice. Do both and pink noise, the
    reference signal the whole convention exists to flatten, rises across the
    display at the full tilt value. The default here is therefore 0.0 dB per
    octave, which is what copying SPAN's and Pro-Q's *intent* rather than their
    number means, and the residual preference tilt that survives the move to
    bands is `preferenceTiltDbPerOctave` below, which is 1.49 and not 4.5.

    Worth knowing how this would have shipped: it would have looked right. A
    real mix carries far more low frequency energy than pink noise, so a 4.5 dB
    per octave tilt partly cancels it and the picture stays believable. Only a
    noise source shows it up, which is why the test drives one.

    The other direction is the same mistake: dividing a band's power by its
    bandwidth in hertz turns pink noise from flat to falling at 3 dB per
    octave. Band energy is an integral and not an average, and because these
    band widths grow in proportion to frequency the integral already *is* the
    constant ratio reading. `Fx/Tilt.h` learned the same thing measuring a
    noise spectrum, from the other side.

    ------------------------------------------------------------------------
    WHAT IS A CHOICE RATHER THAN A SPECIFICATION
    ------------------------------------------------------------------------

    Named here rather than implied, because this file cites two standards and
    the reader is entitled to know where the citations stop.

    - The mapping from FFT bins to bands. No standard, and no manufacturer
      documents theirs. The choice here is a power sum with the two edge bins
      weighted by the fraction of their width that falls inside the band, so
      that the weights of any one bin across all bands sum to one and a band
      sitting between bin centres is neither doubled nor dropped.
    - Refusing a band narrower than one bin instead of approximating it. See
      `resolves` below. No source anywhere for this; the precedent is this
      project's own `Tuner::InputRing`, which refuses a snapshot it cannot
      serve safely rather than returning a torn one.
    - The window. Voxengo's Primary guide names Hann as its default and calls
      it "best for music analysis"; every other manual read documents the
      control and withholds the default.
    - How often a reading is taken, which here is not a parameter at all: there
      is no overlap mechanism, and the hop is however often the display asks
      for a frame. See `transformSizes` for what that means and what it
      costs.
    - The tilt pivot of exactly 1000 Hz. SPAN and Pro-Q both say only "around
      1 kHz" and neither publishes a number. The pivot only slides the whole
      curve up or down, so it is cosmetic, but this cannot claim to match
      either product's numbers.
    - Referencing the scale to a full scale sine in BOTH channels, so that one
      reads 0 dB and a single channel carrying full scale reads -3.01. See
      `fullScaleReferenceDb`.
    - Infinite peak hold with a manual reset. The most widely documented
      behaviour (SPAN's "Max", Pro-Q's Freeze, Ableton's Max, Insight's
      Infinite, and one of Logic MultiMeter's options), but Logic also
      publishes timed holds of 2, 4, 5 and 6 seconds and SPAN publishes a
      decaying maximum tied to a 20 dB fall time. There is no consensus and
      there are three incompatible unit conventions for "speed".
    - Nominal labels for one sixth octave bands. IEC 61260-1:2014 Annex E.3 is
      normative, specifies the labels for bandwidths other than octave, half
      octave and third octave, and is paywalled; ISO 266 standardises only the
      one third octave series. So no preferred series is invented here. Thirds
      are labelled from ISO 266 and sixths are not labelled at all: see
      `nominalThirdOctaveFrequency`.
*/
namespace Spectrum
{

//==============================================================================
// Band geometry: IEC 61260-1:2014, "Electroacoustics - Octave-band and
// fractional-octave-band filters - Part 1: Specifications", with ANSI/ASA
// S1.11-2004 as the second reading of the same equations.

/** The reference frequency, IEC 61260-1:2014 clause 5.3 and ANSI S1.11-2004
    clause 3.4: 1000 Hz, exactly. */
inline constexpr double referenceFrequency = 1000.0;

/** The octave frequency ratio G, IEC 61260-1:2014 clause 5.2.1 formula (1):
    `G = 10^(3/10)`, which clause 5.2.2 gives as 1.99526 to six figures.

    Base ten and not base two. `G = 2` exists, is acknowledged in a note to
    clause 5.2.2 and is the subject of the standard's informative Annex D, but
    the normative definition in the body of the standard is base ten, and it is
    what the ISO 266 nominal labels were derived from.

    The two agree at 1 kHz exactly and diverge towards the ends of the audio
    range: -23.27 cents at the 20 Hz band and +17.79 cents at the 20 kHz band.
    So a check written at 1 kHz passes under either convention and says
    nothing about which one is implemented, which is why the test checks a
    band at each end instead.
*/
inline double octaveRatio() noexcept { return std::pow (10.0, 0.3); }

/** The exact mid-band frequency of band `x` at `1/bandsPerOctave` bandwidth.

    IEC 61260-1:2014 clause 5.4.1 formula (2), for b ODD:

        f_m = f_r * G^(x / b)

    IEC 61260-1:2014 clause 5.4.2 formula (3), for b EVEN:

        f_m = f_r * G^((2x + 1) / (2b))

    `x` is any integer, positive, negative or zero.

    THE ODD AND EVEN CASES ARE DIFFERENT FORMULAS AND THIS IS THE TRAP IN THE
    WHOLE FILE. One third octave has b = 3, which is odd, so 1 kHz is a band
    centre. One sixth octave has b = 6, which is even, so there is no integer x
    with (2x + 1) / 12 = 0 and 1 kHz is a band EDGE, shared between the bands
    centred at 944.06 and 1059.25 Hz.

    Build sixths with the odd formula and the result looks entirely correct:
    the right number of bands, the right ratio between them, the right widths,
    nothing out of range and nothing to error. Every centre is out by G^(1/12),
    which is 99.66 cents, almost exactly a semitone. The test pins it from both
    sides, by asserting that 1 kHz is a centre at thirds and an edge at sixths,
    and by asserting the nesting property below, which the wrong formula also
    breaks.

    Note also that the band index is IEC's `x` and not ANSI's band number.
    ANSI S1.11-2004 clause 3.5 equation (3) writes the exponent as (x - 30) / b,
    so the 1 kHz third octave band is band 30 there and x = 0 here, and ANSI's
    own abstract says the term "band number" is not present in IEC 61260 at
    all. Mixing the two conventions shifts the display by ten octaves.
*/
inline double midBandFrequency (int x, int bandsPerOctave) noexcept
{
    const double b = (double) bandsPerOctave;

    const double exponent = (bandsPerOctave % 2 != 0)
                                ? (double) x / b
                                : (2.0 * (double) x + 1.0) / (2.0 * b);

    return referenceFrequency * std::pow (octaveRatio(), exponent);
}

/** `G^(1/(2b))`, the ratio from a band's centre to either of its edges. */
inline double edgeRatio (int bandsPerOctave) noexcept
{
    return std::pow (octaveRatio(), 1.0 / (2.0 * (double) bandsPerOctave));
}

/** The band edges, ANSI S1.11-2004 clause 3.7 equations (5) and (6):

        f_1 = G^(-1/(2b)) * f_m        f_2 = G^(+1/(2b)) * f_m

    IEC 61260-1:2014 carries the same pair in clause 5.6, whose body is behind
    the paywall and was not read; its clause 3.8 defines the edges such that
    the exact mid-band frequency is their geometric mean, which forces this
    form given clause 5.4. The test asserts that geometric mean property
    directly rather than trusting the second hand reading.
*/
inline double lowerBandEdge (double midBand, int bandsPerOctave) noexcept
{
    return midBand / edgeRatio (bandsPerOctave);
}

inline double upperBandEdge (double midBand, int bandsPerOctave) noexcept
{
    return midBand * edgeRatio (bandsPerOctave);
}

/** The band width as a fraction of the centre frequency, ANSI S1.11-2004
    clause 3.16 equation (9): `(f_2 - f_1) / f_m = G^(1/(2b)) - G^(-1/(2b))`.

    0.230768 at one third octave and 0.115193 at one sixth. Note these are not
    the base two figures: `2^(1/6) - 2^(-1/6)` is 0.231563, which is close
    enough to pass any eyeball check and is a different number.
*/
inline double normalisedBandwidth (int bandsPerOctave) noexcept
{
    const double r = edgeRatio (bandsPerOctave);
    return r - 1.0 / r;
}

/** The nominal label for a one third octave band, from ISO 266:1997.

    IEC 61260-1:2014 clause 5.5 is explicit that filters "shall be identified,
    or labelled, by their nominal mid-band frequencies", so this is what a
    display puts on its axis: 1000 and not 1000.0000, 31.5 and not 31.6228.

    ISO 266:1997 clause 1 states the preferred frequencies are the R10 series
    of preferred numbers from ISO 3 referred to 1000 Hz, and its clause 4.2
    says the table may be extended by powers of ten. So the series is generated
    from one decade rather than transcribed, which is also what makes it
    extend past the printed table without a second list to keep in step.

    Returns 0.0 for anything that is not a one third octave band, because
    there is no honest answer: Annex E.3 of IEC 61260-1:2014 specifies the
    labels for other bandwidths, it is normative, and it was not read. Inventing
    a preferred series for sixths and calling it standard is exactly the kind of
    plausible invention this project keeps meeting.

    So a display at any other bandwidth has no band to hang a preferred label
    on, and the panel does not try: it labels its frequency axis by frequency,
    placing each preferred frequency where it actually falls, which at one
    sixth octave is the join between the two bands nested in a third. An
    earlier version matched labels to band centres within three per cent, which
    at one sixth octave matches nothing at all, since the exact nesting puts
    every third octave centre 5.93 per cent from the nearest sixth octave
    centre. That display had sixty two bars and a blank axis, and an audit
    found it.
*/
inline double nominalThirdOctaveFrequency (int x, int bandsPerOctave) noexcept
{
    if (bandsPerOctave != 3)
        return 0.0;

    // One decade of the R10 series, starting at the reference band.
    static constexpr double decade[10] = { 1000.0, 1250.0, 1600.0, 2000.0, 2500.0,
                                           3150.0, 4000.0, 5000.0, 6300.0, 8000.0 };

    // Floor division and a non-negative remainder, which `/` and `%` do not
    // give for negative operands in C++: -17 / 10 is -1 and -17 % 10 is -7,
    // where this needs -2 and 3. Written out rather than left to the built in
    // operators because the bands below 1 kHz are all at negative x, which is
    // most of the audio range.
    int decadeIndex = x / 10;
    int within      = x % 10;

    if (within < 0)
    {
        within += 10;
        --decadeIndex;
    }

    return decade[(std::size_t) within] * std::pow (10.0, (double) decadeIndex);
}

/** One band of a display. `x` is IEC's band index, so it is stable under a
    change of range and survives being written down. */
struct Band
{
    int    x      = 0;
    double centre = 0.0;
    double lower  = 0.0;
    double upper  = 0.0;
};

/** The lowest edge of the audio range: the lower edge of the one third octave
    band ISO 266 labels 20 Hz, which is 17.7828 Hz.

    The range is defined by edges rather than by centres for two reasons. The
    20 Hz band's exact centre is 19.9526, which is below 20, so a range stated
    in centres would drop the band it is named after. And because sixths nest
    exactly inside thirds, a range stated in edges gives the same span at
    either bandwidth, with no band half in and half out.
*/
inline double audioRangeLowerEdge() noexcept
{
    return lowerBandEdge (midBandFrequency (-17, 3), 3);
}

/** The highest edge of the audio range: the upper edge of the 20 kHz band,
    which is 22387.2 Hz. Above Nyquist at 44.1 kHz, which `bands` handles. */
inline double audioRangeUpperEdge() noexcept
{
    return upperBandEdge (midBandFrequency (13, 3), 3);
}

/** The bands of a display at this bandwidth and sample rate, lowest first.

    Every band that fits entirely inside the audio range and entirely below
    Nyquist. At one third octave and 48 kHz that is the full 31 bands, 20 Hz to
    20 kHz nominal. At 44.1 kHz the 20 kHz band is left out, because its upper
    edge is 22387.2 Hz and Nyquist is 22050. Truncated there it would read
    0.30 dB low on a 1/f density and 0.33 dB low on a flat one, which is small,
    and the point is not the size: the deficit changes with the sample rate, so
    it would be a band that quietly means something different on different
    hardware. Left out rather than shown wrong, which is the same decision as
    `resolves` below and for the same reason.

    An earlier draft of this comment said 0.74 dB, which came from the research
    this was built from and does not reproduce: the figure is
    `10*log10(ln(22050/17782.79) / ln(22387.21/17782.79))`, which is 0.296. Left
    on the record rather than quietly corrected, because a number that is
    present, plausible and wrong is what this project keeps meeting, and an
    audit found this one.

    Each band's lower edge is taken from its neighbour's upper edge rather than
    computed again, so the display is exactly contiguous. Computed separately
    the two differ in the last bit or two, since one is `centre * G^(1/(2b))`
    and the other is a different centre divided by it, and that leaves
    hairline gaps between the bands. Harmless in a drawing and not harmless in
    two places: a cursor landing in one reports no band, and a bin straddling
    one has a fraction of itself in neither, so the partition the weights rely
    on stops being exact. There is no accumulation, because every upper edge is
    still computed from its own centre.

    Returns an empty list for a bandwidth below 1 or a sample rate that cannot
    carry even one band.
*/
inline std::vector<Band> bands (int bandsPerOctave, double sampleRate)
{
    std::vector<Band> result;

    if (bandsPerOctave < 1 || ! (sampleRate > 0.0))
        return result;

    const double lowestEdge  = audioRangeLowerEdge();
    const double highestEdge = std::min (audioRangeUpperEdge(), 0.5 * sampleRate);

    if (! (highestEdge > lowestEdge))
        return result;

    // A relative slack, because the nesting of sixths inside thirds is exact
    // in the mathematics and lands a few parts in 1e16 either side of the
    // boundary in floating point. Without it the lowest and highest band at
    // one sixth octave would come and go depending on rounding, which is the
    // kind of thing that works on one compiler.
    const double slack = 1.0 + 1.0e-9;

    // Wide enough for every bandwidth this is sensibly asked for, and bounded
    // so that a nonsense bandwidth cannot spin here.
    const int limit = 64 * std::max (1, bandsPerOctave);

    for (int x = -limit; x <= limit; ++x)
    {
        Band band;
        band.x      = x;
        band.centre = midBandFrequency (x, bandsPerOctave);
        band.lower  = lowerBandEdge (band.centre, bandsPerOctave);
        band.upper  = upperBandEdge (band.centre, bandsPerOctave);

        if (band.lower * slack >= lowestEdge && band.upper <= highestEdge * slack)
            result.push_back (band);
    }

    for (std::size_t i = 1; i < result.size(); ++i)
        result[i].lower = result[i - 1].upper;

    return result;
}

//==============================================================================
// The window, and its two constants.

/** The periodic Hann window, `w(i) = 0.5 - 0.5 * cos(2 * pi * i / N)`.

    Harris's equation (27b) form, and the one `Spectrogram.h` already builds.
    Periodic rather than symmetric, and the reason is the test rather than the
    sound: the periodic form has a coherent gain of exactly 0.5 and an
    equivalent noise bandwidth of exactly 1.5 bins at every size, which are
    Harris's published figures to as many places as anybody wants. The
    symmetric form, dividing by N - 1, gives 0.499878 and 1.500366 at 4096.
    That difference is 0.002 dB and inaudible, and it makes both constants
    depend on the transform size, so no test can assert a number and a size
    change becomes a silent level change.
*/
inline void buildHannWindow (std::vector<double>& window, int size)
{
    window.assign ((std::size_t) std::max (0, size), 0.0);

    constexpr double twoPi = 6.283185307179586476925286766559;

    for (int i = 0; i < size; ++i)
        window[(std::size_t) i] = 0.5 - 0.5 * std::cos (twoPi * (double) i / (double) size);
}

/** Coherent gain, Harris equation (13) normalised by N: `sum(w) / N`.

    The constant for a discrete sinusoid read off ONE BIN. Exactly 0.5 for the
    periodic Hann, which is the figure in Harris's Table I.

    Exposed to be asserted and explained, and not used in the band path. See
    `equivalentNoiseBandwidth`, which is the one that is, and which carries the
    measurement that settled which of the two belongs here. */
inline double coherentGain (const std::vector<double>& window)
{
    if (window.empty())
        return 0.0;

    double sum = 0.0;

    for (const auto w : window)
        sum += w;

    return sum / (double) window.size();
}

/** Equivalent noise bandwidth in bins, Harris equation (11):
    `N * sum(w^2) / (sum(w))^2`.

    A DIFFERENT constant from the one above. Exactly 1.5 for the periodic Hann,
    again Harris's Table I.

    THIS IS THE ONE THE BAND PATH USES, and which of the two belongs here was
    settled by measurement rather than by argument, because the argument is
    genuinely slippery and the research this was built from got it the other
    way round.

    The case for the coherent gain is that a full scale sine should read 0 dB,
    which is Voxengo's documented "Align 0 dB" behaviour, and the coherent gain
    is the constant that makes a tone read its own amplitude. All true, and it
    is true OF ONE BIN. A band is a sum of bins, and a windowed tone does not
    sit on one bin: the periodic Hann puts a tone exactly on three, at
    relative amplitudes 1, 1/2 and 1/2, whose powers add to exactly 1.5 times
    the peak bin's. So a band wide enough to hold the main lobe, which is every
    band this displays, collects all of it and reads 1.7609 dB high. Measured,
    at 48 kHz and a transform size of 16384: the peak bin reads 0.0000 dB and
    the band sum reads +1.7609 dB.

    Dividing by the noise bandwidth instead makes the per bin quantity the
    power in one bin's worth of bandwidth, so a band sum is the integral of the
    power density across the band, which is what a band level is. Measured on
    the same two signals: a full scale sine in both channels reads 0.0000 dB in
    its band, and the band powers of a broadband signal filling the display sum
    to that signal's own mean square to within 0.016 dB, which is 37 parts in
    10000. Both at once, which is the check that this is the right constant and
    not a coincidence that happens to suit a tone.

    Written out at this length because the mistake is invisible. It is a fixed
    1.7609 dB on everything, so the shape of the spectrum is untouched, pink
    noise still reads flat, and the only thing that moves is the number beside
    the cursor. Conflate it with the peak against RMS convention as well and
    the error is 4.7712 dB, which decomposes as 3.0103 + 1.7609: somebody
    chasing that as one 4.8 dB bug will never find it, so both halves are named
    here.
*/
inline double equivalentNoiseBandwidth (const std::vector<double>& window)
{
    if (window.empty())
        return 0.0;

    double sum = 0.0, sumOfSquares = 0.0;

    for (const auto w : window)
    {
        sum          += w;
        sumOfSquares += w * w;
    }

    if (! (sum > 0.0))
        return 0.0;

    return (double) window.size() * sumOfSquares / (sum * sum);
}

//==============================================================================
// Ballistics: IEC 61672-1, "Electroacoustics - Sound level meters - Part 1".

/** The two time weightings the standard defines, by its own names. */
enum class Ballistics
{
    fast,   /**< F, 125 ms. */
    slow    /**< S, 1 s. */
};

/** IEC 61672-1 clause 5.7.1, time weighting F: 0.125 s.

    Clause numbers and figures here are from the 2002 edition, read as the full
    text of IS 15575-1:2005, which is its Indian national adoption. The
    official preview of the 2013 edition would not open. F and S are unchanged
    between the editions.
*/
inline constexpr double fastTimeConstantSeconds = 0.125;

/** IEC 61672-1 clause 5.7.1, time weighting S: 1 s. */
inline constexpr double slowTimeConstantSeconds = 1.0;

inline double timeConstantSeconds (Ballistics b) noexcept
{
    return b == Ballistics::slow ? slowTimeConstantSeconds : fastTimeConstantSeconds;
}

/** The decay rate a one pole on the mean square gives, in dB per second:
    `10 / ln(10) / tau`, which is 34.74 dB/s for F and 4.343 dB/s for S.

    IEC 61672-1 clause 5.7.2 specifies the acceptance windows rather than these
    ideal figures, measured after a steady 4 kHz tone stops: at least 25 dB/s
    for F, and between 3.4 and 5.3 dB/s for S. Both ideals sit inside their
    window, as they must, and the test asserts against the standard's windows
    rather than against this expression, so that the check is the specification
    and not a restatement of the code.
*/
inline double idealDecayDbPerSecond (double timeConstant) noexcept
{
    return timeConstant > 0.0 ? 10.0 / std::log (10.0) / timeConstant : 0.0;
}

//==============================================================================
// Display conventions.

/** What the levels are read against: A FULL SCALE SINE IN BOTH CHANNELS READS
    EXACTLY 0 dB, which needs no offset at all, so this is zero.

    It is zero for a reason rather than by luck, and the reason is worth having
    because an earlier version of this file added 3.0103 here and was wrong.
    A full scale sine has a mean square of one half. The two channels are
    summed as powers, so a full scale sine present in both of them sums to
    exactly one, and ten times the logarithm of one is zero. The factor of two
    from peak referencing a single channel and the factor of two from having
    two channels are the same factor of two, which is why adding the first on
    top of the second read every stereo mix 3.01 dB high.

    The reference is therefore the stereo one, and that is the choice. The mix
    report measures the master, which is always two channels, so referencing to
    a full scale stereo tone is referencing to the loudest thing that can
    arrive: the top of the display is where full scale material sits, and the
    reading agrees with what this studio's peak meters say about the same tone.
    It is also what Voxengo documents for SPAN's "Align 0 dB" control, which
    aligns "a stationary 1 kHz sine at 0 dBFS to 0 dB visually, regardless of
    Block Size", since that is a claim about the signal as presented rather
    than about one leg of it.

    THE CONSEQUENCE, because somebody will report it as a bug: a full scale
    sine in ONE channel reads -3.01 dB, not zero. That is correct. One leg of a
    stereo master carrying full scale is half the power of two legs carrying
    it, and this display is about how much of the mix sits in a band.

    Two more disagreements, both expected. AES17 designates the RMS value of a
    full scale sine as 0 dBFS, which for a single channel is 3.01 dB below this
    scale; Sound Forge references that way and Adobe Audition lets the user
    pick, so the industry does not agree and this is a choice rather than a
    specification. And this does not agree with `LoudnessMeter.h` on a tone,
    because BS.1770 is an RMS loudness scale with its own -0.691 dB offset: a
    full scale stereo sine reads 0.0 dB here and -3.70 LUFS there. What the two
    DO agree about is which signal "the mix" is, because both sum the channels
    as powers with a gain of one, so a change that moves one moves the other.

    A tilt can still put a band above this, which is why the display draws
    above zero rather than claiming nothing can reach it: see the tilt below.
*/
inline constexpr double fullScaleReferenceDb = 0.0;

/** The tilt a per FFT bin display needs to make pink noise flat: 3.0103 dB per
    octave, which is `10 * log10(2)` again and for the same reason, one octave
    being a doubling.

    Not used by this analyser. It is here so that the relationship to the
    documented 4.5 can be written down as arithmetic rather than as prose, and
    so the test can assert that the band path needs none of it.
*/
inline constexpr double perBinPinkTiltDbPerOctave = 3.0102999566398120;

/** The 4.5 dB per octave that Voxengo SPAN and FabFilter Pro-Q 4 both document
    as their default display slope. */
inline constexpr double documentedPerBinTiltDbPerOctave = 4.5;

/** What is left of that 4.5 once the band summing has done the pink flattening
    part: 1.4897 dB per octave.

    This is the honest translation of the convention, and the number to reach
    for if the display should lean the way SPAN and Pro-Q lean out of the box.
    It is a preference and not a correction, which is why it is not the default.
*/
inline constexpr double preferenceTiltDbPerOctave
    = documentedPerBinTiltDbPerOctave - perBinPinkTiltDbPerOctave;

/** The frequency the tilt pivots around. Exactly 1000 Hz, chosen: SPAN and
    Pro-Q both say only "around 1 kHz" and neither publishes a number. */
inline constexpr double tiltPivotFrequency = 1000.0;

/** What a band with nothing in it reads, and the bottom of the scale. Matches
    the figure `AudioEngine::MixReading` already uses for "no measurement". */
inline constexpr double floorDb = -200.0;

//==============================================================================
// What the tap in front of the analyser has to be able to serve.

/** The transform sizes the display offers, smallest first.

    This is the trade FabFilter documents for Pro-Q's Resolution control, in
    its own words: more low frequency precision against a lower update rate and
    a slower attack. What it costs here, as the lowest one third octave band
    that resolves at all, measured:

        rate        2048      4096      8192     16384
        44.1 kHz   100 Hz     50 Hz     25 Hz     20 Hz
        48 kHz     125 Hz     63 Hz   31.5 Hz     20 Hz
        96 kHz     250 Hz    125 Hz     63 Hz   31.5 Hz
        192 kHz    500 Hz    250 Hz    125 Hz     63 Hz

    So the shortest window does not lose a band or two at the very bottom, it
    loses the whole bass range: eight of the thirty one bands at 48 kHz, and
    twenty one of the sixty two at one sixth octave. And the longest window
    reaches the 20 Hz band only at 44.1 and 48 kHz. An earlier version of this
    comment said 2048 "cannot resolve anything below 25 Hz", which was out by
    two octaves and was found by an audit.

    There is no overlap mechanism anywhere in this file, so these lengths also
    decide how much of the audio is looked at at all. The hop is however often
    the display asks for a frame, which is its refresh rate: at 48 kHz a 16384
    window at twenty frames a second overlaps by 85 per cent, and a 2048 window
    at the same rate leaves a 7 ms gap between frames, so 15 per cent of the
    audio is never analysed. At 192 kHz with the shortest window the gap is
    four fifths of the audio. Nothing in a steady mix cares, and a short
    transient landing in a gap is simply not seen, which is the one thing to
    know before trusting a peak hold at the shortest window. */
inline constexpr int transformSizes[4] = { 2048, 4096, 8192, 16384 };

inline constexpr int largestTransformSize = 16384;

/** The smallest ring capacity that can serve a frame of the largest transform
    size safely, given the largest block a device callback can deliver.

    `Tuner::InputRing`, which is the ring the engine uses for this, states its
    requirement as `capacity >= snapshot + 2 * block`: one block because the
    write counter is published after a block rather than during it, and one
    more so there is room to tell an intact copy from an overwritten one. This
    doubles that block margin.

    Derived here rather than written down in `AudioEngine.cpp`, and that is not
    a tidiness point. The tuner's two numbers met in that file once, which no
    test can compile, and an audit pointed out that halving the capacity to a
    figure that still looked generous would have silently started accepting
    torn snapshots with nothing to error or fail. Here a test can check the
    arithmetic against the requirement it comes from.
*/
inline constexpr int ringCapacity (int largestBlockSamples) noexcept
{
    return largestTransformSize + 4 * largestBlockSamples;
}

/** The same figure for a ring holding the two legs interleaved, which is what
    the engine actually keeps.

    One ring rather than two, so the legs are aligned by construction: one
    write, one tear check, and no way for a block arriving between two separate
    reads to leave the display analysing a different stretch of the left from
    the right. Two rings were tried first and the comment claiming they were
    one frame was not true of them, which an audit pointed out.

    Everything is in interleaved samples, so both the snapshot and the block
    bound double, and this doubles with them.
*/
inline constexpr int interleavedRingCapacity (int largestBlockSamples) noexcept
{
    return 2 * ringCapacity (largestBlockSamples);
}

/** What an unresolved band reads: far below the floor, so a caller that forgets
    to ask `resolves` draws it off the bottom of any sensible scale rather than
    drawing a plausible low reading. */
inline constexpr double noReadingDb = -1000.0;

//==============================================================================
/** The analyser: windows a frame, transforms it, sums the bins into bands, and
    smooths the result with a sound level meter's ballistics.

    Not an audio thread object. It allocates in `configure`, and `push` runs a
    transform, so it belongs on whatever thread drives the display, fed from a
    ring the audio thread writes into. The engine uses `Tuner::InputRing` for
    exactly this, reused rather than reimplemented: that ring's tear check took
    two audits to get right and a second copy of it is a second chance to get
    it wrong.
*/
class Analyser
{
public:
    /** Message thread, allocates.

        `fftSize` must be a power of two, at least 64. Rejecting anything else
        rather than rounding it, as `RealFFT::setSize` does, because a
        transform quietly running at a different length puts every bin at the
        wrong frequency and so every band level on the wrong bins.

        Leaves the analyser unconfigured, and `push` refusing everything, if
        any argument is unusable.
    */
    void configure (int newFftSize, double newSampleRate, int newBandsPerOctave)
    {
        fftSize        = 0;
        sampleRate     = 0.0;
        bandsPerOctave = 0;
        bandList.clear();
        bandBins.clear();
        smoothedPower.clear();
        heldPower.clear();
        frames = 0;

        // The power of two half of this is said here and enforced below:
        // `RealFFT::setSize` refuses anything else and leaves itself unusable,
        // which the `getSize` check then catches, so removing this line
        // changes no behaviour. It is here because a reader deciding whether a
        // size is acceptable should not have to go and read another header to
        // find out, and because the minimum of 64 is this file's own and is
        // not enforced anywhere else.
        if (newFftSize < 64 || (newFftSize & (newFftSize - 1)) != 0)
            return;

        if (! (newSampleRate > 0.0) || newBandsPerOctave < 1)
            return;

        fft.setSize (newFftSize);

        if (fft.getSize() != newFftSize)
            return;

        fftSize        = newFftSize;
        sampleRate     = newSampleRate;
        bandsPerOctave = newBandsPerOctave;

        buildHannWindow (window, fftSize);
        windowSumOfSquares = 0.0;

        for (const auto w : window)
            windowSumOfSquares += w * w;

        scratch.assign ((std::size_t) fftSize, 0.0f);
        bins.assign ((std::size_t) (fftSize / 2 + 1), {});
        binPower.assign ((std::size_t) (fftSize / 2 + 1), 0.0);

        bandList = bands (bandsPerOctave, sampleRate);

        smoothedPower.assign (bandList.size(), 0.0);
        heldPower.assign (bandList.size(), 0.0);

        buildBandBins();
    }

    bool isConfigured() const noexcept { return fftSize > 0 && ! bandList.empty(); }

    int    getFftSize() const noexcept        { return fftSize; }
    double getSampleRate() const noexcept     { return sampleRate; }
    int    getBandsPerOctave() const noexcept { return bandsPerOctave; }

    const std::vector<Band>& getBands() const noexcept { return bandList; }
    int  getNumBands() const noexcept { return (int) bandList.size(); }

    /** How many frames have been accepted, which is what tells a display
        whether there is anything to draw at all. */
    long long getFrameCount() const noexcept { return frames; }

    /** The width of one FFT bin in hertz. */
    double binWidth() const noexcept
    {
        return fftSize > 0 ? sampleRate / (double) fftSize : 0.0;
    }

    /** Whether this band is wide enough for the transform to say anything
        about it.

        A band's total weight is its width divided by the bin width, so this is
        the question of whether the band is at least one bin wide. The 20 Hz one
        third octave band is 4.6044 Hz wide: at 48 kHz that is 1.57 bins at a
        transform size of 16384 and 0.20 bins at 2048.

        A band narrower than a bin either catches one whole bin or catches
        nothing, depending on where its edges happen to fall between bin
        centres, so its reading swings by tens of decibels for reasons that have
        nothing to do with the mix. Refused rather than approximated, and that
        is a deliberate choice with no source behind it: the precedent is
        `Tuner::InputRing`, where a ring too small to serve safely shows nothing
        rather than something wrong. A confident wrong number at 20 Hz is worse
        than a gap, because 20 Hz is the frequency people argue about most and
        the one nobody can check by ear.

        THE REFUSED BANDS ARE ALWAYS THE LOWEST ONES, so a display has one
        boundary rather than holes in it. A band's width is a fixed fraction of
        its centre and the bin width does not depend on frequency, so the width
        in bins grows monotonically across the display and the refusals are a
        prefix of it. Measured at every rate, size and bandwidth the studio
        offers, by `Tests/SpectrumProbe.cpp`. That matters because it says the
        stripes are the bottom edge of the display rather than a gap in the
        middle of it.

        WHAT A RESOLVED BUT STILL NARROW BAND DOES, because one bin is the
        threshold for showing a band at all and not the threshold for the
        reading being exact. Every figure below comes from that probe, over the
        whole matrix of rates, sizes and bandwidths. It is worth saying why the
        probe exists: the first version of this comment was measured at 48 kHz
        with the longest window and the one third octave band set, which is the
        only combination in which every band resolves, and three of the numbers
        it published were therefore wrong. An audit found them.

        A tone sitting on a join between TWO RESOLVED BANDS is shared between
        them and nothing is lost: each reads about 3 dB down and the two
        together are right. That part is inherent in any band display, at any
        width and any transform size, and no window length improves it.

        "Between two resolved bands" means three bins inside the resolved span
        rather than one band above the boundary, and the difference matters.
        The window spreads a tone about two bins either side, and a narrow band
        is a fraction of a bin wide, so a tone two or three bands above the
        boundary can still put a tail of itself into the stripes. Inside that
        margin the total over the display is the tone's own power to within
        0.02 dB; the test asserts it there and asserts the bounded loss
        everywhere else, rather than one loosened tolerance over both.

        A tone sitting on THE ONE BOUNDARY AT THE BOTTOM, where the band below
        is refused, is a different matter and this is the real cost of the
        threshold. Half its lobe lands in a band that is drawn as stripes, so
        it is not shared, it is gone from the display. Worst case measured:
        the resolved band reads 4.09 dB low, and the total over the whole
        display is 3.28 dB low, meaning 53 per cent of the tone is drawn
        nowhere. That is at 44.1 kHz with a 4096 window at one sixth octave,
        for a tone at 89 Hz. So the stripes do not only mean "no reading here",
        they also mean "and the bar next to me is reading low". The panel says
        so; a longer window moves the boundary down and the problem with it.

        Away from that boundary, the band's width decides how much of its
        interior is exact, and this is the part a longer window fixes. The
        periodic Hann spreads a tone over three bins, so a band of about that
        width cannot hold the whole main lobe even with the tone at its centre:

            width in bins      1.07   1.32   1.57   2.07   2.53   3.10   4.30
            error at centre   -2.71  -1.97  -1.15  -0.73  -0.36  -0.09  -0.01

        Each row is the worst case found at that width anywhere in the matrix,
        so the widths are the ones that actually occur rather than round
        numbers: 1.07 bins is the 200 Hz band at 88.2 kHz with a 2048 window,
        and nothing narrower than 1.57 bins exists at 48 kHz at all.

        So the worst a centred tone reads is 2.71 dB low and the worst a tone
        anywhere in a resolved band reads is 4.09 dB low. One bin is still
        where the refusal is drawn rather than four, and that is a trade rather
        than a result: refusing at four bins would blank the bottom two octaves
        at every practical window length, which is the whole bass range, to
        avoid a few decibels on isolated low tones. Below one bin there is
        nothing to trade, because the reading stops being a measurement of the
        band at all: it catches one whole bin or none depending on where the
        edges fall between bin centres.

        Broadband content, which is most of a mix, is unaffected at any width,
        because what smears out of a band smears in from its neighbour. All of
        the above is about isolated tones, which at the bottom of the spectrum
        is what a sustained bass note is.

        Raising the transform size is the remedy, and it is the one FabFilter
        documents for Pro-Q, whose help says a higher Resolution gives "more
        low-frequency precision ... but a lower update rate and slower attack".
    */
    bool resolves (int band) const noexcept
    {
        if (band < 0 || band >= (int) bandBins.size())
            return false;

        return bandBins[(std::size_t) band].totalWeight >= 1.0;
    }

    void setBallistics (Ballistics b) noexcept { ballistics = b; }
    Ballistics getBallistics() const noexcept  { return ballistics; }

    /** The display tilt, in dB per octave, pivoting at 1 kHz. Zero is the pink
        flat setting here: see the long comment at the top of this file, which
        is mostly about why. */
    void setTiltDbPerOctave (double t) noexcept
    {
        tilt = std::isfinite (t) ? t : 0.0;
    }

    double getTiltDbPerOctave() const noexcept { return tilt; }

    /** Feed one frame.

        `left` is required and `right` may be null, which is a mono source.
        `numSamples` must be exactly the transform size: a frame is a whole
        window and there is nothing sensible to do with a partial one.

        `frameSeconds` is the interval since the previous frame, which is what
        the ballistics integrate over. It is the caller's business because the
        caller is a timer rather than the audio clock, and getting it from the
        hop size would be a lie whenever a frame is dropped.

        Returns false, and changes nothing at all, if the analyser is
        unconfigured, the frame is the wrong length, the interval is not a
        positive finite number, or any sample is not finite. The last of those
        matters more than it looks: one NaN sample makes every band NaN, and
        because the smoother feeds on itself the display would then stay NaN
        forever. A single bad block would kill the analyser for the rest of the
        session with nothing to say why.

        TWO CHANNELS ARE COMBINED AS A SUM OF POWERS, not as a sum of signals.
        Four combinations are defensible and they span 6 dB: a time domain sum
        reads 6.02 dB above one channel on correlated material and cancels
        anti correlated material completely, the mid reads level and still
        cancels, the mean of the powers reads level, and the sum of the powers
        reads 3.01 dB up and cannot cancel. Logic's MultiMeter offers a "Mono"
        that is the time domain sum, so an out of phase bass guitar vanishes
        from it, which on a mix balance display sends somebody looking at the
        bass. Summing the powers cannot do that, and it is the same channel
        convention as ITU-R BS.1770, whose loudness sums per channel mean
        squares with a gain of one for L and R, so this display and
        `LoudnessMeter.h` agree about what "the mix" is instead of sitting 3 or
        6 dB apart for reasons nobody can reconstruct later.
    */
    bool push (const float* left, const float* right, int numSamples, double frameSeconds)
    {
        if (! isConfigured() || left == nullptr || numSamples != fftSize)
            return false;

        if (! (frameSeconds > 0.0) || ! std::isfinite (frameSeconds))
            return false;

        if (! allFinite (left, numSamples))
            return false;

        if (right != nullptr && ! allFinite (right, numSamples))
            return false;

        std::fill (binPower.begin(), binPower.end(), 0.0);

        accumulate (left);

        if (right != nullptr)
            accumulate (right);

        const double tau   = timeConstantSeconds (ballistics);
        const double alpha = 1.0 - std::exp (-frameSeconds / tau);
        const bool   first = frames == 0;

        for (std::size_t i = 0; i < bandList.size(); ++i)
        {
            const auto& map = bandBins[i];

            double power = 0.0;

            for (std::size_t j = 0; j < map.weight.size(); ++j)
                power += map.weight[j] * binPower[(std::size_t) map.firstBin + j];

            // Seeded on the first frame rather than ramped up from silence, so
            // that the display is right as soon as it appears instead of
            // climbing for a time constant. The exponential average is on the
            // MEAN SQUARE and not on the magnitude and not on the decibel
            // value: IEC 61672-1 clause 3.4 defines a time weighting as an
            // exponential function weighting the square of the instantaneous
            // pressure, and smoothing in either of the other two domains gives
            // a decay that depends on the level and matches no meter. The test
            // measures the decay against clause 5.7.2's acceptance windows,
            // which is the check that would catch that.
            smoothedPower[i] = first ? power
                                     : smoothedPower[i] + alpha * (power - smoothedPower[i]);

            heldPower[i] = std::max (heldPower[i], smoothedPower[i]);
        }

        ++frames;
        return true;
    }

    /** The band's smoothed level, in dB, with the display conventions applied:
        peak referenced so a full scale sine reads 0, and tilted if a tilt is
        set. `noReadingDb` for a band the transform cannot resolve, and
        `floorDb` for a band with nothing in it. */
    double levelDb (int band) const noexcept
    {
        if (! resolves (band))
            return noReadingDb;

        return toDisplayDb (smoothedPower[(std::size_t) band], band);
    }

    /** The highest level this band has reached since the last `resetHold`.

        Infinite hold with a manual reset, which is the most widely documented
        behaviour across the analysers read and has no standard behind it. */
    double holdDb (int band) const noexcept
    {
        if (! resolves (band))
            return noReadingDb;

        return toDisplayDb (heldPower[(std::size_t) band], band);
    }

    void resetHold() noexcept
    {
        std::fill (heldPower.begin(), heldPower.end(), 0.0);
    }

    /** Forgets every reading, so the next frame seeds rather than ramps. */
    void clear() noexcept
    {
        std::fill (smoothedPower.begin(), smoothedPower.end(), 0.0);
        std::fill (heldPower.begin(), heldPower.end(), 0.0);
        frames = 0;
    }

    /** The band a frequency falls in, or -1 for one outside the display. For a
        panel drawing a cursor readout. */
    int bandAt (double frequency) const noexcept
    {
        for (std::size_t i = 0; i < bandList.size(); ++i)
            if (frequency >= bandList[i].lower && frequency < bandList[i].upper)
                return (int) i;

        return -1;
    }

    /** The total weight a band's bins carry, which is its width in bins. For
        the test, and for a panel that wants to say why a band is blank. */
    double bandWeight (int band) const noexcept
    {
        if (band < 0 || band >= (int) bandBins.size())
            return 0.0;

        return bandBins[(std::size_t) band].totalWeight;
    }

private:
    /** Which bins a band holds, and how much of each.

        Stored as a first bin and a run of weights rather than a list of pairs,
        because a band's bins are always contiguous and the run is what the
        inner loop wants.
    */
    struct BandBins
    {
        int                 firstBin    = 0;
        double              totalWeight = 0.0;
        std::vector<double> weight;
    };

    static bool allFinite (const float* x, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
            if (! std::isfinite (x[i]))
                return false;

        return true;
    }

    /** Windows, transforms and adds this channel's single sided power per bin
        width into `binPower`.

        The quantity is the power in one bin's worth of bandwidth, so that a
        weighted sum across a band is the integral of the power density over
        that band, which is what a band level is. Written as

            2 * |X|^2 / (N * sum(w^2))

        which is the single sided density `2 * |X|^2 / (fs * sum(w^2))` times
        the bin width `fs / N`, with the sample rate cancelling. Equivalently it
        is the window sum normalisation divided by the equivalent noise
        bandwidth, and that header carries the measurement that says this is
        the right one of the two for a band display, and what the other one
        costs.

        `RealFFT::forward` is unscaled: its own comment states that a unit
        amplitude sine on bin k reads n/2 in magnitude, so with the window
        applied it reads `sum(w) / 2`.
    */
    void accumulate (const float* channel)
    {
        for (int i = 0; i < fftSize; ++i)
            scratch[(std::size_t) i] = (float) ((double) channel[i] * window[(std::size_t) i]);

        fft.forward (scratch.data(), bins.data());

        const double scale   = 2.0 / ((double) fftSize * windowSumOfSquares);
        const int    lastBin = fftSize / 2;

        for (int k = 0; k <= lastBin; ++k)
        {
            const double magnitudeSquared = std::norm (bins[(std::size_t) k]);

            // Bin 0 and the Nyquist bin have no conjugate twin in the half
            // spectrum, so the factor of two that folds the negative
            // frequencies back on does not apply to them.
            //
            // NEITHER OF THEM IS REACHED BY ANY BAND THIS DISPLAYS, so this
            // line changes no reading today: a mutation applying the mirror to
            // every bin produces identical output. It is written correctly
            // anyway because the property it relies on belongs to the band
            // selection rather than to this arithmetic, and a later change of
            // range or of rate could hand this a doubled direct current term
            // with nothing to notice. The test checks that no resolved band
            // reaches either bin, at every rate and transform size the studio
            // offers, so the claim is a measured one and will fail rather than
            // quietly stop being true.
            const double mirror = (k == 0 || k == lastBin) ? 0.5 : 1.0;

            binPower[(std::size_t) k] += mirror * scale * magnitudeSquared;
        }
    }

    /** The bin to band weights.

        Bin k stands for the band of frequencies half a bin either side of its
        centre, so it spans `[(k - 0.5) * df, (k + 0.5) * df)`. A bin's weight
        in a band is the fraction of that span lying inside the band's edges.

        Two properties this buys, and both are asserted in the test. Any one
        bin's weights across all bands sum to one, so no bin is counted twice
        and none is dropped at a boundary. And a band's total weight is exactly
        its width in bins, which is what `resolves` then reads: the refusal and
        the weighting are the same arithmetic rather than two rules that could
        drift apart.

        A hard include or exclude on the bin centre would be simpler and is
        what the obvious reading of "the bins inside the band" gives. It steps:
        a band spanning 1.4 bins catches one bin or two depending on where its
        edges land, so adjacent bands of equal width read levels that differ by
        the ratio of their bin counts, which at the low end is a factor of two.
    */
    void buildBandBins()
    {
        bandBins.assign (bandList.size(), {});

        const double df      = binWidth();
        const int    lastBin = fftSize / 2;

        if (! (df > 0.0))
            return;

        for (std::size_t i = 0; i < bandList.size(); ++i)
        {
            const auto& band = bandList[i];
            auto&       map  = bandBins[i];

            // The bins whose spans can touch this band at all. `first` can
            // exceed `last` for a band that falls entirely outside the half
            // spectrum, which leaves an empty run and a total weight of zero,
            // which `resolves` then refuses.
            int first = (int) std::ceil (band.lower / df - 0.5);
            int last  = (int) std::floor (band.upper / df + 0.5);

            // NEITHER CLAMP CAN FIRE, and both stay. An earlier version of
            // this comment claimed the first one was load bearing and gave a
            // worked example at 192 kHz; the worked example disproves the
            // claim and an audit noticed. Worth leaving on the record, because
            // a comment asserting the opposite of what the code does is the
            // fault this project meets most often, and here it was in a
            // comment about a safety check, which is the worst place for it.
            //
            // `first` cannot go negative. `band.lower` is positive, so
            // `band.lower / df - 0.5 > -0.5`, and the ceiling of anything
            // above -0.5 is zero or more. At 192 kHz with the shortest
            // transform size, which is the narrowest case the studio has, the
            // bins are 93.75 Hz wide against a bottom band edge of 17.78, and
            // the argument is -0.3103, whose ceiling is zero.
            //
            // `last` cannot exceed `lastBin`. No band is allowed above
            // Nyquist, so `band.upper <= lastBin * df`, so
            // `floor (band.upper / df + 0.5) <= floor (lastBin + 0.5)`, which
            // is `lastBin`.
            //
            // They stay because both of them are the difference between a
            // changed band selection and an out of bounds read, and because
            // nobody should have to re-derive either argument to be sure the
            // read is in range. A mutation removing either changes nothing
            // today, and the mutation record says so rather than claiming a
            // gap in the test.
            first = std::max (0, first);
            last  = std::min (lastBin, last);

            map.firstBin = first;

            for (int k = first; k <= last; ++k)
            {
                const double binLower = ((double) k - 0.5) * df;
                const double binUpper = ((double) k + 0.5) * df;

                const double overlap = std::min (band.upper, binUpper)
                                     - std::max (band.lower, binLower);

                // The guard is defensive rather than reached. `first` is the
                // lowest bin whose upper span is at or above the band's lower
                // edge and `last` the highest whose lower span is at or below
                // its upper edge, so the overlap over that run is never
                // negative, at worst exactly zero at either end. It is here so
                // that a change to how the run is chosen cannot start
                // subtracting weight from a band, which would read as a hole
                // in the spectrum rather than as an error.
                const double w = overlap > 0.0 ? overlap / df : 0.0;

                map.weight.push_back (w);
                map.totalWeight += w;
            }
        }
    }

    double toDisplayDb (double power, int band) const noexcept
    {
        if (! (power > 0.0))
            return floorDb;

        const double centre = bandList[(std::size_t) band].centre;

        double db = 10.0 * std::log10 (power) + fullScaleReferenceDb;

        if (tilt != 0.0 && centre > 0.0)
            db += tilt * std::log2 (centre / tiltPivotFrequency);

        return std::max (floorDb, db);
    }

    RealFFT                            fft;
    int                                fftSize        = 0;
    double                             sampleRate     = 0.0;
    int                                bandsPerOctave = 0;

    std::vector<double>                window;
    double                             windowSumOfSquares = 0.0;
    std::vector<float>                 scratch;
    std::vector<std::complex<double>>  bins;
    std::vector<double>                binPower;

    std::vector<Band>                  bandList;
    std::vector<BandBins>              bandBins;
    std::vector<double>                smoothedPower;
    std::vector<double>                heldPower;

    Ballistics                         ballistics = Ballistics::fast;
    double                             tilt       = 0.0;
    long long                          frames     = 0;
};

} // namespace Spectrum
