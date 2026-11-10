#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <string>

//==============================================================================
/** The time signature: how many beats are in a bar and which note is a beat.

    The studio was 4/4 everywhere and hard coded, and the hard coding was the
    smaller half of the problem. `SongLength.h` carried `kBeatsPerBar = 4.0`,
    the MIDI export wrote a fixed 4/4, and roughly twenty call sites divided or
    multiplied by a literal `4.0` to get from a beat to a bar. A song in 3/4 or
    6/8 played, because playback never asks what a bar is, and could not be
    written, because every bar line, click, snap and position readout was
    wrong.

    ## The one rule everything else follows from

    **The denominator does not touch the tempo.** The tempo is quarter notes
    per minute whatever the signature says, so in 6/8 at 120 bpm a bar is three
    quarter notes and 1.5 seconds, not 3.0. Apple states it outright, which is
    the only explicit manufacturer statement there turned out to be:

        "The tempo indicator in the LCD always relates to quarter notes, even
         if eighth notes are chosen as the denominator for the time signature."

        "Changing the time signature does not affect project playback, but does
         determine the time grid in the Tracks area and MIDI editors."

    (Logic Pro User Guide, *Set the project time signature*.) Steinberg's own
    VST 3 `ProcessContext` expresses every musical quantity in quarter notes
    and carries the signature beside them as separate information, and Ardour's
    `Temporal::Tempo` holds a note type independent of the meter denominator
    whose documented default is "Note Type (default '4': quarter note)".

    So: **the denominator may not appear in any conversion to seconds.**
    Seconds per stored beat stays `60 / bpm` with nothing else in it. If the
    denominator turns up in a seconds conversion anywhere, that code is wrong,
    and `MeterTest.cpp` sweeps every accepted signature asserting it has not.

    Two studios do something else and neither is a counterexample. Pro Tools
    has a per tempo event "Tempo Resolution (BPM Rate)" which, following a
    dotted quarter click in 6/8, makes 120 bpm mean 120 dotted quarters: a
    display and entry reference rather than a different time base. Ardour
    counted denominator units before 2008, in Paul Davis' own words on their
    forum, and changed it. Cubase, Reaper and Live publish no answer to the
    question at all, which is recorded here so nobody re-reads three manuals
    looking for one.

    ## Two things are called a beat and they are different

    This is where the feature goes wrong if it goes wrong, so the words are
    kept apart throughout this header:

    - A **quarter** is the unit the project stores every position in. Linear,
      signature independent, and what the tempo counts.
    - A **display beat** is the denominator unit, which is what a bar|beat
      readout counts and what the metronome clicks. Logic, again explicitly:
      "A beat corresponds to the denominator in the time signature." So 6/8
      reads 1.1 to 1.6, 7/8 reads 1.1 to 1.7, and 5/4 reads 1.1 to 1.5.

    Ardour names both coordinates in `temporal/tempo.h` and keeps them apart
    deliberately, with the warning that matters:

        "BEAT : a fraction of a PULSE. Defined by the meter in effect, so
         requires meter (time signature) information to convert to/from PULSE
         or QUARTER NOTES. In a 5/8 time, a BEAT is 1/8th note. In a 4/4 time,
         a beat is quarter note. This means that measuring time in BEATS is
         potentially non-linear (if the time signature changes, there will be a
         different number of BEATS corresponding to a given time in any other
         unit)."

    Storing positions in quarters rather than in display beats is what makes a
    mid song signature change an overlay on the file rather than a migration of
    it: inserting one changes no stored number. Nothing in here, and nothing
    that calls it, may store a position in display beats.

    ## Nothing in here has a no-argument accessor, on purpose

    Every studio looked at lets the signature change mid song, with a primary
    citation for each, so one signature for the whole project is not the
    finished feature. The map is the next night's work, and the expensive part
    of this one is the call sites, which are the same call sites either way.

    So `Map` is a signature map that today holds one entry, and everything on
    it takes a position in quarters and ignores it. The argument costs nothing
    now and is the whole migration later: a `barLengthQuarters()` with no
    argument would make every call site a site that has to be found again, and
    the ones that were missed would be silently wrong rather than failing to
    compile.

    The entry itself is fixed at bar 1, which is Cubase's shape: "The first
    signature event is always positioned at bar 1. You cannot change this."

    **One call site is not covered by that argument and it is the hard one.**
    `AudioEngine::audioDeviceIOCallbackWithContext` cannot read a map at all:
    it runs on the audio thread, where it may not allocate, lock or follow a
    pointer that the message thread could be rewriting. So it latches one
    signature from two atomics at the top of each block and uses the free
    functions. A mid song map will need a lock free handoff of its own, the way
    the arrangement already gets one, and the accessors below will not save it
    that work. Said here rather than left for whoever meets it, because an
    argument that quietly does not cover its hardest case is worse than no
    argument.

    ## What is accepted

    Denominators 1, 2, 4, 8, 16 and 32, numerators 1 to 99. That is the set
    Ableton publishes ("a denominator (beat value) of 1, 2, 4, 8 or 16"),
    widened by 32 because Ardour's manual uses 27/32 as an example, and the
    numerator bound Ableton and Avid both publish.

    Non powers of two are refused, and the reason is external rather than a
    preference: the Standard MIDI File denominator byte is a base two
    logarithm, so a 4/3 meter has no byte to write. Accepting one in the
    project would mean exporting a file that said something else, which is the
    worst outcome available.

    No JUCE in here, so Tests/MeterTest.cpp builds it on its own.
*/
namespace Meter
{

/** The numerator bounds Ableton and Avid both publish. */
inline constexpr int kMinNumerator = 1;
inline constexpr int kMaxNumerator = 99;

/** MIDI's timing clock rate, from the MIDI 1.0 specification: "TIMING CLOCK:
    Clock-based MIDI systems are synchronized with this message, which is sent
    at a rate of 24 per quarter note." The time signature meta event's third
    byte is in these units always, whatever the file's own tick division is.
*/
inline constexpr int kMidiClocksPerQuarter = 24;

/** The time signature meta event's fourth byte: "the number of notated
    32nd-notes in a MIDI quarter-note". Eight is the truthful answer whenever a
    MIDI quarter note is notated as a quarter note, which here it always is.

    The byte exists so that a file can say otherwise: "This was added because
    there are already multiple programs which allow a user to specify that what
    MIDI thinks of as a quarter-note (24 clocks) is to be notated as, or
    related to in terms of, something else." Nothing here does that.
*/
inline constexpr int kMidiNotated32ndsPerQuarter = 8;

/** Which denominators can be accepted. Powers of two from a whole note to a
    thirty second, which is the range the MIDI byte can express and the range
    the studios publish.
*/
inline bool isDenominatorAllowed (int denominator) noexcept
{
    return denominator == 1 || denominator == 2 || denominator == 4
        || denominator == 8 || denominator == 16 || denominator == 32;
}

//==============================================================================
/** One signature, as it would be notated. */
struct Signature
{
    int numerator   = 4;
    int denominator = 4;

    bool operator== (const Signature& o) const noexcept
    {
        return numerator == o.numerator && denominator == o.denominator;
    }
    bool operator!= (const Signature& o) const noexcept { return ! (*this == o); }
};

/** Four four, which is also what a MIDI file with no signature event means. */
inline constexpr Signature kDefault { 4, 4 };

inline bool isValid (const Signature& s) noexcept
{
    return s.numerator >= kMinNumerator && s.numerator <= kMaxNumerator
        && isDenominatorAllowed (s.denominator);
}

/** Anything that cannot be accepted becomes 4/4 rather than being clamped.

    Clamping would turn 4/3 into 4/2 or 4/4 depending on which way it rounded,
    and either is a signature the person did not ask for arriving without a
    word. Falling back to the default is just as wrong and is at least the one
    wrong answer that is obvious on sight, and it is what a loaded project with
    a corrupt field should show. Edits are refused at the boundary instead, so
    this is the file format's fallback rather than the editor's.
*/
inline Signature validated (const Signature& s) noexcept
{
    return isValid (s) ? s : kDefault;
}

//==============================================================================
/** How long one display beat is, in quarters: a quarter in 4/4, an eighth in
    6/8, a whole note in 4/1.
*/
inline double beatLengthQuarters (const Signature& s) noexcept
{
    return 4.0 / (double) s.denominator;
}

/** How long a bar is, in quarters.

    Exact for everything accepted: the denominator is a power of two so
    `4 / denominator` is exact in binary, and a numerator up to 99 times it
    is exact too. Nothing downstream needs a tolerance to land on a bar line,
    which is why `barOf` and `barBeatAt` can use plain floors.
*/
inline double barLengthQuarters (const Signature& s) noexcept
{
    return (double) s.numerator * beatLengthQuarters (s);
}

//==============================================================================
/** A position as a bar|beat readout, with the fraction kept so that the round
    trip back to quarters is exact.

    `bar` is one based, so quarter 0 is bar 1 beat 1. It can go below one: the
    count-in locates the transport before the start of the song, and a bar
    before bar 1 is bar 0.

    `beat` is one based and in `1 .. numerator`, counting display beats, which
    is the denominator unit. `fraction` is how far through that beat the
    position is, in `[0, 1)`.

    What the third field of a position readout should be has no common
    convention: Logic uses a user chosen division note value with ticks below
    it, Live uses sixteenths, Ardour uses ticks referenced to the quarter note.
    So this carries the fraction and leaves the choice of what to show of it to
    whoever is drawing it.
*/
struct BarBeat
{
    long long bar  = 1;
    int       beat = 1;
    double    fraction = 0.0;
};

/** How far a position may be from zero and still have a bar number.

    A bar number is a `long long`, and the cast from a double that does not fit
    in one is undefined behaviour rather than a large number. 1e300 quarters is
    not a musical position, but `ProjectIO` loads a clip's start with a floor of
    zero and no ceiling, so a hand edited or damaged project can carry one as
    far as the position readout. A bound of 2^53 quarters is longer than the
    age of the universe at any tempo and is exactly representable.
*/
inline constexpr double kMaxQuarters = 9007199254740992.0;   // 2^53

/** Bar number at a position, one based, floored.

    Anything that is not a position reads as bar 1: a NaN, an infinity, and a
    finite number too large to have a bar number. These are public functions
    taking a bare double, so what reaches them is whatever a caller has, and
    the cast to `long long` of a double that does not fit in one is undefined
    behaviour rather than a large answer. Nothing in the studio is known to
    produce such a position today; the guard is here because a function that
    can be handed one has to say what it does with it, not because a path to it
    has been found.
*/
inline long long barOf (const Signature& s, double quarters) noexcept
{
    if (! std::isfinite (quarters) || std::abs (quarters) > kMaxQuarters)
        return 1;

    return (long long) std::floor (quarters / barLengthQuarters (s)) + 1;
}

/** The position, in quarters, where a bar begins. Bar 1 begins at zero. */
inline double barStartQuarters (const Signature& s, long long bar) noexcept
{
    return (double) (bar - 1) * barLengthQuarters (s);
}

inline BarBeat barBeatAt (const Signature& s, double quarters) noexcept
{
    if (! std::isfinite (quarters) || std::abs (quarters) > kMaxQuarters)
        return {};

    BarBeat r;
    r.bar = barOf (s, quarters);

    const double raw = (quarters - barStartQuarters (s, r.bar)) / beatLengthQuarters (s);

    // Held inside the bar before it is split into a beat and a fraction, so
    // that both come out in range rather than only one of them.
    //
    // It is unreachable for a signature the studio accepts, because every
    // accepted display beat length is a power of two and the bar was found by
    // flooring the same exact division, so the clamp is an identity there and
    // the round trip stays exact. It is reachable for a signature nobody
    // validated, and these are free functions taking a struct with public
    // fields, so a caller can build 11/13: a display beat is then not exactly
    // representable, and at quarter 192.923... the division reads
    // 11.000000000000007 beats into an eleven beat bar.
    //
    // Clamping only the index, which is what this did first, left the fraction
    // at 1.000000000000007: a beat in range and a fraction out of it, which is
    // worse than either, because the beat looks right. `MeterTest` sweeps the
    // unaccepted signatures and found exactly that.
    const double span = (double) std::max (1, s.numerator);
    const double into = std::max (0.0, std::min (std::nextafter (span, 0.0), raw));

    // Now provably in 0 .. numerator - 1, since `into` is below `span`.
    const long long index = (long long) std::floor (into);

    r.beat     = (int) index + 1;
    r.fraction = into - (double) index;
    return r;
}

/** The inverse. Out of range fields are brought into range rather than
    refused, because this is what a position field's text becomes and "bar 2
    beat 9" in 4/4 is a typo rather than a position.
*/
inline double quartersAt (const Signature& s, const BarBeat& p) noexcept
{
    const int beat = std::max (1, std::min (s.numerator, p.beat));
    const double fraction = std::isfinite (p.fraction) ? std::max (0.0, std::min (1.0, p.fraction)) : 0.0;

    return barStartQuarters (s, p.bar)
         + ((double) (beat - 1) + fraction) * beatLengthQuarters (s);
}

//==============================================================================
/** The metronome.

    One click per display beat, with the first beat of the bar accented and
    nothing else. **No manufacturer publishes a default click pattern for a
    compound meter**, so this is a choice rather than a standard, and it is the
    choice with the most published company: Cubase's documented 4/4 default is
    one click per numerator unit where "the click level of the first click is
    high, while the level of the other beats is low", and Pro Tools states
    generally that "the accented note sounds on the first beat of each
    measure" with a single accented and unaccented pair and no per beat
    pattern at all.

    What this deliberately is not: the compound pulse. In 6/8 the click here
    sounds six times a bar, not twice on the dotted quarters. Logic's answer is
    the one to copy when somebody wants that, and the shape of it is the
    important part: its `Group` checkbox "For compound and composite
    signatures, generates note events for each group" is a **separate,
    additional** click layer with its own note and velocity, not a different
    click rate. Adding it later must not change this rate.
*/
inline long long clickIndexAt (const Signature& s, double quarters) noexcept
{
    if (! std::isfinite (quarters) || std::abs (quarters) > kMaxQuarters)
        return 0;

    return (long long) std::floor (quarters / beatLengthQuarters (s));
}

/** Whether the click at an index is the accented one.

    A click index is a multiple of the numerator exactly at a bar line, going
    backwards as well: the count-in runs at negative positions, and bar 0 of a
    7/8 project begins at click -7. Testing against zero rather than comparing
    a remainder is what makes that work without a floored modulo, since C++
    gives `-7 % 7 == 0`.
*/
inline bool isAccentedClick (const Signature& s, long long clickIndex) noexcept
{
    return clickIndex % (long long) std::max (1, s.numerator) == 0;
}

/** Rounds a position down to the bar line at or before it. */
inline double barFloorQuarters (const Signature& s, double quarters) noexcept
{
    if (! std::isfinite (quarters))
        return 0.0;

    const double bar = barLengthQuarters (s);
    return std::floor (quarters / bar) * bar;
}

//==============================================================================
/** A snap size, as a fraction of a display beat, and the sentinel for a whole
    bar.

    The editors offer Bar, Beat, halves and thirds of a beat, and steps, which
    are quarters of a beat. Those names are all relative to a display beat, so
    they are stored that way and multiplied up here: in 6/8 a beat to snap to
    is an eighth, and a step is a sixteenth of the bar rather than a sixteenth
    note. In 4/4 every one of them comes out at the number it was before the
    signature existed, which is what makes this safe to change under every
    saved project.

    A bar cannot be written as a fixed number of display beats, because the
    numerator is what it is, so it has a sentinel rather than a value.
*/
inline constexpr double kSnapWholeBar = -1.0;

inline double snapQuarters (const Signature& s, double fractionOfDisplayBeat) noexcept
{
    if (! std::isfinite (fractionOfDisplayBeat))
        return 0.0;

    if (fractionOfDisplayBeat == kSnapWholeBar)
        return barLengthQuarters (s);

    return std::max (0.0, fractionOfDisplayBeat) * beatLengthQuarters (s);
}

//==============================================================================
/** The finest grid line spacing, in quarters, that is at least `minimumPixels`
    apart at a given zoom and still lands on every bar line.

    The editors draw a grid under the ruler and colour each line by whether it
    is a bar, a display beat or neither, so a spacing that does not divide the
    bar is a grid whose lines are mostly uncoloured and whose bars have no line
    at all. Six pixels divided by the zoom is far enough apart and is exactly
    that: a 2/4 project at two and a half pixels a quarter would get lines
    every 2.4 quarters against bars every 2.

    So the candidates climb through spacings that do divide the bar. From
    `finest`, which the caller gives as a step or a display beat, to a display
    beat, then to each multiple of a display beat that divides the numerator,
    then to whole bars. A multiple that does not divide the numerator is
    skipped rather than rounded to: 99/32 has a bar of 12.375 quarters and four
    quarters is not a part of it, which is how doubling up from a display beat
    was found to put lines either side of the bar and none on it.

    `finest` has to divide a display beat for the first rung to keep the
    promise, which a quarter of one does.
*/
inline double gridStepQuarters (const Signature& signature, double pixelsPerQuarter,
                                double minimumPixels, double finest) noexcept
{
    const Signature s = validated (signature);
    const double beat = beatLengthQuarters (s);
    const double bar  = barLengthQuarters (s);

    const double zoom = std::isfinite (pixelsPerQuarter) ? std::max (1.0e-6, pixelsPerQuarter) : 1.0;
    const double want = std::isfinite (minimumPixels) ? std::max (0.0, minimumPixels) : 0.0;

    const auto wideEnough = [zoom, want] (double spacing) { return spacing * zoom >= want; };

    double spacing = (std::isfinite (finest) && finest > 0.0) ? std::min (finest, beat) : beat;

    if (wideEnough (spacing))
        return spacing;

    spacing = beat;

    for (int k = 2; k <= s.numerator && ! wideEnough (spacing); ++k)
        if (s.numerator % k == 0)
            spacing = (double) k * beat;

    if (wideEnough (spacing))
        return spacing;

    // Whole bars, however many it takes. This line is reached only when a
    // single bar is itself too narrow, so `want / (bar * zoom)` is above one
    // and the ceiling of it is at least two: there is no `max (1.0, ...)` here
    // and that is deliberate. It would be reachable only for a zoom that is
    // not a number, which the guard above has already dealt with, and two
    // guards for one hazard is one guard nobody can check.
    return bar * std::ceil (want / (bar * zoom));
}

//==============================================================================
/** Rounds a position up to a bar line, within a tolerance.

    `tolerance`, in quarters, is how far past a line a position may sit and
    still count as being on it. A clip's length in beats is its source length
    in seconds times the tempo, so a one bar loop rarely lands exactly on the
    line, and without a tolerance a loop cut to length would gain a whole bar
    of silence.

    There is no published specification for this and no published tolerance.
    What is published and does constrain it: Pro Tools rounds meter change
    ranges to the "nearest whole bar number" and inserts time "in whole bar
    increments", and Ableton allows a signature marker to fall mid bar and then
    offers to "delete" or "complete" the resulting fragmentary bar. Up rather
    than nearest is the choice here, because the caller is finding the end of
    something and rounding down would cut it off.
*/
inline double roundUpToBar (const Signature& s, double quarters, double tolerance) noexcept
{
    if (! std::isfinite (quarters))
        return 0.0;

    const double bar = barLengthQuarters (s);

    // `std::max (0.0, tolerance)` reads a NaN tolerance as none, and the
    // argument order is what does it: std::max returns its first argument when
    // the comparison is false, and every comparison with a NaN is. Written the
    // other way round it would return the NaN. So there is no isfinite guard
    // here, and this comment is why.
    const double tol = std::max (0.0, tolerance);

    return std::ceil (quarters / bar - tol / bar) * bar;
}

//==============================================================================
/** The Standard MIDI File time signature meta event, as seven bytes.

    `FF 58 04 nn dd cc bb`, where:

    - `nn` is the numerator as notated, literally.
    - `dd` is a base two logarithm: "The denominator is a negative power of
      two: 2 represents a quarter-note, 3 represents an eighth-note, etc."
    - `cc` is "the number of MIDI clocks in a metronome click", in units of a
      twenty fourth of a quarter note.
    - `bb` is "the number of notated 32nd-notes in a MIDI quarter-note (24 MIDI
      Clocks)".

    Assembled here rather than taken from `juce::MidiMessage`, because JUCE's
    own `timeSignatureMetaEvent` returns
    `{ 0xff, 0x58, 0x04, numerator, powerOfTwo, 1, 96 }`: a click every twenty
    fourth of a quarter note, and a quarter note notated as twelve whole notes.
    Both are wrong against the specification, which wants 24 and 8 for 4/4 with
    a quarter note click, and every MIDI file this studio exported before
    tonight carried them. Nothing reads those two bytes in practice, which is
    why it went unnoticed and is not a reason to keep writing them.

    `cc` here follows the click this studio actually sounds, which is one per
    display beat: 24 in 4/4, 12 in 6/8. The specification's own worked example
    is a 6/8 file whose metronome clicks every three eighths, so its `cc` is 36
    and the whole event is `FF 58 04 06 03 24 08`. Worth knowing that the
    widely mirrored jglatt copy of that example gives `cc` as `18`, which is 24
    decimal, one quarter note, two eighths, contradicting its own prose. That
    page is the first thing anybody checking a 6/8 event will find.
*/
inline int midiDenominatorByte (const Signature& s) noexcept
{
    int byte = 0;
    for (int n = 1; n < s.denominator; n <<= 1)
        ++byte;
    return byte;
}

/** MIDI clocks in a click of the given length. Exposed separately from the
    signature so that the specification's own 6/8 example, a click of a dotted
    quarter, can be checked against this rather than against a retyped 36.
*/
inline int midiClocksForQuarters (double clickLengthQuarters) noexcept
{
    if (! std::isfinite (clickLengthQuarters) || clickLengthQuarters <= 0.0)
        return kMidiClocksPerQuarter;

    // Clamped into a byte, and the clamp is unreachable from the signatures
    // the studio accepts: a display beat is `4 / d` for `d` in 1 .. 32, so the
    // click is 96, 48, 24, 12, 6 or 3 clocks and nothing else. It is here for
    // the caller that does not exist yet rather than for the ones that do, and
    // worth knowing before writing that caller: a dotted whole note click in
    // 4/1, which is what Logic's Group layer would want, is 144 clocks and
    // fits, while a longer one would be clamped into a lie rather than
    // refused. The byte cannot say it, so the click would have to.
    const double clocks = clickLengthQuarters * (double) kMidiClocksPerQuarter;
    return (int) std::max (1.0, std::min (255.0, std::floor (clocks + 0.5)));
}

inline int midiClocksPerClick (const Signature& s) noexcept
{
    return midiClocksForQuarters (beatLengthQuarters (s));
}

/** The whole event. Signatures this studio refuses never reach here, so an
    invalid one becomes 4/4 rather than writing a byte that says something
    else.
*/
inline std::array<int, 7> metaEventBytes (const Signature& signature) noexcept
{
    const Signature s = validated (signature);

    return { 0xff, 0x58, 0x04, s.numerator, midiDenominatorByte (s),
             midiClocksPerClick (s), kMidiNotated32ndsPerQuarter };
}

/** Reads a signature out of the two bytes that carry one.

    The two clock bytes are deliberately ignored. `cc` is the writer's own
    click rate, which is a property of the file's metronome rather than of the
    music, and `bb` is "always set as 8" in every file anybody has looked at.
    Reading either would be adopting somebody else's metronome preference as
    this project's time signature.

    Anything out of range comes back as 4/4, which is what the format says an
    absent signature means anyway.
*/
inline Signature fromMetaEventBytes (int numeratorByte, int denominatorByte) noexcept
{
    if (denominatorByte < 0 || denominatorByte > 5)
        return kDefault;

    return validated ({ numeratorByte, 1 << denominatorByte });
}

//==============================================================================
/** "7/8", for a control to show. */
inline std::string toString (const Signature& s)
{
    return std::to_string (s.numerator) + "/" + std::to_string (s.denominator);
}

/** Reads a signature a person typed, and refuses anything that is not one.

    In here rather than in the control, so that what the studio will accept
    from a keyboard is tested rather than assumed. The rule is deliberately
    strict: digits, one slash, digits, with whitespace allowed around them and
    nothing else at all. "4 / 4" is a signature; "4/4 (swing)", "4-4", "four
    four", "4/" and "4/4/4" are not.

    Refusing is the point. A control that read "4/3" as 4/4 would leave
    somebody looking at a signature they did not type, and one that read it as
    the 4 and threw the rest away would be worse. On false, nothing is written
    to `out` and the caller puts back what was there.
*/
inline bool parse (const std::string& text, Signature& out) noexcept
{
    const auto isSpace = [] (char c) { return c == ' ' || c == '\t'; };

    size_t i = 0;
    const size_t n = text.size();

    const auto skipSpace = [&] { while (i < n && isSpace (text[i])) ++i; };

    // No more than three digits, so that a field somebody leant on cannot
    // overflow before the range check sees it.
    const auto readNumber = [&] (int& value)
    {
        const size_t start = i;
        value = 0;
        while (i < n && text[i] >= '0' && text[i] <= '9')
        {
            if (i - start >= 3)
                return false;

            value = value * 10 + (text[i] - '0');
            ++i;
        }
        return i > start;
    };

    int numerator = 0, denominator = 0;

    skipSpace();
    if (! readNumber (numerator))
        return false;

    skipSpace();
    if (i >= n || text[i] != '/')
        return false;
    ++i;

    skipSpace();
    if (! readNumber (denominator))
        return false;

    skipSpace();
    if (i != n)
        return false;

    const Signature parsed { numerator, denominator };
    if (! isValid (parsed))
        return false;

    out = parsed;
    return true;
}

//==============================================================================
/** The signature map.

    Today it holds one entry, fixed at bar 1, and every accessor takes a
    position in quarters and ignores it. See the note at the top of this file
    for why the argument is there from the first commit rather than from the
    night the map arrives.

    When it does arrive, the properties to keep are the ones `MeterTest.cpp`
    already asserts: `at` is total over every finite position, the entry at bar
    1 cannot be moved or removed, and the bar number is monotonic
    non-decreasing in the position. Ardour's warning is worth carrying across
    with them: "Beats increase monotonically throughout the tempo map (BBT may
    not)."
*/
struct Map
{
    /** The signature in force at a position. */
    Signature at (double quarters) const noexcept
    {
        (void) quarters;     // one entry, so every position has the same answer
        return first;
    }

    double barLengthQuartersAt (double quarters) const noexcept
    {
        return barLengthQuarters (at (quarters));
    }

    double beatLengthQuartersAt (double quarters) const noexcept
    {
        return beatLengthQuarters (at (quarters));
    }

    BarBeat barBeatAt (double quarters) const noexcept
    {
        return Meter::barBeatAt (at (quarters), quarters);
    }

    /** Where a bar|beat readout points. Reads the signature at the bar rather
        than at the answer, which is the same thing today and is the right one
        of the two once it is not.
    */
    double quartersAt (const BarBeat& p) const noexcept
    {
        const double atBar = (double) (p.bar - 1) * barLengthQuarters (first);
        return Meter::quartersAt (at (atBar), p);
    }

    // There is deliberately no clickIndexAt or isAccentedClick here. The only
    // thing that clicks is the audio thread, which cannot read a map, and two
    // accessors with no caller are two things that can quietly stop being
    // right. See the note above about the one call site this design does not
    // cover.

    double roundUpToBarAt (double quarters, double tolerance) const noexcept
    {
        return roundUpToBar (at (quarters), quarters, tolerance);
    }

    double barFloorQuartersAt (double quarters) const noexcept
    {
        return barFloorQuarters (at (quarters), quarters);
    }

    double snapQuartersAt (double quarters, double fractionOfDisplayBeat) const noexcept
    {
        return snapQuarters (at (quarters), fractionOfDisplayBeat);
    }

    double gridStepQuartersAt (double quarters, double pixelsPerQuarter,
                               double minimumPixels, double finest) const noexcept
    {
        return gridStepQuarters (at (quarters), pixelsPerQuarter, minimumPixels, finest);
    }

    /** Refuses anything out of range rather than clamping it, so a bad edit
        leaves the project on the signature it had. Returns whether it took.
    */
    bool setFirst (const Signature& s) noexcept
    {
        if (! isValid (s))
            return false;

        first = s;
        return true;
    }

    const Signature& firstSignature() const noexcept { return first; }

    bool operator== (const Map& o) const noexcept { return first == o.first; }
    bool operator!= (const Map& o) const noexcept { return ! (*this == o); }

private:
    // Private so that nothing can put a signature the MIDI file cannot
    // express into the project: the only way in is `setFirst`, which refuses.
    // Fixed at bar 1, which is Cubase's shape for its own first entry: "The
    // first signature event is always positioned at bar 1. You cannot change
    // this."
    Signature first = kDefault;
};

} // namespace Meter
