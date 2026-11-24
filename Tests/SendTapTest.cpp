// ---------------------------------------------------------------------------
// Checks where a send is tapped off a strip: SendTap.h.
//
// None of this can announce that it is wrong. Every function in here either
// returns a number a project file carries, or a number a send's gain ramp
// heads for, and a mistake in any of them loads, plays and sounds plausible.
// So the properties below are the claims the header makes in prose, asserted
// directly, rather than the arithmetic that happens to implement them.
//
// Four of them are worth more than the rest:
//
//   * The stored numbers are pinned as literals, and every other integer an
//     `.ahp` could hold reads as the default. Renumbering the taps would
//     leave every saved project sending from the wrong point, still loading
//     and still playing: there is nothing to hear except a reverb that is
//     suddenly following a fader it never followed before.
//   * A post fader send's target cannot be touched by the cut argument, for
//     any value of it whatever. The cut is already in the buffer that send is
//     copied out of, so folding it in here as well arrives at its square, and
//     `SoloGraph.h`'s WHAT A PARTIAL CUT DOES TO A WET PATH is what that
//     costs: a wet path collapsing instead of coming down with the mix.
//   * A number the header cannot make sense of reads as silence, and not as
//     the nearest end of the range. Every comparison against a NaN is false,
//     so a clamp hands the NaN straight back, and unity is not an acceptable
//     fallback for an unreadable cut because that is a mute that does
//     nothing. Both of those wrong answers are implemented below and the
//     sweep that rejects the real ones is required to reject them, because a
//     sweep that catches nothing is indistinguishable from code with no bugs.
//   * The names and the behaviour come from one place. A menu that says
//     "post fader (post pan)" over a tap that carries neither is worse than
//     either mistake on its own, so `carriesFader` and `carriesPan` are
//     checked against what `displayName` claims rather than only against
//     themselves.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include "SendTap.h"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace
{

int failures = 0;

// Capped, because a property swept over tens of thousands of values can fail
// that many times and only the first few lines are ever read. The count stays
// complete and is what the exit code comes from.
void fail (const std::string& what)
{
    ++failures;

    if (failures <= 20)
        std::printf ("  FAIL  %s\n", what.c_str());
    else if (failures == 21)
        std::printf ("  ... further failures not printed\n");
}

void check (bool ok, const std::string& what)
{
    if (! ok)
        fail (what);
}

const SendTap::Tap bothTaps[] = { SendTap::Tap::postFader, SendTap::Tap::preFader };

// Deliberately not `displayName`: that function is one of the things under
// test, so a failure message built from it would be describing the mutation
// rather than the case.
const char* which (SendTap::Tap tap)
{
    return tap == SendTap::Tap::preFader ? "preFader" : "postFader";
}

std::string str (double v)
{
    char buffer[64];
    std::snprintf (buffer, sizeof (buffer), "%.17g", v);
    return std::string (buffer);
}

// ---------------------------------------------------------------------------
// The values a level or a cut can arrive as.
//
// `unreadable` is every reading the header refuses: not a number, below
// nothing, and above unity. Note what is NOT in it: a negative zero is a
// perfectly good silence, because `-0.0 >= 0.0` is true, and putting it in
// here would be asserting that the header refuses a value it is right to
// accept.
// ---------------------------------------------------------------------------
const double unreadable[] =
{
    std::numeric_limits<double>::quiet_NaN(),
    -std::numeric_limits<double>::quiet_NaN(),
    std::numeric_limits<double>::infinity(),
    -std::numeric_limits<double>::infinity(),
    -1.0, -0.5, -1.0e-300, -1.0e30,
    1.0 + 1.0e-12, 1.0000001, 1.5, 2.0, 1.0e9
};

const double readable[] =
{
    0.0, -0.0, 1.0e-300, 1.0e-6, 0.125, 0.25, 0.5, 0.7071067811865476,
    0.9999999, 1.0
};

// ---------------------------------------------------------------------------
// What a pre fader send's target has to be, modelled rather than restated.
//
// The engine's post fader send is copied out of a buffer the cut has already
// been multiplied into, and then the send's own level is applied: that is one
// cut and one level, in some order. A pre fader send is copied out of a buffer
// nothing has reached, so for it to carry the same cut once, and once only,
// the target here has to land where that buffer would have put it.
// ---------------------------------------------------------------------------
double asIfTheCutHadBeenInTheBuffer (double bufferGain, double sendLevel)
{
    return bufferGain * sendLevel;
}

// ---------------------------------------------------------------------------
// Two wrong implementations, kept so the sweep can be shown to tell them from
// the right one. Both are what somebody tidying up the header would write.
// ---------------------------------------------------------------------------
double clampingLevelTarget (SendTap::Tap tap, double sendLevel, double cut)
{
    const auto clamp = [] (double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); };

    return tap == SendTap::Tap::preFader ? clamp (sendLevel) * clamp (cut)
                                         : clamp (sendLevel);
}

double unityFallbackLevelTarget (SendTap::Tap tap, double sendLevel, double cut)
{
    if (! (sendLevel >= 0.0 && sendLevel <= 1.0))
        return 0.0;

    if (tap != SendTap::Tap::preFader)
        return sendLevel;

    return ! (cut >= 0.0 && cut <= 1.0) ? sendLevel : sendLevel * cut;
}

double cutTwiceLevelTarget (SendTap::Tap, double sendLevel, double cut)
{
    if (! (sendLevel >= 0.0 && sendLevel <= 1.0))
        return 0.0;

    if (! (cut >= 0.0 && cut <= 1.0))
        return 0.0;

    // The tidy version: one expression for both taps, which quietly folds the
    // cut into a send that was copied out of a buffer already carrying it.
    return sendLevel * cut;
}

using LevelTarget = double (*) (SendTap::Tap, double, double);

double realLevelTarget (SendTap::Tap tap, double sendLevel, double cut)
{
    return SendTap::levelTarget (tap, sendLevel, cut);
}

// ---------------------------------------------------------------------------
// The sweeps, written against a function pointer so each can be pointed at
// something known to be wrong and required to notice.
// ---------------------------------------------------------------------------

/** A number this cannot make sense of reads as exactly silence. */
int silenceViolations (LevelTarget f)
{
    int bad = 0;

    for (auto tap : bothTaps)
    {
        for (double level : unreadable)
            for (double cut : unreadable)
                if (! (f (tap, level, cut) == 0.0))
                    ++bad;

        for (double level : unreadable)
            for (double cut : readable)
                if (! (f (tap, level, cut) == 0.0))
                    ++bad;
    }

    // An unreadable cut only reaches the arithmetic on the pre fader branch,
    // which is the whole of the next property, so this half is pre fader only.
    for (double level : readable)
        for (double cut : unreadable)
            if (! (f (SendTap::Tap::preFader, level, cut) == 0.0))
                ++bad;

    return bad;
}

/** The cut argument cannot reach the post fader branch at all. */
int postFaderViolations (LevelTarget f)
{
    int bad = 0;

    std::mt19937 rng (20261124u);
    std::uniform_real_distribution<double> unit (0.0, 1.0);

    std::vector<double> cuts;

    for (double c : readable)   cuts.push_back (c);
    for (double c : unreadable) cuts.push_back (c);

    for (int i = 0; i < 400; ++i)
        cuts.push_back (unit (rng) * 4.0 - 2.0);

    for (double level : readable)
        for (double cut : cuts)
            if (! (f (SendTap::Tap::postFader, level, cut) == level))
                ++bad;

    for (int i = 0; i < 4000; ++i)
    {
        const double level = unit (rng);
        const double cut = cuts[(std::size_t) (unit (rng) * (double) (cuts.size() - 1))];

        if (! (f (SendTap::Tap::postFader, level, cut) == level))
            ++bad;
    }

    return bad;
}

/** A pre fader send carries the cut once: not twice, and not at all. */
int preFaderViolations (LevelTarget f)
{
    int bad = 0;

    std::mt19937 rng (7778889u);
    std::uniform_real_distribution<double> unit (0.0, 1.0);

    for (int i = 0; i < 20000; ++i)
    {
        const double level = i % 7 == 0 ? readable[(std::size_t) i % (sizeof (readable) / sizeof (readable[0]))]
                                        : unit (rng);
        const double cut = i % 5 == 0 ? readable[(std::size_t) i % (sizeof (readable) / sizeof (readable[0]))]
                                      : unit (rng);

        const double got = f (SendTap::Tap::preFader, level, cut);

        if (got != asIfTheCutHadBeenInTheBuffer (cut, level))
            ++bad;

        // A send cannot arrive louder than the insert it was taken from, and
        // a cut cannot make it louder either. Both are properties of the
        // answer rather than of the expression.
        if (! (got <= level + 1.0e-15) || got < 0.0)
            ++bad;
    }

    return bad;
}

// ---------------------------------------------------------------------------

void testTheStoredNumbersAreThoseTwoNumbers()
{
    std::printf ("What an .ahp carries\n");

    // Literals, deliberately, and not read back out of the header. These two
    // numbers are the only thing standing between a project saved today and
    // the same project opened by a version that has added a tap.
    check (SendTap::tapToStored (SendTap::Tap::postFader) == 0,
           "post fader must store as 0");
    check (SendTap::tapToStored (SendTap::Tap::preFader) == 1,
           "pre fader must store as 1");

    check (SendTap::tapFromStored (0) == SendTap::Tap::postFader, "0 must read as post fader");
    check (SendTap::tapFromStored (1) == SendTap::Tap::preFader, "1 must read as pre fader");

    for (auto tap : bothTaps)
        check (SendTap::tapFromStored (SendTap::tapToStored (tap)) == tap,
               std::string ("a round trip through the stored value must return ") + which (tap));

    // The stored values are a contiguous run from zero, which is what lets a
    // later tap take the next number rather than having to find a hole.
    check (SendTap::numTaps == 2, "there are two taps");

    for (int stored = 0; stored < SendTap::numTaps; ++stored)
        check (SendTap::tapToStored (SendTap::tapFromStored (stored)) == stored,
               "stored value " + std::to_string (stored) + " must survive a round trip");

    // Every other integer a file could hold. A pre FX tap is documented as
    // taking 2, so 2 in particular must read as the default here and not as
    // something this version half understands.
    int wrong = 0;
    int swept = 0;

    const auto sweep = [&] (int stored)
    {
        if (stored == 0 || stored == 1)
            return;

        ++swept;

        if (SendTap::tapFromStored (stored) != SendTap::defaultTap)
            ++wrong;
    };

    for (int stored = -4000; stored <= 4000; ++stored)
        sweep (stored);

    sweep (INT_MIN);
    sweep (INT_MIN + 1);
    sweep (INT_MIN / 2);
    sweep (INT_MAX);
    sweep (INT_MAX - 1);
    sweep (INT_MAX / 2);

    std::mt19937 rng (424242u);
    std::uniform_int_distribution<int> any (INT_MIN, INT_MAX);

    for (int i = 0; i < 200000; ++i)
        sweep (any (rng));

    check (wrong == 0, std::to_string (wrong) + " of " + std::to_string (swept)
                         + " unrecognised stored values did not read as the default");

    check (SendTap::tapFromStored (2) == SendTap::defaultTap,
           "2 is reserved for a pre FX tap and must read as the default until there is one");

    std::printf ("  0 and 1 pinned, %d other stored values all read as the default\n", swept);
}

void testTheDefaultIsWhatEveryProjectWasAlreadyDoing()
{
    std::printf ("\nThe default tap\n");

    check (SendTap::defaultTap == SendTap::Tap::postFader,
           "the default tap must be post fader, which is what every project saved before the "
           "choice existed was doing");

    // A value initialised atomic in the engine, a fresh insert, a cleared
    // mixer and an attribute that is not in the file at all have to arrive at
    // one answer, and this is what makes them agree without four separate
    // decisions.
    check (SendTap::Tap{} == SendTap::defaultTap,
           "a value initialised Tap must be the default tap");

    check (SendTap::tapToStored (SendTap::Tap{}) == 0,
           "a value initialised Tap must store as the stored value of the default");

    std::printf ("  post fader, and a value initialised Tap agrees with it\n");
}

void testTheNamesAndTheBehaviourComeFromOnePlace()
{
    std::printf ("\nWhat the control says against what the tap does\n");

    const char* post = SendTap::displayName (SendTap::Tap::postFader);
    const char* pre  = SendTap::displayName (SendTap::Tap::preFader);

    check (post != nullptr && pre != nullptr, "both taps need a name");

    if (post == nullptr || pre == nullptr)
        return;

    check (std::strlen (post) > 0 && std::strlen (pre) > 0, "neither name may be empty");
    check (std::strcmp (post, pre) != 0, "the two taps must not share a name");

    // Reaper's names assert the behaviour in their two halves, so the name is
    // checked against the behaviour rather than against itself. A swapped
    // pair of names, or an inverted `carriesFader`, is a menu that lies about
    // which tap follows the fader, and nothing downstream of it can tell.
    for (auto tap : bothTaps)
    {
        const std::string name = SendTap::displayName (tap);
        const bool saysPreFader = name.find ("pre fader") != std::string::npos;
        const bool saysPostPan  = name.find ("post pan") != std::string::npos;

        check (saysPreFader != SendTap::carriesFader (tap),
               std::string ("the name \"") + name + "\" disagrees with whether that tap carries "
                 "the fader");
        check (saysPostPan == SendTap::carriesPan (tap),
               std::string ("the name \"") + name + "\" disagrees with whether that tap carries "
                 "the pan");
    }

    check (SendTap::carriesFader (SendTap::Tap::postFader),
           "a post fader send is copied out of the faded buffer, so it carries the fader");
    check (SendTap::carriesPan (SendTap::Tap::postFader),
           "a post fader send is copied out of the panned buffer, so it carries the pan");
    check (! SendTap::carriesFader (SendTap::Tap::preFader),
           "a pre fader send that followed the fader would be a post fader send");
    check (! SendTap::carriesPan (SendTap::Tap::preFader),
           "no studio reads a post pan buffer for a pre fader send");

    // The short form is what a mixer row has space for and is therefore the
    // string somebody actually reads while mixing, so it is held to the same
    // standard as the long one rather than being left as two literals at the
    // control. Required to be the opening of the long name, which is what stops
    // the pair drifting into describing different taps: "pre" over a tap whose
    // full name begins "post fader" is the lie this check exists to refuse.
    for (auto tap : bothTaps)
    {
        const std::string full  = SendTap::displayName (tap);
        const std::string brief = SendTap::shortName (tap);

        check (! brief.empty(), "a tap's short name may not be empty");
        check (full.rfind (brief, 0) == 0,
               std::string ("the short name \"") + brief + "\" is not the opening of \""
                 + full + "\"");
    }

    check (std::strcmp (SendTap::shortName (SendTap::Tap::postFader),
                        SendTap::shortName (SendTap::Tap::preFader)) != 0,
           "the two taps must not share a short name either");

    // Which buffer the mixing loop copies out of. Pinned here because it is the
    // one decision in the whole feature that could have been left as a ternary
    // in code no test can compile: inverted, both buffers are the right strip
    // at a plausible level and the only symptom is a send following a fader it
    // was told not to follow.
    check (SendTap::sourceIsPreFader (SendTap::Tap::preFader),
           "a pre fader send has to be copied out of the pre fader buffer");
    check (! SendTap::sourceIsPreFader (SendTap::Tap::postFader),
           "a post fader send has to be copied out of the insert's own output");

    for (auto tap : bothTaps)
        check (SendTap::sourceIsPreFader (tap) != SendTap::carriesFader (tap),
               "the buffer a tap reads and whether it carries the fader are the same question "
               "asked from two ends, and they have disagreed");

    std::printf ("  \"%s\" and \"%s\", each agreeing with what it carries,\n", post, pre);
    std::printf ("    with \"%s\" and \"%s\" on the row and the buffer each one reads\n",
                 SendTap::shortName (SendTap::Tap::postFader),
                 SendTap::shortName (SendTap::Tap::preFader));
}

void testWhichTapCarriesTheStripCut()
{
    std::printf ("\nWhether the strip's cut reaches the send\n");

    for (bool setting : { false, true })
    {
        // A post fader send is copied out of the buffer the cut was applied
        // to. There is no way for it not to carry it, so the setting cannot
        // reach this answer: without it, a guitar silenced by a vocal solo
        // still feeds the reverb bus the solo is keeping audible.
        check (SendTap::carriesStripCut (SendTap::Tap::postFader, setting),
               std::string ("a post fader send always carries the strip cut, setting ")
                 + (setting ? "on" : "off"));

        check (SendTap::carriesStripCut (SendTap::Tap::preFader, setting) == setting,
               std::string ("the pre fader tap must follow the setting, which is ")
                 + (setting ? "on" : "off"));
    }

    std::printf ("  post fader always, pre fader by the setting\n");
}

void testThePostFaderBranchIgnoresTheCut()
{
    std::printf ("\nA post fader send against the cut argument\n");

    const int bad = postFaderViolations (realLevelTarget);

    check (bad == 0, std::to_string (bad)
                       + " post fader targets were moved by the cut argument");

    // The claim is only worth the paper if the sweep can see the mistake it
    // is about. `cutTwiceLevelTarget` is the tidy version that applies the
    // cut on both branches, which is the squared wet path.
    const int caught = postFaderViolations (cutTwiceLevelTarget);

    check (caught > 0, "the sweep failed to notice a post fader branch that folds the cut in");

    // And the specific arithmetic, named: at a quarter cut a post fader send
    // is at its level, not at a quarter of it and not at a sixteenth.
    const double target = SendTap::levelTarget (SendTap::Tap::postFader, 0.5, 0.25);

    check (target == 0.5, "a post fader send at level 0.5 under a quarter cut is still 0.5, got "
                            + str (target));

    std::printf ("  no cut, readable or otherwise, moved a post fader target; the version that "
                 "folds it in is caught %d times\n", caught);
}

void testThePreFaderBranchCarriesTheCutOnce()
{
    std::printf ("\nA pre fader send against the cut\n");

    const int bad = preFaderViolations (realLevelTarget);

    check (bad == 0, std::to_string (bad) + " pre fader targets did not carry the cut once");

    // The two taps differ by the cut and by nothing else, so at no cut at all
    // they have to agree exactly.
    int disagreed = 0;

    for (double level : readable)
        if (SendTap::levelTarget (SendTap::Tap::preFader, level, 1.0)
              != SendTap::levelTarget (SendTap::Tap::postFader, level, 1.0))
            ++disagreed;

    check (disagreed == 0, "at a cut of unity the two taps must arrive at the same place");

    // Silence at the source is silence at the send, which is the whole point
    // of the setting that puts the cut here.
    for (double level : readable)
        check (SendTap::levelTarget (SendTap::Tap::preFader, level, 0.0) == 0.0,
               "a fully cut strip must feed its pre fader send nothing");

    // And not the square, named, because that is the near miss.
    const double quarter = SendTap::levelTarget (SendTap::Tap::preFader, 0.5, 0.25);

    check (quarter == 0.125, "0.5 of a strip cut to a quarter is 0.125, got " + str (quarter));
    check (quarter != 0.5 * 0.25 * 0.25,
           "a pre fader target must not carry the cut twice");

    // Monotone in the cut: riding a solo mute gain down may not make a send
    // louder anywhere.
    int notMonotone = 0;

    for (double level : readable)
    {
        double previous = -1.0;

        for (int step = 0; step <= 1000; ++step)
        {
            const double got = SendTap::levelTarget (SendTap::Tap::preFader, level,
                                                     (double) step / 1000.0);

            if (got < previous)
                ++notMonotone;

            previous = got;
        }
    }

    check (notMonotone == 0, "the pre fader target must not fall as the cut rises");

    std::printf ("  20000 levels and cuts, the cut applied exactly once\n");
}

void testANumberItCannotReadIsSilence()
{
    std::printf ("\nA level or a cut the header cannot make sense of\n");

    const int bad = silenceViolations (realLevelTarget);

    check (bad == 0, std::to_string (bad)
                       + " unreadable values gave something other than exact silence");

    // Named, because these are the two wrong readings somebody would reach
    // for, and both of them are quiet about it.
    const int clamped = silenceViolations (clampingLevelTarget);
    const int unity   = silenceViolations (unityFallbackLevelTarget);

    check (clamped > 0, "the sweep failed to notice a clamp instead of a refusal");
    check (unity > 0, "the sweep failed to notice unity standing in for an unreadable cut");

    // The discriminating cases, spelled out. A clamp and a refusal agree
    // below zero and disagree above unity, and a NaN gets past a clamp
    // untouched because every comparison against it is false.
    check (SendTap::levelTarget (SendTap::Tap::postFader, 2.0, 1.0) == 0.0,
           "a level above unity reads as silence, not as unity");
    check (SendTap::levelTarget (SendTap::Tap::preFader, 0.5, 2.0) == 0.0,
           "a cut above unity reads as silence, not as unity");
    check (SendTap::levelTarget (SendTap::Tap::preFader, 0.5,
                                 std::numeric_limits<double>::quiet_NaN()) == 0.0,
           "a cut that is not a number reads as silence, and a clamp would hand it back");

    // Unity in particular is not the fallback for an unreadable cut: that is
    // a mute that does nothing, which is the failure the whole setting exists
    // to prevent.
    for (double cut : unreadable)
        check (SendTap::levelTarget (SendTap::Tap::preFader, 0.8, cut) != 0.8,
               "an unreadable cut must not leave the send at full level");

    // A negative zero is a readable silence rather than an unreadable number,
    // and so is a denormal. Refusing those would be a bug of the same shape
    // in the other direction.
    check (SendTap::levelTarget (SendTap::Tap::preFader, 0.5, -0.0) == 0.0,
           "a negative zero cut is silence");
    check (SendTap::levelTarget (SendTap::Tap::postFader, -0.0, 1.0) == 0.0,
           "a negative zero level is silence");
    check (SendTap::levelTarget (SendTap::Tap::preFader, 1.0, 1.0) == 1.0,
           "a cut of exactly unity and a level of exactly unity are both in range");

    std::printf ("  every unreadable value is exact silence; a clamp is caught %d times and a "
                 "unity fallback %d times\n", clamped, unity);
}

} // namespace

int main()
{
    std::printf ("SendTap\n=======\n\n");

    testTheStoredNumbersAreThoseTwoNumbers();
    testTheDefaultIsWhatEveryProjectWasAlreadyDoing();
    testTheNamesAndTheBehaviourComeFromOnePlace();
    testWhichTapCarriesTheStripCut();
    testThePostFaderBranchIgnoresTheCut();
    testThePreFaderBranchCarriesTheCutOnce();
    testANumberItCannotReadIsSilence();

    std::printf ("\n");

    if (failures != 0)
    {
        std::printf ("%d check(s) failed.\n", failures);
        return 1;
    }

    std::printf ("All checks passed.\n");
    return 0;
}
