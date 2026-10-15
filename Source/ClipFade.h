#pragma once
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// Clip fades: the ramp at each end of an audio clip, and the de-click fade
// every edge gets whether anybody asked for one.
//
// A clip trimmed to its first note starts on whatever sample the trim landed
// on. If that sample is not near zero the start is a step, and a step's
// spectrum falls at only 6 dB per octave, so it puts energy right across the
// audible band. That is the click, and it is why every studio applies a short
// fade at a clip boundary: Ableton calls the option "Create Fades on Clip
// Edges", Ardour's manual says its region fade "serves to de-click the
// transitions at the start and end of the region", and Reaper carries a fade
// length per clip end in its own data model.
//
// Two quantities per end, not one, which is Reaper's model and the best of the
// four. Its extension SDK documents `D_FADEINLEN` as the "item manual fadein
// length in seconds" and separately `D_FADEINLEN_AUTO` as the "item
// auto-fadein length in seconds, 0 means no auto-fadein". So the studio's
// de-click fade and the fade somebody drew coexist instead of one overwriting
// the other: a clip stores what was asked for, zero until somebody drags it,
// and an edge still at zero is filled in by the project's automatic length.
// Setting that length to zero turns the automatic fades off, which is Reaper's
// own convention for the same field.
//
// A crossfade is not a separate object here, and that is a decision rather
// than an omission. Reaper and Ableton both make a crossfade an overlap of two
// clips' own fades, and `AudioEngine::renderAudioClips` already sums
// overlapping clips on a track into the same insert, so a fade out meeting a
// fade in is a crossfade with no new entity in the file format and no second
// render path to disagree with the first. Ardour's manual makes the same point
// from the other direction: "It is important to understand that region fades
// *are* crossfades."
//
// Everything here is in seconds of output time and carries no JUCE, so the
// whole of it compiles on its own in a second under sanitizers. The engine
// keeps only the conversion into beats, which is what it has per sample. See
// Tests/ClipFadeTest.cpp.
//
// ---- What the shapes are, and which material each is for -------------------
//
// Avid states the rule in both directions, on its Fades Dialog Link Settings
// page, with each choice's failure named: Equal Power is "Recommended for
// material that is not phase coherent" and avoids the dip equal gain causes,
// and Equal Gain is "Recommended for material that is phase-coherent or
// nearly phase-coherent" and avoids the clipping equal power causes. The
// criterion is phase coherence and not "the same file", which matters: two
// takes of one singer are different waveforms however much they sound alike,
// and the same bar of a drum loop copied twice is the same waveform.
//
// Ardour's manual states it with the numbers, and they agree: linear is "the
// correct crossfade to use with highly-correlated signals" and its two sides
// are "each -6dB attenuated at the midpoint", while constant power is "the
// correct crossfade to use when splicing audio in the general (uncorrelated)
// case" and its sides "each reach -3dB at the midpoint". Apple is a third
// independent statement, naming EqP (Equal Power Crossfade) and giving the
// reason as minimising "volume dips between audio regions".
//
// So:
//
//   linear      g = x                    equal gain.  g(x) + g(1-x) = 1
//   equalPower  g = sin(x * pi/2)        equal power. g(x)^2 + g(1-x)^2 = 1
//   sCurve      g = (1 - cos(x * pi))/2  equal gain, and the de-click shape
//
// `equalPower` is the default, following Ardour, whose `default-fade-shape`
// configuration variable is `FadeConstantPower` and is what both its region
// fades and its crossfades get. That is not a consensus and the disagreement
// is worth recording: Pro Tools' documented default is its Preset Curve 4,
// which Avid describes as fading "in with a linear fade curve. This is the
// default curve", and Avid's user guide calls its Default Fade Out "standard
// equal gain". Reaper's shipped default is not documented at all.
//
// There is no established default for a fade from silence, and saying so is
// more honest than implying an authority. Ardour's choice has a published
// reason but the reason is about Ardour's data model rather than about sound:
// in a layered model every region fade is a crossfade against whatever lies
// beneath, and "If a region doesn't have any region(s) under it, then the
// region is crossfaded to silence". The theoretical argument does not transfer
// either. Equal power exists to hold the sum of two uncorrelated powers
// constant, and against silence there is no second term to hold constant with,
// so both laws are simply different curves there.
//
// ---- Why the de-click fade is the raised cosine ---------------------------
//
// This is the one piece of arithmetic in the header that is derived rather
// than quoted, so it is marked as such. The published part is Harris, "On the
// Use of Windows for Harmonic Analysis with the Discrete Fourier Transform",
// Proc. IEEE 66(1), January 1978, whose Table I gives the sidelobe fall off
// against the order of the boundary discontinuity: -6 dB per octave for the
// rectangle, -12 for the triangle, "reflecting the discontinuity of the window
// residing in the first derivative", and -18 for Hann, where "the
// discontinuity of this window resides in the second derivative". Nuttall,
// NUSC TR 6239, 1980, states the same result as an asymptotic expansion.
//
// The step from there is to apply it to a fade's two joins separately, which
// is the part that is easy to get wrong, because the two joins are not the
// same. At the silent end the fade meets zero; at the other it meets unity.
// Writing n for the order of the lowest discontinuous derivative, the energy
// the join adds falls at 6(n+1) dB per octave:
//
//                      silent end        unity end
//   hard cut           n=0,  6 dB/oct    6 dB/oct
//   linear             n=1, 12 dB/oct    n=1, 12 dB/oct
//   sin(x * pi/2)      n=1, 12 dB/oct    n=2, 18 dB/oct   (zero slope at 1)
//   (1 - cos(x pi))/2  n=2, 18 dB/oct    n=2, 18 dB/oct
//
// So the raised cosine is the shape a de-click fade wants, and the project's
// default shape is 6 dB per octave worse than it at the end that matters
// most, which is a third of the decay rather than half of it: it is the order
// n that halves, not the figure.
// Ardour uses its default shape for its de-click fade and therefore gets
// 12 dB per octave at the silent end. `ClipFadeTest` measures the decay rather
// than trusting the table.
//
// The second reason points the same way and was not obvious. The raised cosine
// is an equal gain law: g(x) + g(1-x) = 1 exactly, by the cosine's own
// symmetry about a quarter cycle. So where two ramps of it cover the same
// span, which is what two overlapping clips give, the join is bit transparent
// on phase coherent material, and a cut is the one case where both sides are
// certain to be the same waveform. That is exactly what Avid's rule asks for.
//
// It does **not** apply to two clips that merely abut, and reading it that way
// is a mistake worth naming because it is the one that was nearly shipped
// here. Abutting ramps are consecutive, not simultaneous: the composite gain
// passes through zero at the join rather than holding unity across it. The
// identity is about two ramps over one span. `Request::autoIn` and
// `autoOut` are what keep that case out of the automatic fade's way.
//
// ---- Why the length is a time, and why not a sample count -----------------
//
// Every studio checked stores a fade length in absolute time and not one
// stores it as a fraction of the clip. Ardour's fade is an
// `Evoral::ControlList` built with `Temporal::AudioTime`, so it does not move
// with tempo; Reaper's `D_FADEINLEN` is documented in seconds. A fraction is
// the plausible and wrong choice, and wrong in a way that would be hard to
// diagnose: trimming a clip would change how its fade sounds, and stretching
// one would change its de-click length. The same reasoning keeps `CompModel.h`
// in seconds.
//
// Ardour's de-click length is 64 samples, a bare literal appearing four times
// in its tree with no named constant. That is deliberately not copied, for the
// reason `CLAUDE.md` already records for the K-weighting filters: a fixed
// sample count gets shorter in time as the sample rate rises, 1.45 ms at
// 44.1 kHz and 0.33 ms at 192 kHz, which is backwards. What the fade has to do
// is set by the audible band, not by the converter.
//
// The default automatic length is 4 ms, which is the figure Ableton publishes
// twice: its Clip View section says the edge fade "ranges from 0-4
// milliseconds", and its Arrangement View section says a deleted fade handle
// resets to "a default length of 4 ms" and that adjacent clips "automatically
// get 4 ms crossfades". Ardour's 1.45 ms at 44.1 kHz and Avid's suggested
// 10 ms for its punch crossfade bracket it. No standard exists for the
// shortest click-free fade: there is no AES recommended practice and no
// published psychoacoustic threshold expressed as a duration, and every figure
// in circulation is a vendor default. 4 ms is the one a manufacturer publishes
// as a time rather than as a number of samples, so it is the one used, and the
// criterion actually checked is the spectral decay above.
//
// One figure Ableton publishes cannot be implemented from the documentation:
// its edge fade is "signal-dependent" within that 0 to 4 ms range, and how the
// length is chosen from the signal is not published. Guessing at it would be
// the plausible and wrong failure this project keeps a record of, so the
// length here is fixed and the fact that Live varies it is recorded instead.
// ---------------------------------------------------------------------------

namespace ClipFade
{

/** The three shapes. Ordered so that the first entry is the one that is safe
    to fall back to when a saved id is not recognised, which is the discipline
    `PanOneParams` and `ModMatrix` already follow: linear is the only one of
    the three with no transcendental in it and the only one whose meaning
    nobody could argue about. */
enum class Shape
{
    linear = 0,     // equal gain:  g(x) + g(1-x) = 1
    equalPower,     // equal power: g(x)^2 + g(1-x)^2 = 1
    sCurve          // raised cosine: equal gain, and 18 dB/oct at both joins
};

inline constexpr int numShapes = 3;

/** The shape a fade gets when nobody chose one. Ardour's `default-fade-shape`,
    which is `FadeConstantPower`. */
inline constexpr Shape defaultShape = Shape::equalPower;

/** The shape the automatic de-click fade uses, which is deliberately not
    `defaultShape`: see the derivation above. The two have different jobs. A
    de-click fade exists to suppress the broadband energy of an edge, which the
    raised cosine does 6 dB per octave better at the silent end, and a fade
    somebody drew exists to hold the level across a join, which is what equal
    power is for. */
inline constexpr Shape declickShape = Shape::sCurve;

/** The project's automatic edge fade length when nothing has been saved.
    Ableton's published figure. Seconds. */
inline constexpr double defaultAutoSeconds = 0.004;

/** A saved shape id, with anything unrecognised resolving to `linear` rather
    than to whatever an out of range cast would land on. Losing a shape choice
    is a fade that is less expressive than it was; reading one off the end of
    the enum is undefined behaviour, and in a switch with no default it is a
    gain that is never written. */
inline Shape shapeFromId (int id) noexcept
{
    return (id >= 0 && id < numShapes) ? (Shape) id : Shape::linear;
}

inline int idOfShape (Shape s) noexcept { return (int) s; }

/** The rising half of a shape: 0 at x = 0 and 1 at x = 1, monotone between.

    There is no second formula for the falling half. A fade out is this read
    backwards, which is what makes the crossfade identities exact rather than
    approximate: sin((1 - u) * pi/2) is cos(u * pi/2) by the identity and not
    by rounding, so a fade out meeting a fade in of the same shape across the
    same overlap satisfies its law to the last bit. Two separate formulas would
    agree to a few decimals and drift, and the whole point of choosing a law is
    that it holds.

    A non-finite position returns zero. It cannot happen from a finite clip and
    a finite transport, but the cost of being wrong matters: a gain of zero
    loses one clip for one sample, where a NaN gain poisons the insert buffer
    for the rest of the block and everything that sends off it.
*/
inline double risingGain (Shape s, double x) noexcept
{
    if (! std::isfinite (x))
        return 0.0;

    x = std::clamp (x, 0.0, 1.0);

    switch (s)
    {
        case Shape::equalPower:  return std::sin (x * 1.5707963267948966);           // x * pi/2
        case Shape::sCurve:      return 0.5 * (1.0 - std::cos (x * 3.141592653589793));
        case Shape::linear:      break;
    }
    return x;
}

/** What a clip asks for, in seconds of output time. Zero or less means the
    edge has no fade of its own and the project's automatic length fills it in,
    which is Reaper's manual-and-auto pair kept as one stored number per end
    plus one project-wide number. */
struct Request
{
    double in  = 0.0;
    double out = 0.0;
    Shape  inShape  = defaultShape;
    Shape  outShape = defaultShape;

    // Whether each edge may take the project's automatic de-click fade. The
    // caller sets these false where another clip's edge meets this one
    // exactly, and that is not a refinement, it is the difference between
    // de-clicking and damage.
    //
    // Clips here do not overlap unless somebody overlaps them, so two
    // abutting clips' automatic fades would be **consecutive rather than
    // simultaneous**: the first would ramp down to nothing across its last
    // four milliseconds and the second would ramp up from nothing across its
    // first four, and the composite gain would pass through exactly zero at
    // the join. That is a hole, not a crossfade, and at a join where the
    // waveform was continuous, which is what `Project::split` and
    // `Project::duplicate` produce, it is strictly worse than the hard cut it
    // replaced: measured on a split, up to 0.99 of full scale of error and
    // three milliseconds of silence that was not there before.
    //
    // Ableton gets a crossfade at an abutting join by overlapping the
    // material, which is what "adjacent audio clips will automatically get
    // 4 ms crossfades" means, and it is the overlap doing the work rather
    // than the fades. Here that overlap has to be made by dragging one clip's
    // edge out over the other, which the renderer already sums correctly. So
    // at an exact join the automatic fade is declined instead, leaving the
    // join bit for bit what it was, and a drawn fade is left alone because
    // somebody who drags a handle there means it.
    bool autoIn  = true;
    bool autoOut = true;
};

/** What the clip's length can actually carry. Seconds of output time. */
struct Resolved
{
    double in  = 0.0;
    double out = 0.0;
    Shape  inShape  = defaultShape;
    Shape  outShape = defaultShape;
};

/** Turns what a clip asks for into what fits in it.

    `clipSeconds` is the clip's duration in output time, which for a stretched
    or warped clip is its length on the arrangement and not the length of the
    audio behind it: a fade is a gesture on the timeline, so its length is
    measured where the listener is.

    `autoSeconds` is the project's automatic de-click length, and zero or less
    turns it off.

    Three rules, and the reason each is the rule:

    - **Each fade is clamped to the clip, never scaled.** A fade length is a
      time, so trimming a clip moves its fade with the edge and leaves its
      length alone. Nothing here rewrites what the clip stored, which is what
      makes trimming reversible: drag the edge back out and the fade comes back
      at the length it was, where a studio that rescaled on trim or reset on
      overflow would have lost it.

    - **The two fades may not overlap.** Ableton states it flatly: "A clip's
      start and end fades cannot overlap each other." Worth copying
      deliberately, because Ardour's library layer clamps each fade
      independently and then applies both to the same samples where they meet,
      which is an implementation gap rather than a behaviour. Overlapping fades
      would multiply two ramps together and dip in the middle of a clip that
      nobody asked to dip.

    - **When they would overlap, the clip is split between them in proportion
      to what each end could carry on its own, which past the clip length is
      the clip length.** Each fade is clamped to the clip first and the ratio
      is taken between the clamped figures, which is deliberately not the
      ratio of what was asked for. Unclamped, one absurd request swallows the
      other end: a clip asking for a hundred seconds in while taking the four
      millisecond automatic fade out would resolve to 0.99996 and 0.00004 of
      its length, so the de-click fade at the other edge, the one that exists
      to stop a click, would be gone. Clamped first it keeps it.

      Something has to give, and no studio publishes a rule for this: Ardour's
      library clamps each one and lets them collide, Ardour on a split resets a
      fade that no longer fits to its default, and Ableton and Pro Tools refuse
      the gesture at the point it is made rather than resolving it afterwards.
      This rule is symmetric between the two ends, continuous in the clip
      length so a clip being trimmed does not jump, and leaves the two fades
      meeting at one instant where the gain reaches unity, so there is no step
      anywhere. The stored lengths are untouched, so this is a view of an
      impossible request rather than a change to it.
*/
inline Resolved resolve (double clipSeconds, const Request& r, double autoSeconds) noexcept
{
    Resolved out;

    if (! std::isfinite (clipSeconds) || clipSeconds <= 0.0)
        return out;     // nothing to fade, and both shapes left at the default

    const double autoLen = (std::isfinite (autoSeconds) && autoSeconds > 0.0) ? autoSeconds : 0.0;

    // An edge that asked for nothing gets the de-click fade and the de-click
    // shape with it. Taking the shape too is the point: the stored shape
    // belongs to the fade somebody drew, and an edge with no drawn fade has no
    // opinion about it.
    if (std::isfinite (r.in) && r.in > 0.0)  { out.in  = r.in;  out.inShape  = r.inShape;  }
    else                                     { out.in  = r.autoIn ? autoLen : 0.0; out.inShape  = declickShape; }

    if (std::isfinite (r.out) && r.out > 0.0) { out.out = r.out; out.outShape = r.outShape; }
    else                                      { out.out = r.autoOut ? autoLen : 0.0; out.outShape = declickShape; }

    out.in  = std::min (out.in,  clipSeconds);
    out.out = std::min (out.out, clipSeconds);

    const double total = out.in + out.out;
    if (total > clipSeconds)
    {
        // total is strictly positive here, because clipSeconds is.
        out.in  = clipSeconds * (out.in / total);

        // The remainder rather than the other proportion, so the two meet to
        // within a bit rather than to within two. `windowFor` absorbs what is
        // left, which is why it does so rather than for tidiness.
        out.out = clipSeconds - out.in;
    }

    return out;
}

/** The two ramps in absolute beats, which is what the renderer has per sample.

    Built once per clip on the message thread and copied into every entry the
    clip produces, so a take folder's outer edges fade while the crossfades at
    its internal comp joins are left to `CompModel.h`, and the two multiply
    rather than fight. An entry wholly inside the clip, which is every comped
    stretch but the first and the last, is outside both windows and costs two
    comparisons per sample.

    The same conversion the comp uses, and for the same reason: the fade is
    kept in seconds so that changing the tempo does not change how long a
    de-click fade lasts, and only the ramps become beats, because beats are the
    renderer's coordinate.
*/
struct Window
{
    double inStart  = 0.0, inEnd  = 0.0;    // the fade in ramps up across this
    double outStart = 0.0, outEnd = 0.0;    // and the fade out ramps down across this
    Shape  inShape  = defaultShape;
    Shape  outShape = defaultShape;
};

/** `beatsPerSecond` is bpm/60. `startBeat` and `endBeat` are the clip's span
    on the arrangement. */
inline Window windowFor (double startBeat, double endBeat,
                         const Resolved& r, double beatsPerSecond) noexcept
{
    Window w;
    w.inShape  = r.inShape;
    w.outShape = r.outShape;

    w.inStart  = startBeat;
    w.outEnd   = endBeat;

    if (! std::isfinite (startBeat) || ! std::isfinite (endBeat) || ! std::isfinite (beatsPerSecond)
        || endBeat <= startBeat || beatsPerSecond <= 0.0)
    {
        // A degenerate span carries no fades rather than a window that could
        // read outside itself. Both ramps are collapsed onto the start, which
        // `gainAt` reads as unity everywhere, and which keeps
        // `inEnd <= outStart` true even for a reversed span: that inequality
        // is what makes "no sample is in both ramps" a property of the window
        // rather than something to be inferred, and an invariant with an
        // exception is not an invariant.
        w.inStart = w.inEnd = w.outStart = w.outEnd = startBeat;
        return w;
    }

    w.inEnd    = std::min (startBeat + r.in  * beatsPerSecond, endBeat);
    w.outStart = std::max (endBeat   - r.out * beatsPerSecond, startBeat);

    // This one is load bearing rather than defensive, which is worth writing
    // down because its sibling above is the opposite. `resolve` guarantees the
    // two lengths fit, but the engine reaches here through a division by the
    // tempo and a multiplication back by it, and the pair lands one unit in
    // the last place past the span on a few per cent of real positions.
    // Removing this line produces overlapping ramps at ordinary places, such
    // as a four beat clip forty minutes into a session at 128 bpm, and
    // `ClipFadeTest` catches that.
    //
    // The `startBeat` clamp on `w.outStart` just above, on the other hand, is
    // provably dead: `w.inEnd` is at least `startBeat` for any non-negative
    // length on a non-degenerate span, and this line then raises `outStart` to
    // at least `w.inEnd`, so the `startBeat` term can never be the one that
    // binds. It is kept because the symmetry with the line above it is worth
    // more than one removed comparison, and because a future change to either
    // expression would need it. Documented rather than tested, the way
    // `PanLaw.h` documents its own no-op.
    w.outStart = std::max (w.outStart, w.inEnd);

    return w;
}

/** The fade gain at a beat. Unity everywhere outside the two ramps, which is
    every clip with no fades and the whole middle of every clip with them.

    The two branches are exclusive by construction, because `windowFor`
    guarantees `inEnd <= outStart`, so no sample can be multiplied by both
    ramps. An empty ramp reads as unity rather than as a division by zero.
*/
inline double gainAt (const Window& w, double beat) noexcept
{
    if (beat < w.inEnd)
        return w.inEnd > w.inStart
             ? risingGain (w.inShape, (beat - w.inStart) / (w.inEnd - w.inStart))
             : 1.0;

    if (beat >= w.outStart)
        return w.outEnd > w.outStart
             ? risingGain (w.outShape, (w.outEnd - beat) / (w.outEnd - w.outStart))
             : 1.0;

    return 1.0;
}

} // namespace ClipFade
