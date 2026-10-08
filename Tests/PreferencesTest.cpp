// ---------------------------------------------------------------------------
// Checks the reading of application settings.
//
// A preference arrives from a file nobody validated: plain text in the user's
// settings directory, surviving upgrades and downgrades, sometimes hand
// edited. So the interesting inputs here are not the ones a control produces,
// they are the ones a file can contain, and the failures are quiet:
//
//   a value that is not a number     clamping hands a NaN straight back, and
//                                    the conversion to int after it is
//                                    undefined behaviour
//   an id the list does not have     the combo box selects nothing, reads back
//                                    as zero, and the monitoring mode becomes
//                                    an enum value that does not exist
//   an interval that reads as off    the autosave counter then fires on every
//                                    single tick
//   a level formula that is wrong    a metronome at a plausible level that is
//                                    not the one it used to be
//
// Every check below is for one of those rather than for the happy path, and
// the sweep at the end runs the values a file can really hold through all of
// it at once, because what has to be true is that no input produces a number
// the application cannot use.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include "Preferences.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <vector>

static int failures = 0;

static void fail (const std::string& what)
{
    std::printf ("  FAIL  %s\n", what.c_str());
    ++failures;
}

static void check (bool ok, const std::string& what)
{
    if (! ok)
        fail (what);
}

static void checkClose (double got, double wanted, double tolerance, const std::string& what)
{
    if (! (std::abs (got - wanted) <= tolerance))
        fail (what + ": got " + std::to_string (got) + ", wanted " + std::to_string (wanted));
}

/** The values a settings file can really contain, as opposed to the ones a
    control can produce. */
static std::vector<double> hostileNumbers()
{
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();

    return { 0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 128.0, 7200.0,
             1.0e-300, -1.0e-300, 1.0e300, -1.0e300,
             inf, -inf, nan, -nan,
             std::numeric_limits<double>::max(),
             std::numeric_limits<double>::lowest(),
             std::numeric_limits<double>::denorm_min() };
}

static std::vector<Prefs::Range> everyRange()
{
    return { Prefs::autosaveMinutes, Prefs::metronomeLevelDb, Prefs::defaultTempo,
             Prefs::recordTrimMs };
}

//==============================================================================
static void everyDefaultIsInsideItsOwnRange()
{
    // Not as obvious as it looks: a default outside the range it belongs to is
    // a setting that moves the first time the window is opened, because the
    // control cannot show the value and offers a different one back.
    for (const auto& r : everyRange())
    {
        check (r.minimum < r.maximum, "a range is empty or inverted");
        check (r.holds (r.fallback), "a default sits outside its own range");
    }
}

static void aValueThatIsNotANumberFallsBack()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    for (const auto& r : everyRange())
    {
        check (Prefs::number (nan, r)  == r.fallback, "a NaN did not fall back to the default");
        check (Prefs::number (inf, r)  == r.fallback, "an infinity did not fall back to the default");
        check (Prefs::number (-inf, r) == r.fallback, "a negative infinity did not fall back to the default");
    }
}

static void anythingAFileCanHoldComesBackUsable()
{
    for (const auto& r : everyRange())
        for (const double stored : hostileNumbers())
        {
            const double got = Prefs::number (stored, r);

            check (std::isfinite (got), "a stored value came back as something that is not a number");
            check (r.holds (got), "a stored value came back outside its range");
        }
}

static void aValueInRangeIsLeftAlone()
{
    // The clamp must not be doing anything to values that were already fine,
    // or a preference quietly changes every time it is read back.
    for (const auto& r : everyRange())
    {
        const double inside[] = { r.minimum, r.maximum, r.fallback,
                                  0.5 * (r.minimum + r.maximum) };

        for (const double v : inside)
            check (Prefs::number (v, r) == v, "a value already in range was altered on the way in");
    }
}

//==============================================================================
static void anIdTheListDoesNotHaveFallsBack()
{
    for (const auto& c : { Prefs::monitorMode, Prefs::recordMode, Prefs::countIn })
    {
        check (c.count >= 1, "a choice has no items");
        check (c.fallback >= 1 && c.fallback <= c.count, "a choice default is not one of its items");

        // Zero is the one that mattered: an unknown id leaves a combo box with
        // nothing selected, and nothing selected reads back as zero.
        for (const int stored : { 0, -1, -1000, c.count + 1, c.count + 99, 1000000 })
            check (Prefs::choiceId (stored, c) == c.fallback,
                   "the id " + std::to_string (stored) + " was accepted as one of "
                     + std::to_string (c.count) + " items");

        for (int id = 1; id <= c.count; ++id)
            check (Prefs::choiceId (id, c) == id, "a valid id was replaced by the default");
    }
}

static void theMonitoringModeNeverLeavesTheEnum()
{
    // The whole reason choiceId exists. The application turns this id into
    // AudioEngine::Monitor by subtracting one, so the id has to be inside the
    // list for the enum value to exist at all.
    for (const int stored : { -5, -1, 0, 1, 2, 3, 4, 5, 99 })
    {
        const int mode = Prefs::choiceId (stored, Prefs::monitorMode) - 1;

        check (mode >= 0 && mode < Prefs::monitorMode.count,
               "the stored id " + std::to_string (stored) + " produced monitoring mode "
                 + std::to_string (mode) + ", which is not one the engine has");
    }
}

//==============================================================================
static void onlySomethingThatLooksLikeANumberIsTrusted()
{
    // The ones that have to be rejected. Each of them parses as zero, and zero
    // is a real setting: off for the autosave, full level for the metronome.
    for (const char* text : { "", " ", "abc", "nan", "NaN", "inf", "-inf", "0x10",
                              "true", "off", "-", "+", ".", "--", "e", "E", "+-.",
                              "12 dB", "1,5", "2of3" })
        check (! Prefs::looksNumeric (text),
               std::string ("\"") + text + "\" was treated as a number");

    for (const char* text : { "0", "-0", "1", "128", "-9.1", "+2.5", ".5", "-.5",
                              "1e3", "1E-3", "2.", "30", "0.0001" })
        check (Prefs::looksNumeric (text),
               std::string ("\"") + text + "\" was not treated as a number");

    check (! Prefs::looksNumeric (nullptr), "a missing value was treated as a number");
}

static void everyAutosaveIntervalIsOneOfTheOfferedOnes()
{
    check (Prefs::autosaveChoiceCount == Prefs::autosaveChoice.count,
           "the autosave list and the number of items it claims disagree");

    check (Prefs::autosaveChoices[0] == 0.0, "the first autosave interval is not off");

    for (int i = 1; i < Prefs::autosaveChoiceCount; ++i)
    {
        check (Prefs::autosaveChoices[i] > Prefs::autosaveChoices[i - 1],
               "the autosave intervals are not in increasing order");
        check (Prefs::autosaveMinutes.holds (Prefs::autosaveChoices[i]),
               "an offered autosave interval is outside the range the arithmetic accepts");
    }

    // The reason this is a choice rather than a number: an id that is not on
    // the list has to reach the default, where an unreadable number would
    // reach zero, which is off. Reading past the end of the list would be a
    // sanitizer error here rather than a wrong interval, which is the point of
    // asking for every id a file could hold.
    for (const int stored : { -1000, -1, 0, Prefs::autosaveChoiceCount + 1, 99999 })
        checkClose (Prefs::autosaveMinutesFor (stored), 2.0, 0.0,
                    "an autosave id off the list did not fall back to two minutes");

    for (int id = 1; id <= Prefs::autosaveChoiceCount; ++id)
        checkClose (Prefs::autosaveMinutesFor (id), Prefs::autosaveChoices[id - 1], 0.0,
                    "an autosave id gave the wrong interval");

    // And the default is still the interval the autosave used to run at.
    checkClose (Prefs::autosaveMinutesFor (Prefs::autosaveChoice.fallback), 2.0, 0.0,
                "the default autosave interval is no longer two minutes");
}

//==============================================================================
static void theAutosaveIntervalKeepsTheIntervalItAlwaysHad()
{
    // Pinned on purpose. The autosave used to be a literal 60 * 120 counted
    // against a timer running at 60 Hz, and this is the same two minutes.
    // Anyone rewriting the arithmetic has to keep it or explain it.
    check (Prefs::autosaveTicks (2.0, 60.0) == 7200,
           "two minutes at 60 Hz is no longer the 7200 ticks it used to be");

    check (Prefs::autosaveTicks (1.0, 60.0) == 3600, "one minute at 60 Hz is not 3600 ticks");
    check (Prefs::autosaveTicks (0.5, 60.0) == 1800, "half a minute at 60 Hz is not 1800 ticks");

    // The rate is a parameter rather than an assumption, so a change to the
    // timer must carry the interval with it.
    check (Prefs::autosaveTicks (2.0, 30.0) == 3600, "the interval ignored a change of timer rate");

    // And the rate the application really runs at is the one the old literal
    // assumed, so the default interval is unchanged by all of this.
    check (Prefs::mainTimerHz > 0.0, "the main timer rate is not positive");
    check (Prefs::autosaveTicks (Prefs::autosaveMinutesFor (Prefs::autosaveChoice.fallback),
                                 Prefs::mainTimerHz) == 7200,
           "the default autosave interval is no longer the 7200 ticks the application used");
}

static void autosavingOffIsTheOnlyWayToGetZero()
{
    check (Prefs::autosaveTicks (0.0, 60.0) == 0, "zero minutes did not turn autosaving off");

    // Everything else has to be at least one tick. A zero returned for an
    // interval that is on means "++ticks >= 0" at the call site, which
    // autosaves on every tick of a 60 Hz timer.
    //
    // Including the intervals too small to be a whole tick. No control offers
    // those, which is the point: a file can hold them, and they are the only
    // values that round down to zero, so a sweep that starts at something
    // sensible tests the floor not at all. That is how this check was written
    // the first time, and removing the floor passed it.
    for (const double minutes : { 1.0e-9, 1.0e-6, 0.0001, 0.001, 0.005, 0.01 })
        check (Prefs::autosaveTicks (minutes, 60.0) >= 1,
               "an interval too short to be one tick came back as off at "
                 + std::to_string (minutes) + " minutes");

    for (double minutes = 0.01; minutes <= Prefs::autosaveMinutes.maximum + 5.0; minutes += 0.17)
    {
        const int ticks = Prefs::autosaveTicks (minutes, 60.0);

        check (ticks >= 1, "an interval that is on came back as off at "
                             + std::to_string (minutes) + " minutes");
    }

    // A rate that cannot be trusted is the other way to reach zero, and it has
    // to be off rather than every tick.
    const double nan = std::numeric_limits<double>::quiet_NaN();

    check (Prefs::autosaveTicks (2.0, 0.0) == 0, "a zero timer rate did not turn autosaving off");
    check (Prefs::autosaveTicks (2.0, -60.0) == 0, "a negative timer rate did not turn autosaving off");
    check (Prefs::autosaveTicks (2.0, nan) == 0, "a timer rate that is not a number did not turn autosaving off");
}

static void aLongerIntervalIsNeverFewerTicks()
{
    int previous = 0;

    for (double minutes = 0.0; minutes <= Prefs::autosaveMinutes.maximum; minutes += 0.05)
    {
        const int ticks = Prefs::autosaveTicks (minutes, 60.0);

        check (ticks >= previous, "asking for a longer gap between autosaves gave a shorter one at "
                                   + std::to_string (minutes) + " minutes");
        previous = ticks;
    }
}

//==============================================================================
static void theMetronomeLevelIsTheLevelItAlwaysWas()
{
    // The click was a fixed 0.35 before it had a control, and the default has
    // to still be that or every existing user's metronome changes level on
    // upgrade for no reason they asked for.
    checkClose (Prefs::metronomeGain (Prefs::metronomeLevelDb.fallback), 0.35, 0.005,
                "the default click level is no longer the fixed level it replaced");
}

static void theLevelFormulaIsDecibels()
{
    checkClose (Prefs::metronomeGain (0.0), 1.0, 1.0e-9, "0 dB is not unity gain");
    checkClose (Prefs::metronomeGain (-6.0206), 0.5, 1.0e-4, "-6 dB is not half the amplitude");
    checkClose (Prefs::metronomeGain (-20.0), 0.1, 1.0e-6, "-20 dB is not a tenth of the amplitude");

    // Amplitude, not power. Getting this wrong gives a level that is plausible
    // at every setting and wrong at all of them.
    checkClose (Prefs::metronomeGain (-3.0103), 0.70710678, 1.0e-6,
                "-3 dB is not the amplitude ratio it should be");
}

static void theQuietestSettingIsSilent()
{
    check (Prefs::metronomeGain (Prefs::metronomeLevelDb.minimum) == 0.0f,
           "the bottom of the level range still makes a sound");

    // And anything a file might hold below it, rather than only the exact
    // bound.
    check (Prefs::metronomeGain (-1000.0) == 0.0f, "a level far below the range still makes a sound");
}

static void theLevelIsBoundedAndRises()
{
    float previous = -1.0f;

    for (double db = Prefs::metronomeLevelDb.minimum; db <= Prefs::metronomeLevelDb.maximum; db += 0.1)
    {
        const float gain = Prefs::metronomeGain (db);

        check (std::isfinite (gain), "the click gain is not a number at " + std::to_string (db) + " dB");
        check (gain >= 0.0f, "the click gain went negative at " + std::to_string (db) + " dB");

        // The click is summed into the master after the mix, so a gain above
        // one can clip a master that was not clipping.
        check (gain <= 1.0f, "the click gain exceeded unity at " + std::to_string (db) + " dB");
        check (gain >= previous, "turning the click up made it quieter at " + std::to_string (db) + " dB");

        previous = gain;
    }
}

//==============================================================================
static void nothingAFileCanHoldBreaksAnything()
{
    // Everything at once, over the hostile values and a long randomised run,
    // because the guarantee being made is about the whole path rather than any
    // one function: whatever the file said, the application gets a number it
    // can use.
    std::mt19937 rng (0x9f27a1);
    std::uniform_real_distribution<double> wide (-1.0e9, 1.0e9);
    std::uniform_int_distribution<int> ids (-50, 50);

    auto exercise = [] (double stored, int id)
    {
        const int ticks = Prefs::autosaveTicks (stored, 60.0);
        check (ticks >= 0 && ticks <= 1000000000, "the autosave interval left its bounds");

        const float gain = Prefs::metronomeGain (stored);
        check (std::isfinite (gain) && gain >= 0.0f && gain <= 1.0f, "the click gain left its bounds");

        const double tempo = Prefs::number (stored, Prefs::defaultTempo);
        check (std::isfinite (tempo) && Prefs::defaultTempo.holds (tempo),
               "the default tempo left its range");

        const double trim = Prefs::number (stored, Prefs::recordTrimMs);
        check (std::isfinite (trim) && Prefs::recordTrimMs.holds (trim),
               "the record offset correction left its range");

        // Whatever the file held, the figure the engine ends up applying has
        // to be the one the control could have produced. A correction the
        // window offers and the arithmetic then clamps is a control that
        // stops working partway along its travel.
        RecordAlign::Figures f;
        f.sampleRate = 48000.0;
        f.trimMs     = trim;
        const int offsetWithTrim = RecordAlign::captureOffsetSamples (f);

        f.trimMs = 0.0;
        const int offsetWithout = RecordAlign::captureOffsetSamples (f);

        check (offsetWithTrim - offsetWithout
                 == (int) std::lround (trim * 48.0),
               "a correction inside the control's own range was clamped by the arithmetic");

        for (const auto& c : { Prefs::monitorMode, Prefs::recordMode, Prefs::countIn,
                               Prefs::autosaveChoice })
        {
            const int chosen = Prefs::choiceId (id, c);
            check (chosen >= 1 && chosen <= c.count, "a choice left its list");
        }

        // Reads the list, so an id that escaped validation is an out of bounds
        // read rather than a wrong answer, and the sanitizer says so.
        const double interval = Prefs::autosaveMinutesFor (id);
        check (Prefs::autosaveMinutes.holds (interval), "an autosave interval left its range");
    };

    for (const double stored : hostileNumbers())
        for (int id = -3; id <= 10; ++id)
            exercise (stored, id);

    for (int i = 0; i < 200000; ++i)
        exercise (wide (rng), ids (rng));
}

//==============================================================================
//==============================================================================
/** The correction has to default to nothing.

    The studio is meant to be right from the figures the driver reports, and a
    correction with a default in it would be a thumb on the scale that nobody
    put there. Zero also has to be inside the range, or the control cannot
    return to it.
*/
static void theRecordTrimDefaultsToDoingNothing()
{
    check (Prefs::recordTrimMs.fallback == 0.0,
           "the record offset correction does not default to nothing");
    check (Prefs::recordTrimMs.holds (0.0),
           "the record offset correction cannot be returned to nothing");
    check (Prefs::recordTrimMs.minimum < 0.0 && Prefs::recordTrimMs.maximum > 0.0,
           "the record offset correction only goes one way");

    // The same bounds the arithmetic clamps to, by construction rather than by
    // coincidence: two copies of a range drift.
    check (Prefs::recordTrimMs.minimum == RecordAlign::minTrimMs
             && Prefs::recordTrimMs.maximum == RecordAlign::maxTrimMs,
           "the control's range and the arithmetic's range have drifted apart");
}

int main()
{
    std::printf ("preferences\n");

    everyDefaultIsInsideItsOwnRange();
    aValueThatIsNotANumberFallsBack();
    anythingAFileCanHoldComesBackUsable();
    aValueInRangeIsLeftAlone();

    anIdTheListDoesNotHaveFallsBack();
    theMonitoringModeNeverLeavesTheEnum();

    onlySomethingThatLooksLikeANumberIsTrusted();
    everyAutosaveIntervalIsOneOfTheOfferedOnes();

    theAutosaveIntervalKeepsTheIntervalItAlwaysHad();
    autosavingOffIsTheOnlyWayToGetZero();
    aLongerIntervalIsNeverFewerTicks();

    theMetronomeLevelIsTheLevelItAlwaysWas();
    theLevelFormulaIsDecibels();
    theQuietestSettingIsSilent();
    theLevelIsBoundedAndRises();

    nothingAFileCanHoldBreaksAnything();

    theRecordTrimDefaultsToDoingNothing();

    if (failures > 0)
    {
        std::printf ("\n%d preferences check(s) failed\n", failures);
        return 1;
    }

    std::printf ("all preferences checks passed\n");
    return 0;
}
