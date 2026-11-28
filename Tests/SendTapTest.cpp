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

//==============================================================================
// The crossfade between the two taps, and the fault it was written twice to fix.
//
// The first version of this in the engine mixed the two source buffers and then
// applied one gain to the blend. That looks equivalent to mixing two resolved
// signals and is not, because the two taps differ in their gain as well as in
// their buffer: `levelTarget` folds the cut into the pre fader branch and not
// the post fader one. So a tap change was two moves at once and the output was
// their product, which is not monotone.
//
// The case it was worst in is the one the engine reaches by default. On a muted
// strip the pre fader gain is zero and the post fader buffer is zero, so both
// steady states are exactly silence, and an audit measured a 5 ms burst at
// -12 dB of the strip's own level going onto the bus. Those are the numbers the
// first three checks below exist to refuse, and the fourth is the general
// statement they are instances of.
void testTheFadeCannotOvershootEitherEndOfItself()
{
    std::printf ("\nThe crossfade between the two taps\n");

    // The endpoints, exactly. Not to within a tolerance: the blocks either side
    // of a fade take the steady path, so a join that was out by a rounding
    // error would be a click of its own at each end.
    check (SendTap::fadeSample (0.7, 0.5, -0.2, 0.25, 0.0) == 0.7 * 0.5,
           "t of nothing has to give back the tap being left, exactly");
    check (SendTap::fadeSample (0.7, 0.5, -0.2, 0.25, 1.0) == -0.2 * 0.25,
           "t of one has to give back the tap being arrived at, exactly");

    // The measured fault. A muted strip at the shipped default: the post fader
    // buffer is silence at a send level of one, and the pre fader buffer is the
    // strip's full signal at a gain of nothing. Both ends are silence, so every
    // point between them has to be silence too.
    {
        int heard = 0;
        double worst = 0.0;

        for (int i = 0; i <= 1000; ++i)
        {
            const double t = (double) i / 1000.0;

            // Both directions, because the burst was measured in both.
            const double out = SendTap::fadeSample (0.0, 1.0, 0.9, 0.0, t);
            const double back = SendTap::fadeSample (0.9, 0.0, 0.0, 1.0, t);

            worst = std::max (worst, std::max (std::fabs (out), std::fabs (back)));

            if (out != 0.0 || back != 0.0)
                ++heard;
        }

        check (heard == 0, "a tap change on a muted strip must be silent at every point of the "
                           "fade, and was audible at " + std::to_string (heard) + " of 1001, "
                           "worst " + str (worst));
    }

    // A partial solo cut, where the two resolved signals are equal and the fade
    // therefore has nothing to do. This is the case the product version
    // overshot by more than 8 dB, because the buffer and the gain were both
    // moving and in opposite directions.
    {
        const double cut = 0.1;          // about -20 dB
        const double level = 0.8;
        const double x = 0.6;            // the strip's own signal

        int moved = 0;

        for (int i = 0; i <= 1000; ++i)
        {
            const double t = (double) i / 1000.0;

            // Post fader: the buffer already carries the cut, the gain is the
            // level. Pre fader: the buffer is the raw signal, the gain is the
            // level times the cut. Two ways of writing the same answer.
            const double out = SendTap::fadeSample (x * cut, level, x, level * cut, t);

            if (std::fabs (out - x * cut * level) > 1.0e-12)
                ++moved;
        }

        check (moved == 0, std::to_string (moved) + " of 1001 points of a fade between two taps "
                           "that resolve to the same signal were not that signal");
    }

    // The general statement, which is what the other three are instances of: a
    // convex combination of two numbers lies between them, so the fade can
    // never be louder than the louder of the two steady states it travels
    // between. Randomised, including signs that differ, because a fade between
    // a positive and a negative sample is where a blend that was not convex
    // would hide.
    {
        std::mt19937 rng (20261124u);
        std::uniform_real_distribution<double> sym (-1.0, 1.0);
        std::uniform_real_distribution<double> unit (0.0, 1.0);

        int over = 0, notMonotone = 0, jumped = 0;

        for (int trial = 0; trial < 20000; ++trial)
        {
            const double fromSample = sym (rng);
            const double toSample   = sym (rng);
            const double fromGain   = unit (rng);
            const double toGain     = unit (rng);

            const double a = fromSample * fromGain;
            const double b = toSample * toGain;
            const double bound = std::max (std::fabs (a), std::fabs (b));

            double previous = a;

            for (int i = 0; i <= 64; ++i)
            {
                const double t = (double) i / 64.0;
                const double out = SendTap::fadeSample (fromSample, fromGain,
                                                        toSample, toGain, t);

                // A tolerance only for the floating point of the multiply, not
                // for any travel outside the pair.
                if (std::fabs (out) > bound + 1.0e-12)
                    ++over;

                // Monotone between the two ends, which is the property the
                // product version failed: its output rose above both ends and
                // came back.
                if (b >= a ? out < previous - 1.0e-12 : out > previous + 1.0e-12)
                    ++notMonotone;

                // And gradual, which is the property a fade exists for and the
                // one the two checks above cannot see: a hard switch at the
                // halfway point satisfies both of them and is exactly the
                // click this is here to remove. No step of a fade in 64 may
                // carry more than a small share of the whole distance.
                if (i > 0 && std::fabs (out - previous) > std::fabs (b - a) * (4.0 / 64.0) + 1.0e-12)
                    ++jumped;

                previous = out;
            }
        }

        check (over == 0, std::to_string (over) + " points of a fade were louder than the louder "
                          "of the two steady states it was travelling between");
        check (notMonotone == 0, std::to_string (notMonotone) + " points of a fade moved away "
                                 "from the tap it was travelling towards");
        check (jumped == 0, std::to_string (jumped) + " steps of a fade carried more of the "
                            "distance than a gradual one can: a fade that steps is the click "
                            "it exists to remove");

        std::printf ("  20000 fades, none louder than either end of itself, none turning back\n");
        std::printf ("    and none of them stepping rather than travelling\n");
        std::printf ("    and a tap change on a muted strip is silent at every point of it\n");
    }
}

// ---------------------------------------------------------------------------
// What the fade does outside its own range.
//
// The header says `t` is not clamped and that going outside the range
// extrapolates rather than saturates. That sentence said "the test sweeps
// outside the range" while no test did, which is the shape of claim CLAUDE.md
// is about: a number or a behaviour stated in a comment with nothing in the
// tree still producing it. The engine cannot currently drive `t` outside the
// range, since `fade` is armed at one and floored at nothing, so this pins a
// property nothing relies on today and the comment now says that too.
//
// Asserted as collinearity rather than by recomputing the weighted sum, which
// would be the implementation restated. A straight line continued is the whole
// claim: equal steps in `t` give equal steps in the output, everywhere, and a
// clamp would flatten them at the ends.
// ---------------------------------------------------------------------------
void testTheFadeExtrapolatesRatherThanSaturating()
{
    std::printf ("\nThe fade outside its own range\n");

    const double fromSample = 0.7, fromGain = 0.5;    // resolves to  0.35
    const double toSample = -0.2, toGain = 0.25;      // resolves to -0.05

    const double a = fromSample * fromGain;
    const double b = toSample * toGain;

    int bent = 0, saturated = 0;

    double previous = SendTap::fadeSample (fromSample, fromGain, toSample, toGain, -2.0);
    double previousStep = 0.0;

    for (int i = -19; i <= 30; ++i)
    {
        const double t = (double) i / 10.0;
        const double out = SendTap::fadeSample (fromSample, fromGain, toSample, toGain, t);

        const double step = out - previous;

        // Equal steps in t have to give equal steps out, which is what makes
        // it one straight line through the whole sweep rather than a line with
        // the ends bent over.
        if (i > -19 && std::fabs (step - previousStep) > 1.0e-12)
            ++bent;

        // And beyond each end it has to have gone past that end, which is the
        // half a clamp would get wrong while still passing the collinearity
        // check on the inside of the range.
        if (t > 1.0 && std::fabs (out - b) <= std::fabs (b - a) * 1.0e-9)
            ++saturated;

        if (t < 0.0 && std::fabs (out - a) <= std::fabs (b - a) * 1.0e-9)
            ++saturated;

        previousStep = step;
        previous = out;
    }

    check (bent == 0, std::to_string (bent) + " steps of a sweep from t of -2 to t of 3 were not "
                      "the same size as the step before: the fade is not one straight line");
    check (saturated == 0, std::to_string (saturated) + " points outside the range sat on an "
                           "endpoint rather than past it: that is a clamp, and the header says "
                           "there is not one");

    // The two the sweep is really about, as named cases, because a reader
    // wanting to know what t of 2 does should find it written down.
    check (SendTap::fadeSample (fromSample, fromGain, toSample, toGain, 2.0) < b,
           "t of two has to be past the tap being arrived at");
    check (SendTap::fadeSample (fromSample, fromGain, toSample, toGain, -1.0) > a,
           "t of minus one has to be past the tap being left");

    std::printf ("  50 points from t of -2 to 3 are one straight line, and the ends are not\n");
    std::printf ("    clamped: t of 2 reads %s against an end of %s\n",
                 str (SendTap::fadeSample (fromSample, fromGain, toSample, toGain, 2.0)).c_str(),
                 str (b).c_str());
}

// ---------------------------------------------------------------------------
// Which end of the fade is which, when it is driven by what is left.
//
// `sendTapFade` is armed at one and counted down, so the engine holds the fade
// that remains and not the progress made, and every caller of `fadeSample` had
// to turn one into the other. `fadeSampleRemaining` is that subtraction, in one
// place, and the reason it is worth having its own function is that getting it
// backwards is invisible to every other check in this file: the output stays
// convex, stays monotone, stays exact at both ends and never oversteps. It just
// runs the five milliseconds the wrong way round, so the join the fade exists
// to remove is moved to the far end of it rather than removed.
//
// So this names the two ends by what they are rather than by their arithmetic:
// a whole fade still to run is the tap being LEFT, and none left is the tap
// being ARRIVED AT.
// ---------------------------------------------------------------------------
void testTheFadeRunsFromTheTapBeingLeftToTheOneBeingArrivedAt()
{
    std::printf ("\nThe direction of a fade driven by what is left of it\n");

    const double leaving = 0.8, leavingGain = 0.5;      // resolves to 0.40
    const double arriving = 0.3, arrivingGain = 0.25;   // resolves to 0.075

    // Exactly, for the reason the endpoints are exact in the test above: the
    // blocks either side of a fade take the steady path.
    check (SendTap::fadeSampleRemaining (leaving, leavingGain, arriving, arrivingGain, 1.0)
             == leaving * leavingGain,
           "a whole fade still to run has to give back the tap being left, exactly");
    check (SendTap::fadeSampleRemaining (leaving, leavingGain, arriving, arrivingGain, 0.0)
             == arriving * arrivingGain,
           "no fade left has to give back the tap being arrived at, exactly");

    // And it has to travel from the first of those to the second as the fade
    // counts down, which is the assertion an inverted subtraction fails and
    // nothing else in this file can see. Swept the way the engine drives it:
    // from a full fade down to nothing.
    {
        int wrongWay = 0;
        double previous = leaving * leavingGain;

        for (int i = 64; i >= 0; --i)
        {
            const double remaining = (double) i / 64.0;
            const double out = SendTap::fadeSampleRemaining (leaving, leavingGain,
                                                             arriving, arrivingGain, remaining);

            // The tap being arrived at resolves below the one being left
            // here, so every step of a countdown has to be downward. A
            // reversed drive starts at the far end and climbs back.
            if (out > previous + 1.0e-12)
                ++wrongWay;

            previous = out;
        }

        check (wrongWay == 0, std::to_string (wrongWay) + " samples of a fade counted down from "
                              "one to nothing moved towards the tap it was leaving rather than "
                              "the one it was arriving at");
    }

    // The relationship to the function underneath, which is the one line this
    // wrapper is, swept over both arguments so that a wrapper that dropped the
    // subtraction entirely is caught as well as one that inverted it.
    {
        std::mt19937 rng (20261128u);
        std::uniform_real_distribution<double> sym (-1.0, 1.0);
        std::uniform_real_distribution<double> unit (0.0, 1.0);

        int disagreed = 0;

        for (int trial = 0; trial < 20000; ++trial)
        {
            const double fs = sym (rng), ts = sym (rng);
            const double fg = unit (rng), tg = unit (rng);
            const double remaining = unit (rng);

            if (SendTap::fadeSampleRemaining (fs, fg, ts, tg, remaining)
                  != SendTap::fadeSample (fs, fg, ts, tg, 1.0 - remaining))
                ++disagreed;
        }

        check (disagreed == 0, std::to_string (disagreed) + " of 20000 blends disagreed with the "
                               "same blend driven by the progress made");

        std::printf ("  20000 blends, and a whole fade left reads %s, the tap being left,\n",
                     str (leaving * leavingGain).c_str());
        std::printf ("    while none left reads %s, the one being arrived at\n",
                     str (arriving * arrivingGain).c_str());
    }
}

// ---------------------------------------------------------------------------
// The pre fader copy, which may never be declined to something about to read
// it.
//
// `processInsert` takes one block of two channels before the fader multiply,
// and only when a send on the insert is going to read it, so an insert with no
// pre fader send pays nothing. That makes the guard and the read two halves of
// one decision, and the engine's own comment says what a guard narrower than
// the reads produces: `routeSends` reading a buffer `processInsert` did not
// fill this block, which is this insert's audio from an earlier block. A few
// milliseconds of the right strip at the wrong time, with nothing to connect
// it to any control.
//
// The two predicates differ by one clause because they are asked either side
// of the moment a tap change is written down, and `routeSends` is what writes
// it down. So the check that matters is not either formula: it is the
// implication ACROSS that transition, which relates the two functions to each
// other rather than to themselves.
//
// Stated honestly, because the three checks below are not equally strong:
//
//   * The transition implication is the real one. It is the only check here
//     that compares two different functions, and it is what fails when the
//     anticipating clause is dropped.
//   * The guard covering the reads is the guard restated from the two places
//     the buffer is read, so it is worth exactly this much: it fails when the
//     guard moves and the reads do not. Nothing can catch both moving
//     together.
//   * Nothing pre fader anywhere must want no copy, which is what fails when
//     a guard is left stuck at true.
//
// And all three are pointed at deliberately wrong twins of both predicates and
// required to reject them, because a sweep that catches nothing is
// indistinguishable from code with no bugs.
// ---------------------------------------------------------------------------

// Every state the two can be asked about. `fade` is in three places rather
// than two because a fade part way through is a different case from a fade
// just armed, and the shipped predicates only test it against nothing.
struct SendState
{
    SendTap::Tap tap, activeTap, fadeFrom;
    double fade;
};

std::vector<SendState> everySendState()
{
    std::vector<SendState> states;

    for (auto tap : bothTaps)
        for (auto active : bothTaps)
            for (auto from : bothTaps)
                for (double fade : { 0.0, 0.5, 1.0 })
                    states.push_back ({ tap, active, from, fade });

    return states;
}

// What routeSends does to the state before it acts on it, when the tap has
// changed and the destination has not: the tap it was running at becomes the
// tap it is fading from, the fade is armed, and the active tap moves on. Taken
// from AudioEngine::routeSends rather than invented here.
SendState afterRouteSendsNoticesTheChange (SendState before)
{
    if (before.tap != before.activeTap)
    {
        before.fadeFrom  = before.activeTap;
        before.fade      = 1.0;
        before.activeTap = before.tap;
    }

    return before;
}

// Which buffer routeSends reads, modelled from the two reads themselves: the
// tap it is heading for, and, while a fade is running, the tap it is fading
// from. Both of those are `sourceIsPreFader` calls in the engine.
bool routeSendsReadsThePreFaderCopy (const SendState& s)
{
    return SendTap::sourceIsPreFader (s.tap)
        || (s.fade > 0.0 && SendTap::sourceIsPreFader (s.fadeFrom));
}

void testThePreFaderCopyIsNeverDeclinedToSomethingAboutToReadIt()
{
    std::printf ("\nThe pre fader copy, and the two questions that are not the same question\n");

    const auto states = everySendState();

    // The shipped pair, and three wrong twins. Each twin is one plausible
    // mistake: a clause left out, or a guard that never refuses.
    const auto beforeShipped = [] (const SendState& s)
    {
        return SendTap::preFaderCopyWantedBeforeRouting (s.tap, s.activeTap, s.fade, s.fadeFrom);
    };

    const auto whileShipped = [] (const SendState& s)
    {
        return SendTap::preFaderCopyWantedWhileRouting (s.tap, s.fade, s.fadeFrom);
    };

    // Wrong twin one: the anticipating clause dropped, which is the mistake
    // the two predicates looking alike invites.
    const auto beforeWithoutAnticipation = [] (const SendState& s)
    {
        return SendTap::sourceIsPreFader (s.tap)
            || (s.fade > 0.0 && SendTap::sourceIsPreFader (s.fadeFrom));
    };

    // Wrong twin two: the running fade dropped instead.
    const auto beforeWithoutTheFade = [] (const SendState& s)
    {
        return SendTap::sourceIsPreFader (s.tap) || SendTap::sourceIsPreFader (s.activeTap);
    };

    // Wrong twin three: a guard that never refuses, which costs nothing
    // audible and is still not what the header claims.
    const auto alwaysWants = [] (const SendState&) { return true; };

    // ---- the real check: nothing may be declined to something about to read
    // it, across the transition routeSends performs.
    const auto countsDeclined = [&states] (auto before, auto whileRouting)
    {
        int declined = 0;

        for (const auto& s : states)
        {
            const auto after = afterRouteSendsNoticesTheChange (s);

            if (! routeSendsReadsThePreFaderCopy (after))
                continue;

            // One state, one count, whichever of the two guards refused it.
            if (! before (s) || ! whileRouting (after))
                ++declined;
        }

        return declined;
    };

    const int declined = countsDeclined (beforeShipped, whileShipped);

    check (declined == 0, std::to_string (declined) + " of " + std::to_string (states.size())
                          + " send states would have read a pre fader copy that one of the two "
                            "guards declined to take");

    // The twins have to be caught, or the check above says nothing.
    check (countsDeclined (beforeWithoutAnticipation, whileShipped) > 0,
           "dropping the anticipating clause has to be caught: the block a send leaves the pre "
           "fader tap is the block processInsert has not been told about yet");
    check (countsDeclined (beforeWithoutTheFade, whileShipped) > 0,
           "dropping the running fade has to be caught: the fade off the pre fader tap outlives "
           "the block the tap changed in");

    // ---- the guard covering the reads, which is the guard restated and is
    // worth what the comment above says it is worth.
    {
        int mismatched = 0;

        for (const auto& s : states)
            if (whileShipped (s) != routeSendsReadsThePreFaderCopy (s))
                ++mismatched;

        check (mismatched == 0, std::to_string (mismatched) + " send states had routeSends' guard "
                                "and routeSends' reads disagreeing about the pre fader copy");
    }

    // ---- and a copy nobody will read is not taken, which is what fails when
    // a guard is left stuck at true.
    {
        const SendState nothingPreFader { SendTap::Tap::postFader, SendTap::Tap::postFader,
                                          SendTap::Tap::postFader, 0.0 };

        check (! beforeShipped (nothingPreFader),
               "an insert whose sends are all post fader, with nothing fading, must not pay for "
               "a copy");
        check (! whileShipped (nothingPreFader),
               "and routeSends must not ask for one either");
        check (alwaysWants (nothingPreFader) && ! routeSendsReadsThePreFaderCopy (nothingPreFader),
               "the twin that never refuses has to be wrong in this state, or the two checks "
               "above are rejecting nothing");
    }

    std::printf ("  %zu states, none of them reading a copy that was declined,\n", states.size());
    std::printf ("    and three wrong twins of the pair rejected\n");
}

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
    testTheFadeCannotOvershootEitherEndOfItself();
    testTheFadeExtrapolatesRatherThanSaturating();
    testTheFadeRunsFromTheTapBeingLeftToTheOneBeingArrivedAt();
    testThePreFaderCopyIsNeverDeclinedToSomethingAboutToReadIt();

    std::printf ("\n");

    if (failures != 0)
    {
        std::printf ("%d check(s) failed.\n", failures);
        return 1;
    }

    std::printf ("All checks passed.\n");
    return 0;
}
