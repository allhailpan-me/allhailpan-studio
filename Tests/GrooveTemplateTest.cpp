// ---------------------------------------------------------------------------
// Checks a groove taken from one performance and applied to another.
//
// The claim this file has to hold up is not that GrooveTemplate.h is self
// consistent. It is that the thing it calls a feel is the thing the rest of
// the world calls one, and that putting a feel on a part does what the
// manuals say and nothing else.
//
// Four of these would fail quietly, and they are the reason the file is as
// long as it is:
//
//   - The factor of two. A swing percentage is a position within a PAIR of
//     slots, so a swing of 0.62 is 0.12 of a pair and 0.24 of one slot. Get
//     that wrong and the result still looks like swing, still sounds like
//     swing, and is half the swing it claims. The round trip below pins it:
//     swing a part with the parametric groove, read a template back off the
//     result, apply that template to the original, and the two must agree to
//     the last bit. That single check fixes the units, the sign, the slot
//     pairing and the factor all at once.
//   - The model. Deviation based, following Ableton, rather than target
//     based, following everybody else. The difference is invisible on a part
//     that was programmed and is the whole feature on one that was played, so
//     there is a check that asserts the model directly rather than leaving it
//     to a comment somebody can change.
//   - The wrap. A template shorter than the part repeats, and the slot is
//     chosen from the note's own grid step. An off by one there is correct in
//     the first repetition and wrong in every one after it, which is exactly
//     the kind of thing a quick listen misses.
//   - The accent reference. Velocity is applied as a deviation from the
//     template's own mean, which is a derived number held as a member. A held
//     derived number is one that can go stale, so the invariant is asserted
//     after both of the two ways slots can be set rather than trusted.
//
// GrooveTemplate.h and Groove.h have no JUCE in them, so this builds and runs
// on its own with sanitizers in a couple of seconds.
//
//   ./Tests/run.sh
//
// Sources, all manufacturers' own documentation:
//   Ableton Reference Manual, Live 12, chapter 14 "Using Grooves", for the
//   deviation model, for the velocity slider being measured against the
//   groove's own content, and for extraction reading timing and volume.
//   Apple, Logic Pro User Guide, "Create groove templates", for the rule that
//   where several notes share a position only the first is evaluated.
//   Cakewalk Documentation, "Quantizing", for the window geometry and for the
//   four documented answers to what happens to a note that is not near a
//   template point.
//   WC Music Research, README.DNA, for what a groove template file holds.
//   Akai MPC software user guide, Timing Correct, for the swing percentage
//   this has to keep agreeing with.
// ---------------------------------------------------------------------------

#include "Groove.h"
#include "GrooveTemplate.h"

#include <algorithm>
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

static void checkClose (double a, double b, double tol, const std::string& what)
{
    if (! (std::abs (a - b) <= tol))
        fail (what + "  (" + std::to_string (a) + " vs " + std::to_string (b)
                    + ", off by " + std::to_string (std::abs (a - b)) + ")");
}

static GrooveTemplate::SourceNote srcNote (double beat, float velocity = 0.8f, int pitch = 60)
{
    GrooveTemplate::SourceNote n;
    n.beat = beat;
    n.velocity = velocity;
    n.pitch = pitch;
    return n;
}

// The mean of the filled slots, worked out here rather than read back from
// the thing being tested.
static double meanOfFilled (const GrooveTemplate& t)
{
    double sum = 0.0;
    int    hits = 0;

    for (int i = 0; i < t.numSlots(); ++i)
        if (const auto s = t.slotFor (i); s.filled)
        {
            sum += (double) s.velocity;
            ++hits;
        }

    return hits > 0 ? sum / (double) hits : 0.0;
}

//=============================================================================
// The round trip that pins the units, the sign and the factor of two.
//
// A part swung by the parametric groove, read back as a template, must put a
// straight part in exactly the same places. Anything wrong with the slot
// pairing, the direction of the deviation, or the pair against slot factor
// shows up here as a position that is out by a visible amount rather than by
// a rounding error.
//=============================================================================
static void aSwungPartReadsBackAsThatSwing()
{
    const struct { Groove::Base base; double step; const char* name; } bases[] =
    {
        { Groove::Base::eighth,       0.5,   "1/8"  },
        { Groove::Base::sixteenth,    0.25,  "1/16" },
        { Groove::Base::thirtySecond, 0.125, "1/32" },
    };

    for (const double swing : { 0.5, 0.54, 0.58, 0.62, 2.0 / 3.0 })
        for (const auto& b : bases)
        {
            Groove parametric;
            parametric.enabled = true;
            parametric.base    = b.base;
            parametric.swing   = swing;
            parametric.amount  = 1.0;

            const int steps = 16;
            const double length = b.step * steps;

            // Play the donor: a straight part put through the swing.
            std::vector<GrooveTemplate::SourceNote> donor;
            std::vector<double> straight;

            for (int k = 0; k < steps; ++k)
            {
                const double at = b.step * k;
                straight.push_back (at);
                donor.push_back (srcNote (parametric.place (at, 60)));
            }

            const auto feel = GrooveTemplate::extract (donor, length, b.step);

            check (feel.numSlots() == steps,
                   std::string ("swing round trip, slot count at ") + b.name);

            // The deviation worked out from the definition of swing rather
            // than from the code: the offbeat sits `swing` of the way through
            // a pair of slots, which is (swing - 0.5) * 2 of one slot.
            const double expected = (swing - 0.5) * 2.0;

            for (int k = 0; k < steps; ++k)
            {
                const auto slot = feel.slotFor (k);
                check (slot.filled, std::string ("swing round trip, slot filled at ") + b.name);
                checkClose (slot.timing, (k % 2) != 0 ? expected : 0.0, 1.0e-12,
                            std::string ("swing round trip, deviation of slot ")
                              + std::to_string (k) + " at " + b.name
                              + " swing " + std::to_string (swing));
            }

            // And applying it to the straight part must land every note
            // exactly where the swing number put it.
            Groove byFeel;
            byFeel.enabled = true;
            byFeel.base    = b.base;
            byFeel.amount  = 1.0;
            byFeel.feel    = feel;

            for (int k = 0; k < steps; ++k)
                checkClose (byFeel.place (straight[(std::size_t) k], 60),
                            parametric.place (straight[(std::size_t) k], 60), 1.0e-12,
                            std::string ("swing round trip, placement of step ")
                              + std::to_string (k) + " at " + b.name);
        }
}

//=============================================================================
// A feel read off a part that is already on the grid must be no feel at all.
//=============================================================================
static void aQuantisedPartHasNoFeelInIt()
{
    std::vector<GrooveTemplate::SourceNote> donor;

    for (int k = 0; k < 16; ++k)
        donor.push_back (srcNote (0.25 * k, 0.7f));

    const auto feel = GrooveTemplate::extract (donor, 4.0, 0.25);

    for (int k = 0; k < feel.numSlots(); ++k)
        checkClose (feel.slotFor (k).timing, 0.0, 1.0e-15,
                    "a quantised donor leaves slot " + std::to_string (k) + " at zero");

    Groove g;
    g.enabled = true;
    g.amount  = 1.0;
    g.feel    = feel;

    for (const double amount : { 0.0, 0.25, 0.5, 1.0 })
    {
        g.amount = amount;

        for (int k = 0; k < 16; ++k)
            checkClose (g.place (0.25 * k, 60), 0.25 * k, 1.0e-15,
                        "a feel with no deviation in it moves nothing");
    }
}

//=============================================================================
// The model, asserted rather than commented.
//
// Deviation based means the donor's offset is ADDED to where the note already
// is. Target based would mean the note is moved ONTO the donor's position.
// The two differ by exactly the recipient's own offset, so a recipient that
// is itself off the grid is where they part company.
//=============================================================================
static void theFeelIsAddedRatherThanImposed()
{
    // One donor note, a quarter of a slot late.
    const double step = 0.25;
    const double donorOffset = 0.25;   // in fractions of a slot

    std::vector<GrooveTemplate::SourceNote> donor { srcNote (step * (1.0 + donorOffset)) };
    const auto feel = GrooveTemplate::extract (donor, step * 2.0, step);

    Groove g;
    g.enabled = true;
    g.amount  = 1.0;
    g.feel    = feel;

    // A recipient note exactly on the grid: both models agree here.
    checkClose (g.place (step, 60), step + donorOffset * step, 1.0e-15,
                "an on grid note takes the donor's deviation");

    // A recipient note off the grid is left alone, because the studio's rule
    // is that a part that was played is left where the performance put it,
    // and Quantise.h is what straightens it if that is wanted. Under a target
    // model it would instead be dragged to the donor's position, which is the
    // behaviour this check exists to exclude.
    const double played = step * 1.1;
    checkClose (g.place (played, 60), played, 1.0e-15,
                "a note played off the grid is not dragged onto the donor's position");

    // And the same template applied to the donor itself changes nothing, for
    // the same reason. Under a target model it would be a fixed point; under
    // this one it is untouched, and both are "unchanged" for different
    // reasons, so the check above is the one that separates them.
    checkClose (g.place (step * (1.0 + donorOffset), 60), step * (1.0 + donorOffset), 1.0e-15,
                "applying a feel to the part it came from changes nothing");
}

//=============================================================================
// The amount knob has to be a straight line through the shift, and nothing
// else. A curve sneaking in, or the amount being applied twice, both show up
// as a departure from the affine prediction.
//=============================================================================
static void theAmountIsAffine()
{
    std::vector<GrooveTemplate::SourceNote> donor;

    for (int k = 0; k < 8; ++k)
        donor.push_back (srcNote (0.25 * k + (k % 3 == 0 ? 0.03 : -0.05)));

    const auto feel = GrooveTemplate::extract (donor, 2.0, 0.25);

    Groove g;
    g.enabled = true;
    g.feel    = feel;

    for (int k = 0; k < 8; ++k)
    {
        const double at = 0.25 * k;

        g.amount = 0.0;
        const double none = g.place (at, 60);
        g.amount = 1.0;
        const double full = g.place (at, 60);

        checkClose (none, at, 1.0e-15, "an amount of nothing moves nothing");

        for (const double a : { 0.1, 0.25, 0.5, 0.75, 0.9 })
        {
            g.amount = a;
            checkClose (g.place (at, 60), none + a * (full - none), 1.0e-12,
                        "the amount is a straight line at " + std::to_string (a));
        }
    }
}

//=============================================================================
// A template shorter than the part repeats, and the repetition has to start
// in the right place.
//
// Built from a template whose first and last slots deviate in opposite
// directions, so taking the wrong one is wrong by the whole range rather than
// by a hair.
//=============================================================================
static void theFeelRepeatsFromTheRightPlace()
{
    const double step = 0.25;
    const int    slots = 4;

    std::vector<GrooveTemplate::SourceNote> donor
    {
        srcNote (step * 0.0 + step * 0.3),   // slot 0, late
        srcNote (step * 1.0),
        srcNote (step * 2.0),
        srcNote (step * 3.0 - step * 0.4),   // slot 3, early
    };

    const auto feel = GrooveTemplate::extract (donor, step * slots, step);

    checkClose (feel.slotFor (0).timing,  0.3, 1.0e-12, "slot 0 is late");
    checkClose (feel.slotFor (3).timing, -0.4, 1.0e-12, "slot 3 is early");

    // The wrap itself, read straight off the template, because this is where
    // it lives and because the placement below cannot reach a negative
    // position: a note is never placed in front of its own part.
    for (int rep = -3; rep <= 3; ++rep)
        for (int k = 0; k < slots; ++k)
            check (feel.slotFor (rep * slots + k) == feel.slotFor (k),
                   "slot " + std::to_string (rep * slots + k) + " is slot "
                     + std::to_string (k) + " again");

    // C++ leaves the remainder negative for a negative left operand, so these
    // are the ones an unguarded % gets wrong.
    checkClose (feel.slotFor (-1).timing, -0.4, 1.0e-12, "one step before the origin is the last slot");
    checkClose (feel.slotFor (-slots).timing, 0.3, 1.0e-12, "a whole template before it is the first");
    checkClose (feel.slotFor (-slots - 1).timing, -0.4, 1.0e-12, "and the one before that is the last");

    Groove g;
    g.enabled = true;
    g.amount  = 1.0;
    g.feel    = feel;

    // Four repetitions. Every one must behave like the first.
    for (int rep = 0; rep < 4; ++rep)
        for (int k = 0; k < slots; ++k)
        {
            const double at = step * (rep * slots + k);
            const double shift = feel.slotFor (k).timing * step;
            checkClose (g.place (at, 60), at + shift, 1.0e-12,
                        "repetition " + std::to_string (rep) + " slot " + std::to_string (k));
        }

    // The one that an off by one gets wrong: the first note of repetition two
    // takes slot 0, not slot 3. Those two differ by seven tenths of a slot
    // here, so there is no reading of the result that confuses them.
    const double firstOfSecond = step * slots;
    checkClose (g.place (firstOfSecond, 60), firstOfSecond + 0.3 * step, 1.0e-12,
                "the note after the template's end takes its first slot, not its last");
}

//=============================================================================
// A note cannot be pushed in front of the part it belongs to.
//
// `Groove::place` has always ended at zero and the swing half could never
// reach it, because swing only ever delays an offbeat. A feel can be early,
// so the first note of a part is now the one case where the groove asks for
// something there is no room for. Kept rather than removed: a negative
// position is before the clip's own origin, where the engine would drop the
// note entirely, and losing the downbeat is worse than not moving it.
//=============================================================================
static void nothingIsPlacedBeforeTheStart()
{
    Groove g;
    g.enabled = true;
    g.amount  = 1.0;
    g.feel    = GrooveTemplate::extract ({ srcNote (-0.25 * 0.4), srcNote (0.25) }, 0.5, 0.25);

    checkClose (g.feel.slotBeats(), 0.25, 1.0e-12, "the spacing is what was asked for");
    checkClose (g.place (0.0, 60), 0.0, 0.0, "the first note stays at the start rather than going behind it");
    check (g.place (0.25, 60) >= 0.0, "and nothing else goes behind it either");
}

//=============================================================================
// A slot the donor never played contributes nothing.
//
// Cakewalk documents four answers to this and the other three all invent a
// deviation for a position there is no evidence about. This asserts the one
// that was chosen.
//=============================================================================
static void anEmptySlotDoesNothing()
{
    const double step = 0.25;

    // A two note figure, both notes pushed late, over a bar of sixteenths.
    std::vector<GrooveTemplate::SourceNote> donor
    {
        srcNote (step * 0.0 + step * 0.2),
        srcNote (step * 8.0 + step * 0.2),
    };

    const auto feel = GrooveTemplate::extract (donor, 4.0, step);

    check (feel.numSlots() == 16, "the template is as long as the part, not as its last note");

    int filled = 0;
    for (int k = 0; k < feel.numSlots(); ++k)
        if (feel.slotFor (k).filled)
            ++filled;

    check (filled == 2, "only the slots the donor played are filled");

    Groove g;
    g.enabled = true;
    g.amount  = 1.0;
    g.feel    = feel;

    int moved = 0;
    for (int k = 0; k < 16; ++k)
    {
        const double at = step * k;
        if (std::abs (g.place (at, 60) - at) > 1.0e-15)
            ++moved;
    }

    check (moved == 2, "fourteen of sixteen notes are left exactly where they were");

    // An empty slot carries no deviation and no accent at all, not merely a
    // `filled` flag saying to ignore whatever it carries. Everything that
    // reads a slot asks `filled` first, so this is what makes forgetting to
    // ask harmless instead of a note moved by a number nobody set.
    for (int k = 0; k < feel.numSlots(); ++k)
        if (! feel.slotFor (k).filled)
        {
            check (feel.slotFor (k).timing == 0.0, "an empty slot carries no deviation");
            check (feel.slotFor (k).velocity == 0.0f, "an empty slot carries no accent");
        }
}

//=============================================================================
// A part's length does not have to be a whole number of steps. The playlist
// trims a clip wherever somebody drags it, and the clip's own length is what
// is handed over, so the slot count has to be the nearest whole number of
// steps.
//
// Nearest rather than however many fit inside it, because nearest is wrong by
// at most half a step and in either direction, while truncating is wrong by
// up to a whole step and always the same way: shorter than the part it came
// from, so it wraps early and the feel walks forward every time round. A
// length of 4.8 steps is 5 slots here and would be 4 under truncation, which
// loses a fifth of the part. Rounding down does happen, at 4.4 steps, and
// that is the half step either way rather than the systematic shortening.
//=============================================================================
static void aPartThatIsNotAWholeNumberOfStepsStillFits()
{
    const double step = 0.25;

    struct Case { double length; int slots; const char* what; };

    const Case cases[]
    {
        { 4.0,   16, "a bar of sixteenths is sixteen slots" },
        { 1.2,    5, "four point eight steps rounds up to five" },
        { 1.1,    4, "four point four steps rounds down to four" },
        { 0.3,    1, "one point two steps is one slot" },
        { 0.2,    1, "four fifths of a step still makes one slot" },
        { 2.625,  11, "ten and a half steps rounds up to eleven" },
    };

    for (const auto& c : cases)
    {
        const auto feel = GrooveTemplate::extract ({ srcNote (0.0) }, c.length, step);

        check (! feel.isEmpty(), std::string ("a feel came out of it: ") + c.what);

        if (feel.numSlots() != c.slots)
            fail (std::string (c.what) + ": got " + std::to_string (feel.numSlots())
                  + " slots, wanted " + std::to_string (c.slots));
    }
}

//=============================================================================
// The humanise knob has to do something when a feel is loaded, and something
// of the right size.
//
// This exists because three breakings of that one term all survived the whole
// suite: deleting it, making it four times too small by spending it in beats
// where the rest of the line is in slots, and making it four times too big.
// Every other check here asserts a bound or an ordering or determinism, and a
// humanise that is absent or tiny satisfies all three perfectly.
//
// The donor plays dead on the grid, so every deviation is zero and whatever
// separates a placed note from its own grid position is the humanising and
// nothing else. That also keeps the combined clamp off, so what is measured
// is the term itself rather than the bound over it.
//=============================================================================
static void theHumaniseKnobReachesTheFeelPath()
{
    const double step = 0.25;

    std::vector<GrooveTemplate::SourceNote> donor;
    for (int k = 0; k < 16; ++k)
        donor.push_back (srcNote (step * k, 0.8f));

    const auto feel = GrooveTemplate::extract (donor, 4.0, step);

    check (! feel.isEmpty(), "the donor made a feel");

    for (int k = 0; k < feel.numSlots(); ++k)
        checkClose (feel.slotFor (k).timing, 0.0, 1.0e-12,
                    "a donor played on the grid deviates by nothing");

    // Over enough notes and note numbers to see the top of the draw, since
    // the draw is a hash of the two and not a sweep.
    const auto worstDrift = [&feel] (double random)
    {
        Groove g;
        g.enabled = true;
        g.amount  = 1.0;
        g.random  = random;
        g.feel    = feel;

        double worst = 0.0;

        for (int k = 0; k < 256; ++k)
            for (int note = 0; note < 128; ++note)
            {
                const double at = feel.slotBeats() * k;
                worst = std::max (worst, std::abs (g.place (at, note) - at));
            }

        return worst / feel.slotBeats();
    };

    checkClose (worstDrift (0.0), 0.0, 1.0e-12,
                "the knob at nothing moves nothing, on a donor that deviates by nothing");

    // A quarter of a slot either way at the top of the knob, which is what
    // the swing half has always spent, and proportional on the way there.
    struct Case { double random; };
    const Case cases[] { { 0.15 }, { 0.4 }, { 1.0 } };

    for (const auto& c : cases)
    {
        const double drift = worstDrift (c.random);
        const double want  = c.random * 0.25;

        std::printf ("    humanise %.2f with a feel: worst %.4f of a slot, up to %.4f\n",
                     c.random, drift, want);

        if (drift > want + 1.0e-9)
            fail (std::string ("humanise ") + std::to_string (c.random)
                  + " moved a note " + std::to_string (drift)
                  + " of a slot, past the " + std::to_string (want) + " it may spend");

        // The lower bound is what catches the term being absent or spent in
        // the wrong unit. The draw is a hash, so it does not reach its limit
        // exactly, but over 32768 notes it comes close to it.
        if (drift < want * 0.9)
            fail (std::string ("humanise ") + std::to_string (c.random)
                  + " only moved a note " + std::to_string (drift)
                  + " of a slot, where it should reach nearly " + std::to_string (want));
    }
}

//=============================================================================
// The accents are read at the feel's own resolution, the same as the timing.
//
// Every other accent check in this file uses a feel spaced at 0.25 and leaves
// `base` at its default sixteenth, which is also 0.25, so the two cannot
// disagree and `shade` reading the wrong one of them survived. The state is
// ordinary: `base` is only written when somebody touches the swing control,
// so reading a feel at eighths while `base` still says sixteenths is what
// happens whenever the step resolution is changed on its own.
//=============================================================================
static void theAccentsAreReadAtTheFeelsOwnResolution()
{
    const double slot = 0.5;   // eighths, while base stays at sixteenths

    // Four slots, loud then quiet, so reading the grid at twice the
    // resolution lands on a different slot with a different accent.
    std::vector<GrooveTemplate::SourceNote> donor
    {
        srcNote (slot * 0.0, 1.0f),
        srcNote (slot * 1.0, 0.2f),
        srcNote (slot * 2.0, 1.0f),
        srcNote (slot * 3.0, 0.2f),
    };

    const auto feel = GrooveTemplate::extract (donor, 2.0, slot);

    check (feel.numSlots() == 4, "four eighths in two beats");
    checkClose (feel.slotBeats(), slot, 1.0e-12, "and the feel knows its own spacing");

    Groove g;
    g.enabled  = true;
    g.amount   = 1.0;
    g.velocity = 1.0;
    g.base     = Groove::Base::sixteenth;   // deliberately not the feel's
    g.feel     = feel;

    const double reference = feel.velocityReference();

    for (int k = 0; k < 4; ++k)
    {
        const double at   = slot * k;
        const float  got  = g.shade (at, 0.5f);
        const double want = std::clamp (0.5 + ((double) feel.slotFor (k).velocity - reference),
                                        0.02, 1.0);

        if (std::abs ((double) got - want) > 1.0e-6)
            fail (std::string ("slot ") + std::to_string (k) + " at a feel spaced "
                  + std::to_string (slot) + " with base at sixteenths shaded to "
                  + std::to_string (got) + ", wanted " + std::to_string (want));
    }

    // And the loud slots really are louder than the quiet ones, so the check
    // above is not comparing two numbers that happen to be equal.
    check (g.shade (0.0, 0.5f) > g.shade (slot, 0.5f) + 0.2f,
           "the donor's loud slot comes out louder than its quiet one");
}

//=============================================================================
// A donor longer than the cap is read as far as the cap reaches, and no
// further. It is not folded back into itself.
//
// Wrapping instead took the far end of the performance and laid it into the
// slots the near end had left empty, so a 24 bar take read at thirty-seconds
// came back with bar 17's deviation and bar 17's accent sitting in a slot
// bars 1 to 16 never played. That is a feel out of two different places in
// the part offered as one, and `fromString` refuses the same situation
// outright, so the two paths disagreed about the same fault.
//=============================================================================
static void aDonorLongerThanTheCapIsNotFoldedIntoItself()
{
    const double slot = 0.125;

    // One note at the very start, late. One note two slots past the cap,
    // early, which is where wrapping used to put it: slot 2.
    const int    beyond = GrooveTemplate::kMaxSlots + 2;
    std::vector<GrooveTemplate::SourceNote> donor
    {
        srcNote (slot * 0.0 + slot * 0.2, 0.8f),
        srcNote (slot * (double) beyond + slot * -0.4, 0.3f),
    };

    const auto feel = GrooveTemplate::extract (donor, slot * (double) (beyond + 64), slot);

    check (feel.numSlots() == GrooveTemplate::kMaxSlots, "the slot count is capped");

    check (feel.slotFor (0).filled, "the note inside the template is kept");
    checkClose (feel.slotFor (0).timing, 0.2, 1.0e-12, "with its own deviation");

    check (! feel.slotFor (2).filled,
           "a note past the cap does not appear in a slot the donor left empty there");

    int filled = 0;
    for (int k = 0; k < feel.numSlots(); ++k)
        if (feel.slotFor (k).filled)
            ++filled;

    check (filled == 1, "and nothing else was folded in either");

    // The reference is the one note that is really in the template, not an
    // average over a note that belongs to a different part of the take.
    checkClose (feel.velocityReference(), 0.8, 1.0e-6,
                "the accent reference is over what the template holds");
}

//=============================================================================
// A slot spacing out of a file has to be a musical grid, because a project
// file is text on a disk.
//
// Both ends did real damage. Ten beats a slot made the half-slot bound in
// `Groove::place` five beats, so a downbeat was thrown past the next four
// notes while they stayed where they were, which is the reorder that bound
// exists to prevent. A millionth of a beat made an ordinary position's slot
// index big enough to overflow the humanising hash, which is undefined
// behaviour and which the sanitizers stop on.
//=============================================================================
static void aFileCannotInventASlotSpacing()
{
    const auto spacingOf = [] (long long micros)
    {
        return GrooveTemplate::fromString (std::to_string (micros) + " 100000:800000");
    };

    // Refused, not clamped: a feel at the wrong resolution is worse than none.
    check (spacingOf (10'000'000).isEmpty(), "ten beats a slot is not a grid");
    check (spacingOf (1).isEmpty(), "nor is a millionth of a beat");
    check (spacingOf (4'000'001).isEmpty(), "just over a bar is refused");
    check (spacingOf (15'624).isEmpty(), "just under a 256th note is refused");

    // And the range the rack really uses, plus the edges, all load.
    for (long long micros : { 15'625LL, 125'000LL, 250'000LL, 500'000LL, 4'000'000LL })
    {
        const auto feel = spacingOf (micros);

        if (feel.isEmpty())
            fail (std::string ("a spacing of ") + std::to_string (micros)
                  + " millionths should have loaded");

        checkClose (feel.slotBeats(), (double) micros / 1.0e6, 1.0e-12,
                    "and it keeps the spacing it was given");
    }

    checkClose ((double) GrooveTemplate::kMinSlotBeats, 1.0 / 64.0, 1.0e-12,
                "the floor is a 256th note");
    checkClose ((double) GrooveTemplate::kMaxSlotBeats, 4.0, 1.0e-12,
                "and the ceiling a bar of four");

    // The position that used to overflow the hash, at a spacing that is now
    // refused, so nothing reaches it. Half an hour in at 120 bpm.
    {
        Groove g;
        g.enabled = true;
        g.random  = 1.0;
        g.feel    = GrooveTemplate::fromString ("15625 100000:800000");

        check (! g.feel.isEmpty(), "the finest allowed spacing loads");
        check (std::isfinite (g.place (3600.0, 60)), "and an hour of song is still a number");
    }
}

//=============================================================================
// A project file is text on a disk and nothing stops somebody editing it, so
// the bounds a template promises have to be enforced on the way in and not
// only assumed of an extracted one. `setSlots` is the single gate both paths
// go through, and this is the check that says so.
//=============================================================================
static void aFileCannotPushASlotOutOfRange()
{
    // Three quarters of a slot late, and louder than full scale. Both are
    // beyond what extraction can produce and both are one text editor away.
    const auto feel = GrooveTemplate::fromString ("250000 750000:1800000 -900000:-400000");

    check (! feel.isEmpty(), "a template with out of range numbers still loads");
    check (feel.numSlots() == 2, "both slots are there");

    check (feel.slotFor (0).timing <= 0.5 + 1.0e-12 && feel.slotFor (0).timing >= -0.5 - 1.0e-12,
           "a deviation past the slot boundary is brought back to it");
    checkClose (feel.slotFor (0).timing, 0.5, 1.0e-12, "and it is brought back to the near edge");
    checkClose (feel.slotFor (1).timing, -0.5, 1.0e-12, "the other way round too");

    check (feel.slotFor (0).velocity <= 1.0f, "a velocity past full scale is brought back");
    checkClose (feel.slotFor (0).velocity, 1.0f, 1.0e-6, "to full scale exactly");
    checkClose (feel.slotFor (1).velocity, 0.0f, 1.0e-6, "and a negative one to silence");

    // The reference is the mean of what the slots now hold rather than of
    // what the file asked for, which is the same invariant the extraction
    // path has and the reason it is computed in one place.
    checkClose (feel.velocityReference(), 0.5, 1.0e-6,
                "the reference is the mean of the clamped velocities");

    // And a note grooved by it still cannot be moved more than half a slot,
    // which is what stops a hand edited file reordering a pattern.
    Groove g;
    g.enabled = true;
    g.amount  = 1.0;
    g.random  = 1.0;
    g.feel    = feel;

    double previous = -1.0e9;

    for (int k = 0; k < 8; ++k)
    {
        const double at = feel.slotBeats() * k;
        const double placed = g.place (at, 60);

        check (std::abs (placed - at) <= feel.slotBeats() * 0.5 + 1.0e-12,
               "a file cannot move a note further than half a slot");
        check (placed >= previous - 1.0e-12, "nor reorder the pattern");

        previous = placed;
    }
}

//=============================================================================
// Extraction has to be a function of the notes, not of the order they arrive
// in. Without the collision rule a spread chord makes the slot's timing
// depend on which voice the loop reached last.
//=============================================================================
static void extractionDoesNotDependOnNoteOrder()
{
    const double step = 0.25;

    std::vector<GrooveTemplate::SourceNote> chord
    {
        srcNote (step * 1.0 + 0.004f, 0.90f, 67),
        srcNote (step * 1.0 - 0.006f, 0.40f, 55),   // earliest: this one wins
        srcNote (step * 1.0 + 0.001f, 0.70f, 60),
        srcNote (step * 1.0 - 0.006f, 0.95f, 72),   // same position, higher pitch
        srcNote (step * 2.0,          0.60f, 60),
    };

    const auto reference = GrooveTemplate::extract (chord, 1.0, step);

    checkClose (reference.slotFor (1).timing, -0.006 / step, 1.0e-9,
                "the earliest note of a spread chord sets the slot");
    checkClose ((double) reference.slotFor (1).velocity, 0.40, 1.0e-6,
                "and its velocity, with the lower pitch winning the tie");

    std::mt19937 rng (20261113u);

    for (int trial = 0; trial < 200; ++trial)
    {
        auto shuffled = chord;
        std::shuffle (shuffled.begin(), shuffled.end(), rng);

        check (GrooveTemplate::extract (shuffled, 1.0, step) == reference,
               "the same notes in another order give the same template");
    }
}

//=============================================================================
// Velocity: the accent pattern, measured against the donor's own average.
//=============================================================================
static void accentsAreMeasuredAgainstTheDonorsOwnAverage()
{
    const double step = 0.25;

    // A donor played at one steady loudness has no accent pattern in it, so
    // it must change nothing however far the velocity knob is turned up. This
    // is the velocity counterpart of a donor who played exactly on the grid.
    {
        std::vector<GrooveTemplate::SourceNote> flat;
        for (int k = 0; k < 8; ++k)
            flat.push_back (srcNote (step * k, 0.6f));

        Groove g;
        g.enabled  = true;
        g.velocity = 1.0;
        g.feel     = GrooveTemplate::extract (flat, 2.0, step);

        for (int k = 0; k < 8; ++k)
            checkClose (g.shade (step * k, 0.5f), 0.5, 1.0e-6,
                        "a donor with no accents in it changes nothing");
    }

    // A donor with accents: loud slots push the note up, quiet slots down, by
    // the amount the knob asks for.
    {
        std::vector<GrooveTemplate::SourceNote> accented;
        for (int k = 0; k < 8; ++k)
            accented.push_back (srcNote (step * k, (k % 4) == 0 ? 1.0f : 0.5f));

        const auto feel = GrooveTemplate::extract (accented, 2.0, step);
        const double mean = meanOfFilled (feel);

        checkClose (feel.velocityReference(), mean, 1.0e-12,
                    "the reference is the mean of the filled slots");
        checkClose (mean, (2.0 * 1.0 + 6.0 * 0.5) / 8.0, 1.0e-6,
                    "and the mean is what it should be");

        Groove g;
        g.enabled = true;
        g.feel    = feel;

        for (const double amount : { 0.0, 0.25, 0.5, 1.0 })
        {
            g.velocity = amount;

            for (int k = 0; k < 8; ++k)
            {
                const double slotVelocity = (k % 4) == 0 ? 1.0 : 0.5;
                const double want = std::clamp (0.6 + amount * (slotVelocity - mean), 0.02, 1.0);
                checkClose (g.shade (step * k, 0.6f), want, 1.0e-6,
                            "accent at slot " + std::to_string (k)
                              + " with the knob at " + std::to_string (amount));
            }
        }

        // The knob at nothing is the knob doing nothing, which is the one
        // case a user can see immediately.
        g.velocity = 0.0;
        for (int k = 0; k < 8; ++k)
            checkClose (g.shade (step * k, 0.6f), 0.6, 1.0e-6,
                        "no velocity amount, no change");
    }

    // A note the feel has no slot for keeps its own velocity.
    {
        std::vector<GrooveTemplate::SourceNote> sparse { srcNote (0.0, 1.0f), srcNote (1.0, 0.2f) };

        Groove g;
        g.enabled  = true;
        g.velocity = 1.0;
        g.feel     = GrooveTemplate::extract (sparse, 2.0, step);

        checkClose (g.shade (step * 3.0, 0.42f), 0.42, 1.0e-6,
                    "an empty slot leaves the velocity alone");
    }

    // And the result stays in range whatever the donor did.
    {
        std::vector<GrooveTemplate::SourceNote> extreme
        {
            srcNote (0.0,  1.0f),
            srcNote (step, 0.0f),
        };

        Groove g;
        g.enabled  = true;
        g.velocity = 1.0;
        g.feel     = GrooveTemplate::extract (extreme, 0.5, step);

        for (const float v : { 0.01f, 0.1f, 0.5f, 0.99f, 1.0f })
            for (int k = 0; k < 2; ++k)
            {
                const float out = g.shade (step * k, v);
                check (out >= 0.02f - 1.0e-6f && out <= 1.0f + 1.0e-6f,
                       "a shaded velocity stays in range");
            }
    }
}

//=============================================================================
// The invariant: the reference is the mean of the filled slots, after either
// of the two ways slots can be set.
//=============================================================================
static void theReferenceNeverGoesStale()
{
    std::mt19937 rng (7733u);
    std::uniform_real_distribution<double> position (-8.0, 8.0);
    std::uniform_real_distribution<float>  loudness (0.0f, 1.0f);
    std::uniform_int_distribution<int>     howMany (1, 40);

    for (int trial = 0; trial < 500; ++trial)
    {
        std::vector<GrooveTemplate::SourceNote> notes;
        const int n = howMany (rng);

        for (int i = 0; i < n; ++i)
            notes.push_back (srcNote (position (rng), loudness (rng), 40 + (i % 48)));

        const auto feel = GrooveTemplate::extract (notes, 4.0, 0.25);

        checkClose (feel.velocityReference(), meanOfFilled (feel), 1.0e-12,
                    "the reference matches the slots after extracting");

        const auto reread = GrooveTemplate::fromString (feel.toString());

        checkClose (reread.velocityReference(), meanOfFilled (reread), 1.0e-12,
                    "the reference matches the slots after reading a file");
    }
}

//=============================================================================
// Saving and loading.
//=============================================================================
static void aFeelSurvivesBeingWrittenDown()
{
    std::mt19937 rng (991u);
    std::uniform_real_distribution<double> jitter (-0.49, 0.49);
    std::uniform_real_distribution<float>  loudness (0.0f, 1.0f);
    std::bernoulli_distribution            played (0.7);

    for (const double step : { 0.5, 0.25, 0.125 })
        for (int trial = 0; trial < 100; ++trial)
        {
            std::vector<GrooveTemplate::SourceNote> notes;
            const int slots = 16;

            for (int k = 0; k < slots; ++k)
                if (played (rng))
                    notes.push_back (srcNote (step * (k + jitter (rng)), loudness (rng), 60));

            const auto feel = GrooveTemplate::extract (notes, step * slots, step);

            if (feel.isEmpty())
                continue;

            const auto reread = GrooveTemplate::fromString (feel.toString());

            check (reread.numSlots() == feel.numSlots(), "the slot count survives");
            checkClose (reread.slotBeats(), feel.slotBeats(), 1.0e-9, "the spacing survives");

            for (int k = 0; k < feel.numSlots(); ++k)
            {
                const auto a = feel.slotFor (k), b = reread.slotFor (k);
                check (a.filled == b.filled, "a slot that was empty stays empty");

                if (a.filled)
                {
                    checkClose (a.timing, b.timing, 2.0e-6, "the deviation survives");
                    checkClose (a.velocity, b.velocity, 2.0e-6, "the velocity survives");
                }
            }
        }

    // Nothing in, nothing out.
    check (GrooveTemplate::fromString ("").isEmpty(), "an absent feel reads back as none");

    // Malformed text gives no feel rather than a partly read one.
    for (const char* junk : { "nonsense", "250000 0:", "250000 :5", "250000 0:1 oops",
                              "0 0:1", "-250000 0:1", "250000", "250000 0;1" })
        check (GrooveTemplate::fromString (junk).isEmpty(),
               std::string ("malformed text gives no feel: ") + junk);

    // A template of nothing but empty slots is no template, because a feel
    // that is present and inert is one more state for everything downstream.
    check (GrooveTemplate::fromString ("250000 - - - -").isEmpty(),
           "a template with nothing played in it is no template");

    // A file asking for more slots than the cap is refused rather than
    // truncated, because a truncated feel wraps early and walks forward.
    {
        std::string huge = "250000";
        for (int i = 0; i < GrooveTemplate::kMaxSlots + 1; ++i)
            huge += " 0:500000";

        check (GrooveTemplate::fromString (huge).isEmpty(), "an oversized template is refused");
    }
}

//=============================================================================
// Extraction never produces something the application cannot use, whatever it
// is handed. Randomised, because the interesting cases here are the ones
// nobody thinks to write down.
//=============================================================================
static void nothingTheDonorDoesBreaksIt()
{
    std::mt19937 rng (4242u);
    std::uniform_real_distribution<double> anywhere (-40.0, 40.0);
    std::uniform_real_distribution<double> within (0.0, 1.0);
    std::uniform_real_distribution<float>  loudness (-0.5f, 1.5f);
    std::uniform_int_distribution<int>     pitches (0, 127);
    std::uniform_int_distribution<int>     howMany (0, 60);

    const double steps[] { 0.5, 0.25, 0.125 };
    const double lengths[] { 0.5, 1.0, 2.0, 4.0, 8.0, 16.0 };

    int nonEmpty = 0;
    long long filledSeen = 0;

    for (int trial = 0; trial < 4000; ++trial)
    {
        const double step   = steps[(std::size_t) (trial % 3)];
        const double length = lengths[(std::size_t) ((trial / 3) % 6)];

        std::vector<GrooveTemplate::SourceNote> notes;
        const int n = howMany (rng);

        // Most of the notes land inside the part and one in eight is thrown
        // a long way outside it, so this exercises both the density and the
        // refusal. Drawing every note across eighty beats instead, which is
        // what this did while a note outside the template wrapped round into
        // it, leaves a mean of three filled slots out of sixteen once such a
        // note is dropped rather than wrapped, and most of what the loop
        // below is for needs neighbours to actually be there.
        for (int i = 0; i < n; ++i)
            notes.push_back (srcNote (i % 8 == 0 ? anywhere (rng) : within (rng) * length,
                                      loudness (rng), pitches (rng)));

        const auto feel = GrooveTemplate::extract (notes, length, step);

        if (feel.isEmpty())
            continue;

        ++nonEmpty;

        for (int k = 0; k < feel.numSlots(); ++k)
            if (feel.slotFor (k).filled)
                ++filledSeen;

        check (feel.numSlots() >= 1 && feel.numSlots() <= GrooveTemplate::kMaxSlots,
               "the slot count is sane");
        checkClose (feel.lengthBeats(), feel.slotBeats() * feel.numSlots(), 1.0e-12,
                    "the length is the slots times the spacing");

        for (int k = -3 * feel.numSlots(); k < 3 * feel.numSlots(); ++k)
        {
            const auto slot = feel.slotFor (k);
            check (std::isfinite (slot.timing) && std::abs (slot.timing) <= 0.5 + 1.0e-12,
                   "every deviation is inside half a slot");
            check (slot.velocity >= 0.0f && slot.velocity <= 1.0f,
                   "every velocity is in range");
        }

        // A grooved part must not reorder itself. Deviations are bounded by
        // half a slot either way, so two neighbours can meet but cannot
        // cross, and a pattern that reordered itself would be unusable.
        //
        // The humanise knob is swept here rather than left at zero, and that
        // is the whole point of this loop rather than a thoroughness. A
        // deviation of half a slot already spends the entire distance to the
        // next note, so humanising added on top of it walks one note past the
        // other: measured at 0.42 of a slot backwards before `Groove::place`
        // bounded the two together. Read with the knob down, every check in
        // this file passed while a feel and a humanise together played a
        // pattern in the wrong order.
        //
        // Both knobs are swept to their limits because that is where the
        // budget runs out, and the amount below one is in the sweep because
        // scaling the deviation leaves the humanising its full size and so
        // moves which of the two is doing the crossing.
        for (double random : { 0.0, 0.15, 0.4, 1.0 })
            for (double amount : { 0.3, 0.7, 1.0 })
            {
                Groove g;
                g.enabled = true;
                g.amount  = amount;
                g.random  = random;
                g.feel    = feel;

                double previous = -1.0e9;

                for (int k = 0; k < 4 * feel.numSlots(); ++k)
                {
                    const double at = feel.slotBeats() * k;
                    const double placed = g.place (at, 60 + (k % 7));

                    check (std::isfinite (placed), "a placed note is a real number");

                    if (! (placed >= previous - 1.0e-12))
                        fail (std::string ("slot ") + std::to_string (k)
                              + " at amount " + std::to_string (amount)
                              + ", humanise " + std::to_string (random)
                              + " landed at " + std::to_string (placed)
                              + ", which is behind " + std::to_string (previous));

                    check (std::abs (placed - at) <= feel.slotBeats() * 0.5 + 1.0e-12,
                           "a note moves at most half a slot");

                    previous = placed;
                }
            }
    }

    // Printed because this loop is only worth what it covers, and a change
    // elsewhere that quietly stops the generated notes reaching the template
    // would otherwise leave it passing over almost nothing.
    std::printf ("    %d of 4000 generated donors made a feel, %.2f filled slots each\n",
                 nonEmpty, nonEmpty > 0 ? (double) filledSeen / nonEmpty : 0.0);

    check (nonEmpty > 3000, "most of the generated donors make a feel");
    check ((double) filledSeen / std::max (1, nonEmpty) > 6.0,
           "and the feels they make have neighbours in them to cross");
}

//=============================================================================
// Same input, same output, every time. Grooves are applied when the
// arrangement is handed to the engine, and renderOffline does that again, so
// anything here that was not a pure function of the note would make a bounce
// differ from what was heard.
//=============================================================================
static void theSameFeelPlaysTheSameWayTwice()
{
    std::vector<GrooveTemplate::SourceNote> donor;

    for (int k = 0; k < 12; ++k)
        donor.push_back (srcNote (0.25 * k + 0.01 * ((k * 7) % 5), 0.3f + 0.05f * (k % 6)));

    Groove g;
    g.enabled  = true;
    g.amount   = 0.8;
    g.velocity = 0.5;
    g.random   = 0.3;
    g.feel     = GrooveTemplate::extract (donor, 3.0, 0.25);

    for (int pass = 0; pass < 3; ++pass)
        for (int k = 0; k < 48; ++k)
        {
            const double at = 0.25 * k;
            checkClose (g.place (at, 60 + (k % 5)), g.place (at, 60 + (k % 5)), 0.0,
                        "placement is a function of the note alone");
            checkClose (g.shade (at, 0.6f), g.shade (at, 0.6f), 0.0,
                        "shading is a function of the note alone");
        }

    // And a copy of the groove, as the undo history and the project file both
    // make, plays identically.
    const Groove copy = g;

    for (int k = 0; k < 48; ++k)
    {
        const double at = 0.25 * k;
        checkClose (copy.place (at, 60), g.place (at, 60), 0.0, "a copied groove plays the same");
        checkClose (copy.shade (at, 0.6f), g.shade (at, 0.6f), 0.0, "a copied groove shades the same");
    }

    check (copy == g, "a copied groove compares equal, so the undo history can tell them apart");
}

//=============================================================================
// Turning it off has to turn it off, and the parametric half has to be
// untouched by any of this.
//=============================================================================
static void theSwingHalfStillWorks()
{
    std::vector<GrooveTemplate::SourceNote> donor { srcNote (0.25 * 1.4) };

    Groove g;
    g.enabled = true;
    g.amount  = 1.0;
    g.feel    = GrooveTemplate::extract (donor, 1.0, 0.25);
    g.enabled = false;

    for (int k = 0; k < 8; ++k)
        checkClose (g.place (0.25 * k, 60), 0.25 * k, 0.0, "a groove switched off moves nothing");

    // With no feel loaded, the swing is exactly what it was before any of
    // this existed: the offbeat of a pair sits at the swing fraction.
    Groove swung;
    swung.enabled = true;
    swung.base    = Groove::Base::sixteenth;
    swung.swing   = 0.62;
    swung.amount  = 1.0;

    check (swung.feel.isEmpty(), "a groove starts with no feel in it");

    for (int k = 0; k < 8; ++k)
    {
        const double at = 0.25 * k;
        const double want = (k % 2) != 0 ? at + (0.62 * 0.5 - 0.25) : at;
        checkClose (swung.place (at, 60), want, 1.0e-12, "the swing number still means what it meant");
    }

    // A feel loaded onto a swung groove replaces the swing rather than adding
    // to it, the way a clip in Live has one groove and not two.
    swung.feel = GrooveTemplate::extract ({ srcNote (0.25 * 1.1) }, 0.5, 0.25);
    checkClose (swung.place (0.25, 60), 0.25 + 0.1 * 0.25, 1.0e-12,
                "a loaded feel replaces the swing rather than adding to it");
}

//=============================================================================
// Nonsense in, nothing out.
//=============================================================================
static void anImpossibleRequestGivesNoFeel()
{
    const auto one = srcNote (0.25);

    check (GrooveTemplate::extract ({ one }, 4.0,  0.0).isEmpty(), "no spacing, no feel");
    check (GrooveTemplate::extract ({ one }, 4.0, -0.25).isEmpty(), "negative spacing, no feel");
    check (GrooveTemplate::extract ({ one }, 0.0,  0.25).isEmpty(), "no length, no feel");
    check (GrooveTemplate::extract ({ one }, -4.0, 0.25).isEmpty(), "negative length, no feel");
    check (GrooveTemplate::extract ({}, 4.0, 0.25).isEmpty(), "a part with no notes has no feel");
    check (GrooveTemplate::extract ({ one }, 0.1, 0.25).isEmpty(),
           "a part shorter than one slot has no feel");

    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto inf = std::numeric_limits<double>::infinity();

    check (GrooveTemplate::extract ({ one }, nan, 0.25).isEmpty(), "a length of NaN gives no feel");
    check (GrooveTemplate::extract ({ one }, inf, 0.25).isEmpty(), "an endless part gives no feel");
    check (GrooveTemplate::extract ({ one }, 4.0, nan).isEmpty(), "a spacing of NaN gives no feel");

    // A note that is not a real position is dropped rather than poisoning the
    // template it lands in.
    {
        std::vector<GrooveTemplate::SourceNote> mixed { srcNote (nan), srcNote (inf), srcNote (0.25 * 1.2) };
        const auto feel = GrooveTemplate::extract (mixed, 1.0, 0.25);

        check (! feel.isEmpty(), "the real note still makes a feel");
        check (! feel.slotFor (0).filled, "and the unreal ones filled nothing");
        checkClose (feel.slotFor (1).timing, 0.2, 1.0e-12, "the real note is where it should be");
    }

    // More slots than the cap allows are capped rather than allocated.
    {
        const auto feel = GrooveTemplate::extract ({ srcNote (0.0) }, 10000.0, 0.125);
        check (feel.numSlots() == GrooveTemplate::kMaxSlots, "the slot count is capped");
    }
}

int main()
{
    std::printf ("groove template\n");

    aSwungPartReadsBackAsThatSwing();
    aQuantisedPartHasNoFeelInIt();
    aPartThatIsNotAWholeNumberOfStepsStillFits();
    aFileCannotPushASlotOutOfRange();
    aFileCannotInventASlotSpacing();
    aDonorLongerThanTheCapIsNotFoldedIntoItself();
    theHumaniseKnobReachesTheFeelPath();
    theAccentsAreReadAtTheFeelsOwnResolution();
    theFeelIsAddedRatherThanImposed();
    theAmountIsAffine();
    theFeelRepeatsFromTheRightPlace();
    nothingIsPlacedBeforeTheStart();
    anEmptySlotDoesNothing();
    extractionDoesNotDependOnNoteOrder();
    accentsAreMeasuredAgainstTheDonorsOwnAverage();
    theReferenceNeverGoesStale();
    aFeelSurvivesBeingWrittenDown();
    nothingTheDonorDoesBreaksIt();
    theSameFeelPlaysTheSameWayTwice();
    theSwingHalfStillWorks();
    anImpossibleRequestGivesNoFeel();

    if (failures != 0)
    {
        std::printf ("\n%d groove template check(s) failed\n", failures);
        return 1;
    }

    std::printf ("all groove template checks passed\n");
    return 0;
}
