#pragma once

#include <algorithm>
#include <cmath>

//==============================================================================
/** The settings that belong to the application rather than to a project: what
    each one's bounds are, what to do when a stored value cannot be trusted,
    and the arithmetic that turns it into the number the engine wants.

    No JUCE, so Tests/PreferencesTest.cpp drives this directly. That matters
    more than it looks. A preference is read from a file the application does
    not control: plain text in the user's own settings directory, which
    outlives upgrades and downgrades and which people edit by hand. Each of
    those is a route to a value no control in the window could have produced,
    and what follows is quiet: a metronome at the wrong level, an autosave that
    never fires, a monitoring mode the engine half believes in. Clamping is not
    enough on its own either, because a stored value need not be a number at
    all, and every comparison against a NaN is false: std::clamp hands the NaN
    straight back, and the conversion to int after it is undefined behaviour
    rather than a wrong answer.

    So reading and converting live here, tested against the values a file can
    really contain rather than the ones a slider can produce.
*/
namespace Prefs
{

//==============================================================================
/** The settings file keys.

    Spelled once here rather than at each use. A key written under one spelling
    and read under another is a setting that silently forgets itself, and
    neither side of it looks wrong on its own.
*/
namespace Key
{
    inline constexpr const char* autosaveMinutes  = "autosaveMinutes";
    inline constexpr const char* metronomeLevelDb = "metronomeLevelDb";
    inline constexpr const char* defaultTempo     = "defaultTempo";

    // Already written by the transport controls before this header existed, so
    // the spellings have to stay as they are to keep existing settings files
    // readable.
    inline constexpr const char* monitorMode      = "monitorMode";
    inline constexpr const char* recordMode       = "recordMode";
    inline constexpr const char* countIn          = "countIn";
}

//==============================================================================
/** A numeric setting: the bounds a control offers, and the value to use when
    there is no stored one or the stored one is unusable. */
struct Range
{
    double minimum, maximum, fallback;

    constexpr bool holds (double v) const noexcept { return v >= minimum && v <= maximum; }
};

/** The bound on how long an autosave interval may be, in minutes, where zero
    turns autosaving off. The interval itself is chosen from a list below: this
    is only the range the arithmetic will accept. */
inline constexpr Range autosaveMinutes { 0.0, 30.0, 2.0 };

/** The metronome's level in decibels, where the bottom of the range is
    silence rather than a very quiet click.

    The default is the level the click had when it was a fixed 0.35, to within
    a tenth of a decibel, so that turning this into a preference does not
    change what anyone already hears. The top is 0 dB rather than something
    with headroom above it: the click is summed into the master output after
    the mix, so a click with gain above one can clip a master that was fine,
    and a full scale sine burst is already louder than anyone wants.
*/
inline constexpr Range metronomeLevelDb { -48.0, 0.0, -9.1 };

/** The tempo a new project starts at.

    The bounds match the tempo control's own, because a preference that cannot
    be reached from the control it sets is a preference that looks broken.
*/
inline constexpr Range defaultTempo { 20.0, 400.0, 128.0 };

//==============================================================================
/** Reads a stored number back, usable whatever the file contained. */
inline double number (double stored, const Range& r) noexcept
{
    // Ordered this way round deliberately: the finite test has to come first,
    // because clamping a NaN returns the NaN.
    if (! std::isfinite (stored))
        return r.fallback;

    return std::clamp (stored, r.minimum, r.maximum);
}

//==============================================================================
/** A setting chosen from a list, stored as the one based id of a combo box
    item. */
struct Choice
{
    int count, fallback;
};

/** Monitoring: off, armed, on. Mirrors AudioEngine::Monitor. */
inline constexpr Choice monitorMode { 3, 1 };

/** What the record button captures. Mirrors MainComponent's RecordModeId. */
inline constexpr Choice recordMode { 6, 1 };

/** Count-in: off, one bar, two bars. */
inline constexpr Choice countIn { 3, 1 };

/** Reads a stored choice back, guaranteeing an id the list really has.

    Worth its own function because the application used to pass these straight
    from the settings file to ComboBox::setSelectedId, and an id the box does
    not have leaves the box with nothing selected, which reads back as zero.
    For the monitoring mode that zero then became the enum value minus one,
    so an id that was merely out of range turned into a monitoring state the
    engine did not have: the callback took its low latency monitoring path,
    which skips delay compensation, while the mixer matched neither "armed"
    nor "on" and passed no input through. Monitoring that is silent and costs
    the compensation anyway is the shape of failure that takes a day to find,
    and the only thing needed to cause it was an old settings file.
*/
inline int choiceId (int stored, const Choice& c) noexcept
{
    return (stored >= 1 && stored <= c.count) ? stored : c.fallback;
}

//==============================================================================
/** How many ticks of a timer running at timerHz make up the autosave
    interval, or zero when autosaving is off.

    Returning zero for "off" and never for an interval that is on: the caller
    counts ticks and compares, so a zero where an interval was meant would
    autosave on every single tick.
*/
inline int autosaveTicks (double minutes, double timerHz) noexcept
{
    const double m = number (minutes, autosaveMinutes);

    // Written as a positive test so that a NaN rate falls here rather than
    // through.
    if (m <= 0.0 || ! (timerHz > 0.0))
        return 0;

    const double ticks = std::round (m * 60.0 * timerHz);

    // At least one tick, so an interval that is on cannot read as off, and
    // bounded well inside int so an absurd timer rate cannot overflow the
    // conversion.
    return (int) std::clamp (ticks, 1.0, 1.0e9);
}

/** Linear gain for the metronome, from the stored level in decibels. */
inline float metronomeGain (double storedDb) noexcept
{
    const double db = number (storedDb, metronomeLevelDb);

    // The bottom of the range means off. A level control whose quietest
    // setting still ticks is a control that does not do what it looks like it
    // does, and the metronome has its own button for the same job.
    if (db <= metronomeLevelDb.minimum)
        return 0.0f;

    return (float) std::pow (10.0, db / 20.0);
}

//==============================================================================
/** Whether a stored string is worth handing to a number parser at all.

    Needed because the obvious reading of a settings file does the wrong thing
    quietly. A string that is not a number parses as zero, and zero is a real
    setting for several of these: it is off for the autosave and full level for
    the metronome. So one garbled line in the file would read back as a
    deliberate choice, and for the metronome it would read back as the loudest
    one there is. Anything that does not look like a number has to reach the
    default instead, which means looking at the text before trusting it.

    The text is expected to have been trimmed already. "nan" and "inf" are
    rejected here rather than later: they parse on some runtimes, and a level
    or an interval that is not a number is not a setting.
*/
inline bool looksNumeric (const char* text) noexcept
{
    if (text == nullptr)
        return false;

    bool sawDigit = false;

    for (const char* c = text; *c != 0; ++c)
    {
        if (*c >= '0' && *c <= '9')
        {
            sawDigit = true;
            continue;
        }

        if (*c != '+' && *c != '-' && *c != '.' && *c != 'e' && *c != 'E')
            return false;
    }

    // A string of signs and points parses as zero just as readily as a word
    // does, so a digit somewhere is the thing that makes it a number.
    return sawDigit;
}

//==============================================================================
/** The autosave intervals offered, in minutes, with off first.

    A list rather than a free number because of the paragraph above: a choice
    is stored as an id, and an id that is not one of these falls back to the
    default, where a garbled interval read as a number would read as off. The
    one setting nobody wants to lose silently is the one that saves their work.
*/
inline constexpr double autosaveChoices[] = { 0.0, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0 };

inline constexpr int autosaveChoiceCount = (int) (sizeof (autosaveChoices) / sizeof (autosaveChoices[0]));

/** Defaulting to the fourth item, which is the two minutes the autosave ran at
    when the interval was not adjustable. */
inline constexpr Choice autosaveChoice { autosaveChoiceCount, 4 };

/** Minutes for a stored autosave id, guaranteed to be one of the offered
    intervals. */
inline double autosaveMinutesFor (int storedId) noexcept
{
    return autosaveChoices[(size_t) (choiceId (storedId, autosaveChoice) - 1)];
}

} // namespace Prefs
