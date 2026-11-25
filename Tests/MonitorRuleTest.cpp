// ---------------------------------------------------------------------------
// MonitorRule.h: whether a player hears their own input, and what the
// transport has to do with it.
//
// The input space here is finite and small: three choices, three rules, two
// transport states, two input states and three capture states, which is 108
// states. So this enumerates all of them rather than sampling, which is the
// one case where exhaustion beats the randomised testing this project usually
// prefers. Nothing can be missed by a generator that never happened to
// produce it.
//
// What is deliberately not here is a transcription of the table into an
// expected array. A table checked against a second copy of itself proves the
// two copies agree, which is the trap CLAUDE.md names for the wavetable
// spectra. Every check below is instead a property taken from a sentence in a
// manufacturer's manual, and the sentence is quoted beside it, so the test
// fails when the behaviour stops matching what was published rather than when
// it stops matching whatever the header happens to say today.
//
// The checks that have found the most, in rough order:
//
//  - the three rules must DIFFER, at the cell that separates each pair. Two
//    of the three published rules differ in exactly one cell of the table, so
//    a table that collapses that cell has silently built two identical rules
//    and every per rule check still passes.
//  - the stored numbers and the choice values are pinned by name, because
//    both are written into files: the rule into an .ahp, the choice into the
//    settings file as a combo box id. Moving either silently changes what
//    every existing file means.
// ---------------------------------------------------------------------------

#include "MonitorRule.h"

#include <climits>
#include <cstdio>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void fail (const std::string& what)
{
    std::printf ("  FAIL: %s\n", what.c_str());
    ++failures;
}

void check (bool condition, const std::string& what)
{
    if (! condition)
        fail (what);
}

using MonitorRule::Capture;
using MonitorRule::Choice;
using MonitorRule::Insert;
using MonitorRule::Rule;
using MonitorRule::Transport;

const Choice  allChoices[]  { Choice::off, Choice::armed, Choice::always };
const Rule    allRules[]    { Rule::whileArmed, Rule::whileRecording, Rule::tapeMachine };
const Capture allCaptures[] { Capture::idle, Capture::armed, Capture::recording };

const char* choiceText (Choice c)
{
    switch (c)
    {
        case Choice::off:    return "off";
        case Choice::armed:  return "armed";
        case Choice::always: return "always";
    }

    return "?";
}

const char* ruleText (Rule r)
{
    switch (r)
    {
        case Rule::whileArmed:     return "whileArmed";
        case Rule::whileRecording: return "whileRecording";
        case Rule::tapeMachine:    return "tapeMachine";
    }

    return "?";
}

const char* captureText (Capture c)
{
    switch (c)
    {
        case Capture::idle:      return "idle";
        case Capture::armed:     return "armed";
        case Capture::recording: return "recording";
    }

    return "?";
}

/** One cell of the table, so a failure can say which one it was. */
struct State
{
    Choice  choice;
    Rule    rule;
    Capture capture;
    bool    rolling, hasInput;

    bool audible() const
    {
        return MonitorRule::inputAudible (choice, rule,
                                          Transport { .rolling = rolling },
                                          Insert { .hasInput = hasInput, .capture = capture });
    }

    bool armedOrRecording() const { return capture != Capture::idle; }
    bool beingRecorded()    const { return capture == Capture::recording; }

    std::string text() const
    {
        return std::string (choiceText (choice)) + "/" + ruleText (rule)
             + (rolling  ? " rolling" : " stopped")
             + (hasInput ? " input"   : " noInput")
             + " " + captureText (capture);
    }
};

std::vector<State> everyState()
{
    std::vector<State> states;

    for (auto choice : allChoices)
        for (auto rule : allRules)
            for (auto capture : allCaptures)
                for (int rolling = 0; rolling < 2; ++rolling)
                    for (int hasInput = 0; hasInput < 2; ++hasInput)
                        states.push_back ({ choice, rule, capture, rolling != 0, hasInput != 0 });

    return states;
}

/** Whether this insert is one the `armed` choice passes at all, which is the
    gate every rule sits behind. */
bool participates (const State& s)
{
    return s.hasInput && s.armedOrRecording();
}

//==============================================================================
void theWholeTable()
{
    std::printf ("Every state, enumerated\n");

    const auto states = everyState();

    // 3 choices * 3 rules * 3 capture states * 2 * 2. Pinned so that adding a
    // rule or a capture state without extending this enumeration fails here
    // rather than quietly testing part of the table.
    check (states.size() == 108, "the enumeration is not the whole space: "
                                 + std::to_string (states.size()) + " states");

    check ((int) MonitorRule::numRules == (int) (sizeof (allRules) / sizeof (allRules[0])),
           "numRules disagrees with the rules this test knows about");

    for (const auto& s : states)
    {
        const bool audible = s.audible();

        // An input that does not resolve to channels the device has cannot be
        // heard, under any choice and any rule. First because it is the one
        // answer that no setting may override: there is nothing to hear.
        if (! s.hasInput)
            check (! audible, "an insert with no input was audible: " + s.text());

        // Somebody has turned monitoring off.
        if (s.choice == Choice::off)
            check (! audible, "monitoring was off and an input was audible: " + s.text());

        // Steinberg's "Manual" mode: "Turns input monitoring on or off when
        // you click Monitor." Somebody stating an answer, so no rule and no
        // transport may take it away.
        if (s.choice == Choice::always && s.hasInput)
            check (audible, "monitoring was on and an input was silent: " + s.text());

        // Steinberg's "While Record-Enabled": "Connects the audio source to
        // the channel input when you click Record Enable." Nothing that was
        // not armed, or already being recorded, is passed.
        if (s.choice == Choice::armed && ! s.armedOrRecording())
            check (! audible, "an unarmed, idle insert was audible: " + s.text());

        // All three published rules open the input while a take is running:
        // "during recording only", "and during recording", and the one that
        // never closes it at all.
        if (s.choice == Choice::armed && s.hasInput && s.beingRecorded())
            check (audible, "a strip being recorded could not hear itself: " + s.text());

        if (s.choice != Choice::armed || ! participates (s))
            continue;

        switch (s.rule)
        {
            // What the studio did before this rule existed, and the shipped
            // default: armed is monitored, whatever the transport is doing.
            case Rule::whileArmed:
                check (audible, "whileArmed closed an armed input: " + s.text());
                break;

            // "Switches to input monitoring during recording only."
            case Rule::whileRecording:
                check (audible == s.beingRecorded(),
                       "whileRecording did not follow the recorder: " + s.text());
                break;

            // "Activates input monitoring in stop mode and during recording,
            // but not during playback." Reaper, independently: "monitors
            // input only when the transport has stopped or when you are
            // recording", and "During playback you will hear only the
            // timeline audio, not the input."
            case Rule::tapeMachine:
                check (audible == (s.beingRecorded() || ! s.rolling),
                       "tapeMachine is not stopped-or-recording: " + s.text());
                break;
        }
    }
}

//==============================================================================
/** The check that catches two rules having quietly become one.

    Every check above tests a rule against its own expected behaviour, and all
    of them pass a header in which two rules return the same thing. Two of the
    three published rules differ in exactly one cell, so that is not a remote
    possibility: it is one deleted line.
*/
void theRulesDiffer()
{
    std::printf ("The three rules are distinguishable, at the cells that separate them\n");

    const auto audibleUnder = [] (Rule rule, bool rolling, bool recording)
    {
        return MonitorRule::inputAudible (Choice::armed, rule, Transport { .rolling = rolling },
                                          Insert { .hasInput = true,
                                                   .capture  = recording ? Capture::recording
                                                                         : Capture::armed });
    };

    // Stopped, armed, nothing being recorded. This is the ONLY cell in which
    // whileRecording and tapeMachine differ, which is what Steinberg's two
    // sentences say: one adds "in stop mode" and neither says anything else
    // the other does not.
    check (! audibleUnder (Rule::whileRecording, false, false),
           "whileRecording was audible stopped and idle, so it is not \"during recording only\"");
    check (audibleUnder (Rule::tapeMachine, false, false),
           "tapeMachine was silent stopped and idle, so it is not \"in stop mode\"");
    check (audibleUnder (Rule::whileRecording, false, false)
               != audibleUnder (Rule::tapeMachine, false, false),
           "whileRecording and tapeMachine agree when stopped, so they are one rule");

    // And they must agree everywhere else, or the cell above is not the only
    // difference and one of them is not the published rule.
    for (int capturing = 0; capturing < 2; ++capturing)
        check (audibleUnder (Rule::whileRecording, true, capturing != 0)
                   == audibleUnder (Rule::tapeMachine, true, capturing != 0),
               "whileRecording and tapeMachine disagree while rolling, "
               "which neither manual says they do");

    // Rolling, armed, not recording: the cell the whole night is about, and
    // the one that separates the old behaviour from both new rules.
    check (audibleUnder (Rule::whileArmed, true, false),
           "whileArmed closed an armed input during playback, which changes what the "
           "studio has always done");
    check (! audibleUnder (Rule::whileRecording, true, false),
           "whileRecording was audible during playback of a take it was not recording");
    check (! audibleUnder (Rule::tapeMachine, true, false),
           "tapeMachine was audible during playback, which is the one thing both "
           "manuals say it is not");

    // Stopped and idle separates whileArmed from whileRecording as well.
    check (audibleUnder (Rule::whileArmed, false, false)
               != audibleUnder (Rule::whileRecording, false, false),
           "whileArmed and whileRecording agree when stopped and idle, so they are one rule");
}

//==============================================================================
/** The property the feature exists to produce, stated once on its own.

    Everything above is about cells. This is the sentence somebody would use
    to say what changed: an armed strip can now be silent while you listen
    back and open again the moment you record.
*/
void whatThisIsFor()
{
    std::printf ("The behaviour the rule exists to make possible\n");

    const Insert armedWithInput { .hasInput = true, .capture = Capture::armed };
    const Insert recording      { .hasInput = true, .capture = Capture::recording };

    int rulesThatCloseDuringPlayback = 0;

    for (auto rule : allRules)
    {
        const bool duringPlayback = MonitorRule::inputAudible (Choice::armed, rule,
                                                               Transport { .rolling = true }, armedWithInput);
        const bool whileRecording = MonitorRule::inputAudible (Choice::armed, rule,
                                                               Transport { .rolling = true }, recording);

        if (! duringPlayback)
        {
            ++rulesThatCloseDuringPlayback;

            // A rule that closed the input while recording as well would be
            // no monitoring at all rather than a monitoring rule.
            check (whileRecording,
                   std::string (ruleText (rule)) + " closes the input during playback and "
                   "does not open it to record, which is not a monitoring rule");
        }
    }

    check (rulesThatCloseDuringPlayback == 2,
           "expected two rules to close an armed input during playback, got "
               + std::to_string (rulesThatCloseDuringPlayback));

    // And the old behaviour is still reachable, which is what makes the
    // shipped default a non event for anybody who had been using it.
    check (MonitorRule::inputAudible (Choice::armed, MonitorRule::defaultRule,
                                      Transport { .rolling = true }, armedWithInput),
           "the default rule no longer monitors an armed strip during playback, so "
           "this change is not silent after all");

    // With monitoring simply on, the transport still decides nothing. Checked
    // here as well as in the table because it is the promise that makes the
    // rule safe to ship: somebody who wants to hear everything always can.
    for (auto rule : allRules)
        for (int rolling = 0; rolling < 2; ++rolling)
            check (MonitorRule::inputAudible (Choice::always, rule,
                                              Transport { .rolling = rolling != 0 },
                                              Insert { .hasInput = true,
                                                       .capture  = Capture::idle }),
                   "Choice::always was overridden by a rule or by the transport");
}

//==============================================================================
/** What is written into a file, which is the part that cannot be changed
    later without changing what every existing file means. */
void whatIsStored()
{
    std::printf ("The stored numbers and the choice values\n");

    // Pinned by name, not read back from the header. These three numbers are
    // in every .ahp ever saved by a build that has this, so they are fixed:
    // moving them would make a saved tape machine project open monitoring
    // some other way.
    check (MonitorRule::ruleToStored (Rule::whileArmed)     == 0, "whileArmed is not 0");
    check (MonitorRule::ruleToStored (Rule::whileRecording) == 1, "whileRecording is not 1");
    check (MonitorRule::ruleToStored (Rule::tapeMachine)    == 2, "tapeMachine is not 2");

    // The toolbar casts a combo box id minus one straight to Choice, and the
    // id is in the settings file, so these three are fixed for the same
    // reason. This is the check that would catch the enum being reordered
    // after it moved out of AudioEngine and into this header.
    check ((int) Choice::off    == 0, "Choice::off is not 0");
    check ((int) Choice::armed  == 1, "Choice::armed is not 1");
    check ((int) Choice::always == 2, "Choice::always is not 2");

    for (auto rule : allRules)
        check (MonitorRule::ruleFromStored (MonitorRule::ruleToStored (rule)) == rule,
               std::string (ruleText (rule)) + " did not survive a round trip");

    // Menu order: every rule exactly once, no gaps and no repeats, so a rule
    // cannot be unreachable from the menu or offered twice.
    std::vector<int> seen (MonitorRule::numRules, 0);

    for (int i = 0; i < MonitorRule::numRules; ++i)
    {
        const int stored = MonitorRule::ruleToStored (MonitorRule::ruleAt (i));
        check (stored >= 0 && stored < MonitorRule::numRules,
               "ruleAt gave something outside the list at index " + std::to_string (i));

        // Menu order is stored order, exactly, which is what keeps the first
        // item the one that is safe to fall back to and what makes a fourth
        // rule append rather than insert. Checked position by position
        // rather than as a set: a rotation of the three is a set of three
        // and passes every other check here, and it would quietly put the
        // default in last place.
        check (stored == i, "menu position " + std::to_string (i) + " holds the rule stored as "
                                + std::to_string (stored));

        if (stored >= 0 && stored < MonitorRule::numRules)
            ++seen[(size_t) stored];
    }

    check (MonitorRule::ruleAt (0) == MonitorRule::defaultRule,
           "the first rule in the menu is not the one an unreadable setting falls back to");

    for (int i = 0; i < MonitorRule::numRules; ++i)
        check (seen[(size_t) i] == 1,
               "rule " + std::to_string (i) + " appears " + std::to_string (seen[(size_t) i])
                   + " times in menu order");

    // Anything that is not one of the three lands on the default, which is
    // the discipline the modulation matrix uses for an id nobody knows: a
    // setting that is less than it was beats a setting that is wrong.
    const int nonsense[] { -1, 3, 4, 99, -1000, 1000, INT_MIN, INT_MAX };

    for (int stored : nonsense)
        check (MonitorRule::ruleFromStored (stored) == MonitorRule::defaultRule,
               "a stored value of " + std::to_string (stored) + " did not fall back to the default");

    // And the default is the behaviour that was there before this header, so
    // that nobody's monitoring changed when it arrived. Pinned deliberately:
    // changing the shipped default should mean editing this line and saying
    // why, not noticing afterwards.
    check (MonitorRule::defaultRule == Rule::whileArmed,
           "the default rule is no longer the behaviour the studio shipped before it");

    // The fallback has to be a rule the list really has, or an unreadable
    // setting would be worse than the thing it was protecting against.
    check (MonitorRule::ruleFromStored (MonitorRule::ruleToStored (MonitorRule::defaultRule))
               == MonitorRule::defaultRule,
           "the default rule does not round trip");
}

//==============================================================================
/** The default member initialisers, which are the only part of the structs a
    call site can leave out.

    Both exist as insurance against a partial initialiser arriving later, and
    insurance nothing checks is decoration: a mutation of either one survived
    a mutation pass until these two checks were written. */
void theDefaults()
{
    std::printf ("What an unfilled state means\n");

    // Nothing plugged in and nothing asked of it, under every choice and
    // every rule, including the choice that overrides the transport.
    for (auto choice : allChoices)
        for (auto rule : allRules)
            for (int rolling = 0; rolling < 2; ++rolling)
                check (! MonitorRule::inputAudible (choice, rule,
                                                    Transport { .rolling = rolling != 0 },
                                                    Insert {}),
                       std::string ("a default constructed insert was audible under ")
                           + choiceText (choice) + "/" + ruleText (rule));

    // And an unfilled transport is a stopped one, which is the half of the
    // tape machine rule that opens the input.
    check (MonitorRule::inputAudible (Choice::armed, Rule::tapeMachine, Transport {},
                                      Insert { .hasInput = true, .capture = Capture::armed }),
           "a default constructed transport is not a stopped one");
}

//==============================================================================
void theNames()
{
    std::printf ("The names the menu shows\n");

    std::vector<std::string> names;

    for (auto rule : allRules)
    {
        const char* name = MonitorRule::name (rule);
        check (name != nullptr && *name != '\0',
               std::string ("no name for ") + ruleText (rule));

        if (name != nullptr)
            names.push_back (name);
    }

    // Three rules that read as the same thing in the menu would be three
    // rules nobody can choose between, which is the same failure as two
    // rules that behave the same way.
    for (size_t a = 0; a < names.size(); ++a)
        for (size_t b = a + 1; b < names.size(); ++b)
            check (names[a] != names[b], "two rules share the name \"" + names[a] + "\"");
}

//==============================================================================
/** The decision has to be a function of its arguments and nothing else.
    Cheap, and it is what makes the whole table testable away from a device:
    no history, no order dependence, no state behind it. */
void itIsAFunction()
{
    std::printf ("The decision depends on its arguments and nothing else\n");

    const auto states = everyState();
    std::vector<bool> first;
    first.reserve (states.size());

    for (const auto& s : states)
        first.push_back (s.audible());

    // Backwards, so an answer that depended on the order it was asked in, or
    // on anything left behind by the call before, disagrees with itself.
    for (size_t i = states.size(); i-- > 0;)
        check (states[i].audible() == first[i],
               "the answer changed on a second call: " + states[i].text());
}

} // namespace

int main()
{
    std::printf ("MonitorRule.h\n\n");

    theWholeTable();
    theRulesDiffer();
    whatThisIsFor();
    whatIsStored();
    theDefaults();
    theNames();
    itIsAFunction();

    std::printf ("\n");

    if (failures != 0)
    {
        std::printf ("%d check(s) failed.\n", failures);
        return 1;
    }

    std::printf ("All checks passed.\n");
    return 0;
}
