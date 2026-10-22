#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

//==============================================================================
/** What word length an export is written at, and the dither that makes the
    reduction to it honest.

    The studio rendered its mix in 32 bit float and wrote a 24 bit file, with
    no choice about either. Getting a 16 bit file, which is what a CD is and
    what most places asking for a master ask for, meant converting it
    somewhere else: an extra step, and a conversion this studio then knows
    nothing about.

    Dither is the half of that nobody sees until it is wrong. Turning a float
    mix into a fixed point file quantises it, and an undithered quantiser's
    error is not noise: it is a deterministic function of the signal, so it
    arrives as distortion rather than hiss and it follows the music. Measured,
    a 1 kHz sine at 4 LSB has its fifth and seventh harmonics at -26 and -31 dB
    below the fundamental undithered, and below the measurement floor dithered.
    The error's variance also wanders by 3.6 dB as a sine fades, which is what
    makes an undithered fade out or reverb tail grainy in a way that gets worse
    the quieter it gets.

    That is the shape of fault this project keeps meeting. Nothing errors, the
    meters read right, and the tails are wrong.

    The dither here is nonsubtractive triangular dither of 2 LSB peak to peak,
    which is what Lipshitz, Wannamaker and Vanderkooy recommend by name in the
    conclusions of "Quantization and Dither: A Theoretical Survey", JAES vol
    40, no 5, pp 355 to 375 (1992 May): "We recommend the use of
    nonsubtractive, triangular-pdf dither of 2-LSB peak-to-peak amplitude for
    most audio applications requiring multibit quantization or requantization
    operations."

    Three things from that paper are load bearing here rather than decorative.

    **Two summed draws, not one, and not three.** Its Theorem 8 says dither
    formed by summing `n` independent zero mean rectangular processes each 1
    LSB peak to peak renders the first `n` moments of the total error
    independent of the input, with a total error power of `(n + 1) q^2 / 12`.
    One draw flattens the mean and leaves the variance following the signal,
    which the paper names noise modulation and calls "audibly undesirable".
    Two flattens both, and the paper calls 2 LSB triangular dither "unique and
    optimal in the sense that it renders the first and second moments of the
    total error input independent, while minimizing the second moment". Three
    would cost another 1.25 dB of noise to flatten a third moment nobody hears.

    **What it does not do**, because the overclaim is easy and this header is
    where somebody will read it. Section 5.4 item (1): nonsubtractive dither
    cannot make the total error independent of the input, only chosen moments
    of it. So the claim is that the first and second moments are independent of
    the signal. It is not that the error is independent of the signal, and it
    is not that the error is noise uncorrelated with it.

    **The quantiser has to round to nearest.** Section 2: "Quantizers of the
    midtread variety will be assumed", built on the floor operator, with the
    error having "a maximum magnitude of 0.5 LSB" and a period of 1 LSB. That
    is `floor(x + 1/2)`. The paper never discusses truncation, and truncation
    toward zero is not a uniform quantiser at all: it has a two step dead zone
    around zero, so the periodicity the theorems rest on does not hold and
    dithering into one buys the noise and none of the linearisation.

    That last point is why the quantiser is in here rather than left to the
    file writer. JUCE's `writeFromFloatArrays` rounds to int32 and then
    narrows with an arithmetic right shift, `(uint16) (v >> 16)` at 16 bit and
    `v >> 8` at 24, which is floor division: measured, a mean error of -0.4997
    LSB, so every export this studio has written carries half an LSB of DC.
    Inaudible at 24 bit, at -144 dBFS, which is why nothing ever said so. It
    would also have spoiled the dither rather than defeating it outright, which
    is worse to diagnose: floor is still a uniform quantiser, so the noise
    modulation would still have been fixed and the constant first moment would
    have been -0.5 LSB instead of zero. A flat noise floor with a DC offset
    underneath it.

    So the code is computed here and handed over already quantised, through
    `leftJustified`, which puts it where JUCE's own shift will put it back
    exactly. See `ExportJob.h`.

    No JUCE, so all of it compiles and runs under the sanitizers on its own.
    See `Tests/DitherTest.cpp`, which checks the properties the thing exists to
    produce against the paper's constants rather than against these formulas.
*/
namespace Dither
{
    /** The word length an export is written at.

        Three, because they are three different jobs rather than a range. 16
        bit is what a CD is and what a delivery usually asks for. 24 bit is a
        working master, and is the default because it is what every export up
        to now was written at, so nothing anybody is in the middle of changes.
        32 bit float is the mix exactly as the engine computed it, for handing
        on to somebody who is going to work on it further.

        The studios offer much the same set: Logic's PCM bounce offers 8, 16
        and 24, Pro Tools offers 16, 24 and 32 bit float, and Cubase adds 32
        and 64 bit to those. 8 bit is left out as a format nobody delivers in
        any more.
    */
    enum class Depth
    {
        int16,
        int24,
        float32
    };

    /** Whether a word length reduction is dithered.

        Two entries, and rectangular dither is deliberately not one of them.
        Every implementation's menu offers it and it is a worse answer rather
        than a cheaper one: the 1992 survey's section 5.3 says 1 LSB
        rectangular dither leaves the error variance "clearly signal
        dependent", and measured over a DC sweep across one step its variance
        swings from nothing to `q^2/4` and back, following the signal. Its one
        distinguishing property is a fault, so there is nothing to choose it
        for.

        `none` is here because dithering something twice is worse than not
        dithering it, and the studios say so: Logic's own bounce page warns to
        "avoid dithering the same audio signal multiple times". Somebody
        printing a mix that is going to be mastered elsewhere wants this off.
    */
    enum class Mode
    {
        none,
        triangular
    };

    inline constexpr int numDepths = 3;
    inline constexpr int numModes  = 2;

    // 24 bit and dithered: the depth every export up to now was written at, so
    // no habit changes, and the dither on by default because the correct
    // behaviour is the one that should not have to be found. At 24 bit the
    // policy below turns it off anyway, so the pair means "16 bit when asked
    // for, dithered" rather than noise on a master that does not need it.
    inline constexpr Depth defaultDepth = Depth::int24;
    inline constexpr Mode  defaultMode  = Mode::triangular;

    inline constexpr Depth depthAt (int index) noexcept
    {
        switch (index)
        {
            case 0:  return Depth::int16;
            case 1:  return Depth::int24;
            default: return Depth::float32;
        }
    }

    inline constexpr int depthToStored (Depth d) noexcept
    {
        switch (d)
        {
            case Depth::int16:   return 0;
            case Depth::int24:   return 1;
            case Depth::float32: return 2;
        }

        return 1;   // unreachable, and the index of the default if it were not
    }

    inline constexpr Depth depthFromStored (int stored) noexcept
    {
        if (stored < 0 || stored >= numDepths)
            return defaultDepth;

        return depthAt (stored);
    }

    inline constexpr Mode modeFromStored (int stored) noexcept
    {
        if (stored < 0 || stored >= numModes)
            return defaultMode;

        return stored == 0 ? Mode::none : Mode::triangular;
    }

    inline constexpr int modeToStored (Mode m) noexcept
    {
        return m == Mode::none ? 0 : 1;
    }

    inline constexpr const char* name (Depth d) noexcept
    {
        switch (d)
        {
            case Depth::int16:   return "16 bit";
            case Depth::int24:   return "24 bit";
            case Depth::float32: return "32 bit float";
        }

        return "24 bit";
    }

    inline constexpr const char* name (Mode m) noexcept
    {
        return m == Mode::none ? "no dither" : "triangular dither";
    }

    /** How many bits a sample of this depth occupies in the file. */
    inline constexpr int bitsFor (Depth d) noexcept
    {
        switch (d)
        {
            case Depth::int16:   return 16;
            case Depth::int24:   return 24;
            case Depth::float32: return 32;
        }

        return 24;
    }

    /** Whether writing at this depth reduces the word length of the mix at
        all, which is the only thing dither has any business being near. */
    inline constexpr bool isFixedPoint (Depth d) noexcept
    {
        return d != Depth::float32;
    }

    /** The dither a depth actually gets, which is not always the one asked
        for.

        Only 16 bit is dithered. That is Ardour's policy exactly, and in
        Ardour's case it is structural rather than a default:
        `HasSampleFormat::update_sample_format_selection` force selects `None`
        and marks every other dither type incompatible for 24 bit, 32 bit,
        float and double, so dither is selectable only at 8 and 16, and
        `SampleFormatConverter<float>::init` hard codes `GDitherNone` so a
        float export cannot be dithered at all. Its tracker 0007428, asking
        for dither on 24 bit exports, is closed won't fix on the grounds that
        24 bit covers about 144 dB and thermal and analogue noise sit well
        above that floor.

        The other studios draw the same line from the other end rather than
        disagreeing: Cubase's Export Audio Mixdown page recommends dither for
        the 16 bit case specifically and its Dither Effects page puts the
        audible range of the problem at "16 bit or below", and Logic's bounce
        page says dither "is recommended when bouncing 24-bit recordings into
        16-bit files". Avid is the one source that contradicts itself, its
        "When to Use a Dither Plug-In" page saying 16 or 24 bit while its own
        DigiRack Dither page says lower than 24 bit. See Research/BACKLOG.md.

        A policy and not an opinion, so it is a function and it is tested: a
        user interface that merely greyed the control would leave the engine
        free to disagree with it.
    */
    inline constexpr Mode modeFor (Depth d, Mode asked) noexcept
    {
        return d == Depth::int16 ? asked : Mode::none;
    }

    /** How many codes there are to one unit of full scale, which is also one
        over the quantiser's step.

        2^(bits - 1), so full scale negative is exactly the lowest code and
        full scale positive is one step above the highest. That asymmetry is
        the ordinary one every fixed point format has, and matching it matters
        here for a specific reason: it is what JUCE's own integer path
        produces, `INT_MIN >> 16` being -32768 and `INT_MAX >> 16` being 32767,
        so moving the export from that path to this one does not move either
        rail by an LSB.
    */
    inline constexpr double scaleFor (Depth d) noexcept
    {
        return (double) (1u << (bitsFor (d) - 1));
    }

    /** Negated in unsigned and converted afterwards, rather than written as a
        negation of the positive value. At 32 bit the positive value is one
        past the top of an int, so negating it after the conversion would be
        negating the most negative integer, which is undefined. Unreachable
        through the export, which only quantises at 16 and 24, and left safe
        anyway because a function that is only correct for the arguments
        somebody currently passes it is a trap for whoever passes it another
        one.
    */
    inline constexpr int lowestCode (Depth d) noexcept
    {
        return (int) (0u - (1u << (bitsFor (d) - 1)));
    }

    /** Reduced in unsigned before the conversion, for the reason above: at 32
        bit, converting first lands on the most negative integer and taking one
        off that overflows. The sanitizers caught this one, which is the
        argument for running them over the arithmetic.
    */
    inline constexpr int highestCode (Depth d) noexcept
    {
        return (int) ((1u << (bitsFor (d) - 1)) - 1u);
    }

    /** One step of SplitMix64.

        The paper specifies the dither's distribution and says nothing about
        how to generate it, so this is a decision. SplitMix64 is used because
        it needs no warm up, carries one word of state per channel and is a
        few instructions, and because the test checks the density that comes
        out rather than taking the generator's word for it.
    */
    inline std::uint64_t nextRandom (std::uint64_t& state) noexcept
    {
        state += 0x9e3779b97f4a7c15ull;

        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    /** A draw uniform on [-1/2, +1/2), in units of one quantiser step.

        53 bits, which is a double's mantissa, so the result is uniform over
        representable values rather than over a coarser grid that would show up
        as structure in the dither.
    */
    inline double uniformDraw (std::uint64_t& state) noexcept
    {
        return (double) (nextRandom (state) >> 11) * 0x1.0p-53 - 0.5;
    }

    /** A draw with a triangular density on [-1, +1), in units of one
        quantiser step: two independent uniform draws added.

        Added, and not the difference of successive draws, and the distinction
        is worth the comment because the two are almost impossible to tell
        apart from the outside. Differencing gives the same triangular marginal
        density, the same variance of `q^2/6`, the same flat first and second
        moments and the same total error power, and it satisfies Theorem 8 just
        as well. What it does not give is a flat spectrum: a first difference
        is a high pass, so the error ends up at `q^2/12 + 4 sin^2(pi f / fs)
        q^2/12`, which measures 4.8 dB quieter below 1 kHz and 2.2 dB louder
        near Nyquist. That is a real and free improvement and it is what Ardour
        ships under the name "Triangular", but its primary source, Lipshitz and
        Vanderkooy's "High-Pass Dither", AES 4th Regional Convention, Tokyo
        (1989 June), could not be obtained, so it is not what is built here.

        The only check that tells the two apart is the one on the spectrum.
        Everything else passes for both, which is why `DitherTest` has it.
    */
    inline double triangularDraw (std::uint64_t& state) noexcept
    {
        return uniformDraw (state) + uniformDraw (state);
    }

    //==========================================================================
    /** One channel's word length reduction: the dither and the quantiser.

        One per channel per file, each with its own stream, because two
        channels sharing a dither stream put the same noise on both and image
        it in the centre of the stereo field instead of spreading it. Ardour
        keeps per channel state for the same reason, though it draws every
        channel from one global generator, which is adequate and is not the
        same thing.
    */
    class Quantiser
    {
    public:
        Quantiser (Depth depth, Mode asked, std::uint64_t seed) noexcept
            : scale   (scaleFor (depth)),
              lowest  (lowestCode (depth)),
              highest (highestCode (depth)),
              mode    (modeFor (depth, asked)),
              state   (seed)
        {
        }

        /** The integer code this sample is written as.

            Clipping is a decision rather than a citation, because the theory
            excludes it: the survey's footnote 1 says its results assume "the
            signal is never clipped by saturation of the quantizer" and its
            text assumes "infinite quantizers". Every theorem in it is about a
            quantiser with infinitely many levels, and no manufacturer
            documentation read addresses overload either.

            Clamped to the extreme code, with no headroom reserved. Both
            reference implementations clamp: Ardour's gdither clamps after
            `lrintf`, and JUCE clamps the float to plus or minus one before
            scaling, so an export clamps twice over whatever happens here. No
            headroom, because the dither can push a sample past full scale by
            at most 1 LSB, which at 16 bit is 0.00027 dB, and reserving room
            for it would make a bounce quieter than what was heard, which is
            the fault `renderOffline` exists to prevent.

            The honest qualifier: a clamped sample is not a uniform quantiser,
            so the moment independence does not hold at the rails. That is
            unavoidable in any finite word length and is exactly why the paper
            assumes an infinite one. It only bites on material already pinned
            at full scale, which has a louder problem than its dither.
        */
        int code (float sample) noexcept
        {
            // A plugin can hand back a NaN or an infinity, and converting
            // either to an integer is undefined rather than merely wrong. The
            // mix is silent at that sample instead, which is both defined and
            // the least harmful thing available: carrying it into the file
            // would put the extreme code there and the loudest possible click
            // with it.
            const double x = std::isfinite (sample) ? (double) sample * scale : 0.0;

            // Mid tread, the floor form the paper assumes, which is round to
            // nearest with ties going up.
            //
            // Deliberately not lrint. That follows the current floating point
            // rounding mode, so it ties to even by default, and a quantiser
            // whose answer depends on a mode nothing in this studio sets would
            // make the same mix export differently on the three platforms it
            // is built for. With dither a tie has measure zero, so this only
            // decides the undithered case, and deciding it is the point.
            const double dithered = mode == Mode::none ? x : x + triangularDraw (state);
            const double rounded  = std::floor (dithered + 0.5);

            // Clamped in double before the conversion rather than after it:
            // past about 2^31 the cast itself is undefined, and full scale
            // float times 2^23 is nowhere near that but a stray large sample
            // is.
            return (int) std::clamp (rounded, (double) lowest, (double) highest);
        }

        Mode dither() const noexcept { return mode; }

    private:
        double scale;
        int    lowest, highest;
        Mode   mode;
        std::uint64_t state;
    };

    /** A code moved into the top of a 32 bit word, which is the form JUCE's
        integer writer takes.

        `AudioFormatWriter::write (const int**, n)` is documented as taking
        samples spanning the full 32 bit range, and the WAV writer narrows them
        with `(uint16) (v >> 16)` at 16 bit and `v >> 8` at 24. An arithmetic
        right shift undoes this shift exactly, for every code in range and with
        no floating point anywhere in between, which is the whole reason the
        export goes this way round rather than through
        `writeFromFloatArrays`.

        Done in unsigned and cast back rather than shifted as a signed value,
        so there is no question about a left shift of a negative number for a
        sanitizer to raise.
    */
    inline int leftJustified (int code, Depth d) noexcept
    {
        const int shift = 32 - bitsFor (d);
        return (int) ((std::uint32_t) code << shift);
    }
}
