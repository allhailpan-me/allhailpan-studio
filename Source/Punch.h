#pragma once
#include "CompModel.h"
#include <algorithm>
#include <cmath>
#include <vector>

//==============================================================================
/** Punch in and punch out recording: which part of a pass replaces what was
    already on the track, and where the joins at either end of it go.

    WHAT A PUNCH IS FOR

    A verse is right apart from one line. The ordinary answer is to record the
    whole verse again and hope the rest of it comes out as well as it did the
    first time. The studio answer is to play along from a bar or two before the
    bad line, let the recorder take over for exactly that line, and hand the
    performance back afterwards. Everything outside the punch is the take that
    was already there, untouched.

    THE FAULT THIS EXISTS TO FIX

    Recording here finds an empty track rather than punching into an occupied
    one, so a punch take laid over the performance it was meant to replace
    would have summed with it: two vocals at once, both at full level, for the
    length of the punch. That is not one of the models anybody ships. It is
    worth saying plainly how far outside the field it is, because this was the
    reason punch recording was deferred on eight separate nights and the reason
    it needed the manuals read rather than an afternoon's work:

      - Logic Pro's `Overlapping Track Recordings > Audio` menu offers Create
        Take Folder, Create Track and Create Track Alternative, and nothing
        else. Its MIDI menu, on the same preferences page, offers `Merge`,
        `Overlap` and a whole `Replace` submenu. Apple exposes summing for MIDI
        and deliberately does not expose it for audio.
      - Pro Tools: "Recording with QuickPunch is nondestructive", and what it
        produces is a clip in a playlist over a continuous parent file.
      - Ardour layers the new region opaquely so that "Only the top layer will
        be played", or in `Non-Layered` mode trims the regions underneath "so
        that there are no overlaps", which "does not affect the previously
        recorded audio data, and trimmed regions can be expanded again at
        will."
      - Cubase and Nuendo offer `Keep History`, `Cycle History + Replace` and
        `Replace`, where what is replaced is "existing events or portions of
        events", so a clip and not a file.
      - Studio One is non destructive and "automatically crossfaded at its
        edges with the existing Audio Event".
      - Reaper layers takes by default and trimming is the opt in its own
        action list calls "tape mode".

    Three things every one of them agrees on, so they are taken here as
    standard rather than as a choice: a punch does not sum with what is under
    it, the audio file under it is not modified, and the join wants a
    crossfade.

    WHY THIS IS A COMP

    Because the comp is already exactly this. `CompModel.h` says which take is
    heard where and puts an equal-power crossfade at each join; it is in
    seconds rather than beats, so a comp survives a tempo change; and
    `Clip::compSpans` already turns it into reads that `pushArrangement` hands
    to the engine. So a punch is a three segment comp: the original up to the
    punch in, the new pass across the punch, the original after the punch out.

    That is the whole reason this model was chosen over trimming the clip
    underneath, which Ardour specifies just as completely. A trim would need a
    second mechanism parallel to the comp, and it would not get the export for
    free. The comp does, which for a punch matters for the same reason
    `CLAUDE.md` already gives for comping: what was punched in is the
    performance, and a bounce that differs from what was heard is among the
    worst faults a studio can have.

    THE GEOMETRY, WHICH IS THE PART THAT WOULD HAVE BEEN GOT WRONG

    The crossfades lie entirely outside the punch, in the old material. From
    the Pro Tools Reference Guide version 6, chapter 14, on QuickPunch: it
    writes "a pre-crossfade at the punch-in point (which occurs up to but not
    into the punched region boundary)" and "a post-crossfade at the punch-out
    point (which occurs after the punched region)".

              pre-crossfade           punched region          post-crossfade
          |<---- width ---->|                             |<---- width ---->|
          ===================+=============================+==================
                        punch in                      punch out
           (old material)            (new material)           (old material)

    So the pre-crossfade *ends* at the punch in and the post-crossfade
    *starts* at the punch out, and the punch itself is at full gain from end to
    end. Neither of the two obvious readings is right. `CompModel.h` centres
    its crossfade on a join, which is correct for comping two passes of the
    same performance and wrong here: a centred fade would attenuate the first
    and last few milliseconds of the take that was just punched in, and the
    attack is usually the reason somebody punched. Avid's own advice is to
    crossfade "without removing essential audio".

    The fix needs no change to the comp model, and that is the point. Put the
    comp boundary half a width *before* the punch in and half a width *after*
    the punch out. The centred window then lands exactly where Avid puts it.

    It is also safe when the comp model narrows a join for want of room, which
    it does at `buildCompSpans`: narrowing only shortens the fade and finishes
    it earlier, so the new take is still at full gain across every sample of
    the punch, and what gets consumed is a little more old material in front of
    the punch in. Which is what Avid describes anyway.

    One condition makes that guarantee hold, and it is the reason `Result`
    carries a crossfade width out as well as boundaries: **the folder has to
    carry the width the boundaries were shifted by.** The realised half width
    at a join is at most half the folder's own crossfade setting, so shifting
    by half of that same setting is what puts the fade's end at the punch in
    rather than past it. Hand `buildCompSpans` a wider crossfade than the
    boundaries were built for and the fade reaches into the punch, which is the
    fault this whole header exists to avoid. `TakeFolder::crossfadeMs` is
    already per folder rather than global, so a punched folder carrying the
    punch width is in keeping with what is there.

    WHY THE WHOLE PASS IS CAPTURED AND ONLY THE PUNCH IS HEARD

    Unanimous in the sources, and it is what makes the geometry above possible
    at all: the pre-crossfade needs new take material in a stretch the user did
    not punch, so it can only exist if the recorder was already running there.

    Avid: "Pro Tools begins recording a new file when playback begins,
    automatically generating clips in that file at each punch in/out point",
    and "the complete audio file appears in the Clips List along with the
    QuickPunch created clips". Logic captures "approximately one bar before the
    punch-in locator" and says what that buys: "This allows the start point of
    the region to be adjusted later, enabling you to hear the lead-in, if
    desired."

    Three published behaviours rest on it and none is available without it: the
    pre-crossfade, dragging the punch open afterwards to hear more of the
    performance, and Pro Tools' "instantaneous monitor switching on punch-out"
    with no dropout. That last one lands straight on this project's rule that
    the audio thread must not allocate: the file is opened when the transport
    rolls, never at the punch point.

    The happy consequence is that the engine needs no change for any of this.
    The recorder here already writes the whole pass and `RecordAlign.h` already
    decides where it lands. A punch is entirely a question of which take the
    comp selects, which is why it arrives as arithmetic rather than as a new
    record mode in the device callback.

    THE WIDTH

    Ten milliseconds, and it is a choice between two published suggestions
    rather than a specification, so it is recorded as one. The preference in
    Pro Tools is `Quick Punch/Track Punch Crossfade Length`, under Setup >
    Preferences > Editing, in the Fades section. Avid's current user guide says
    "10ms is a good starting point"; the version 6 Reference Guide said "A good
    general-purpose crossfade length for punches is 4 milliseconds"; Audacity's
    punch and roll publishes 10 ms as its actual default. Avid publishes no
    default and no range for the preference itself, which was looked for in
    four of their documents and not found. Two independent products landing on
    10 ms is why it is the one taken.

    THE RANGE

    The punch in and punch out are the loop locators, which is a published
    default and not a shortcut. Cubase: "The punch in position is locked to the
    left locator position and the punch out position to the right locator
    position", with a `Lock Punch Points to Locators` toggle that frees them.
    Studio One's Auto Punch sets "the Left Locator" and "the Right Locator",
    which are the same two its loop recording uses, and it publishes no way to
    separate them. Unlocking the pair is worth having and is deliberately left
    for later rather than half built.

    One sided punches are here because they are cheap and are what an engineer
    wants half the time. Ardour publishes all three cases: "In only" records
    "from the In marker on, without an end boundary", "Out only" records "until
    the Out marker, without a beginning boundary", and "In and Out" records
    "only between the In and Out markers".

    No JUCE in here, so Tests/PunchTest.cpp drives all of it, including the
    geometry, through the real `buildCompSpans` rather than through a
    restatement of it.
*/
namespace Punch
{

/** The punch crossfade, following Avid's current published suggestion. See the
    header comment for why this is a choice between two of their own figures
    rather than a specification. */
inline constexpr double kDefaultCrossfadeMs = 10.0;

/** A punch shorter than this is a stab at a button rather than a phrase, and
    there is nothing to put a join either side of. The same figure the comp
    already refuses a segment shorter than, for the same reason. */
inline constexpr double kMinPunchSeconds = kMinCompSpan;

//==============================================================================
/** Where the punch starts and stops, in beats, because the loop locators it
    comes from are in beats.

    Either boundary can be absent, which is Ardour's In only and Out only. Both
    absent is not a punch: it would mean the whole pass replaces the whole clip,
    and that is an ordinary recording rather than a punch, so it is refused
    rather than silently treated as one.
*/
struct Range
{
    double inBeat  = 0.0;
    double outBeat = 0.0;
    bool   hasIn   = true;
    bool   hasOut  = true;

    bool operator== (const Range&) const = default;
};

/** Whether this range can punch anything at all. */
inline bool isUsable (const Range& r) noexcept
{
    if (r.hasIn && ! std::isfinite (r.inBeat))
        return false;
    if (r.hasOut && ! std::isfinite (r.outBeat))
        return false;
    if (! r.hasIn && ! r.hasOut)
        return false;                       // see Range: that is not a punch
    if (r.hasIn && r.hasOut)
        return r.outBeat > r.inBeat;
    return true;
}

/** Whether the punch is live at a transport position.

    This is what the record button reports, and the convention it reports with
    is published twice: Logic's "If you enable Autopunch during recording, the
    Record button flashes while the playhead is outside the punch area", and
    Sequoia's record button, which "will flash until the In marker has been
    reached", flashes again after the Out marker, and lights continuously
    between them. So a studio tells the player that it is armed and waiting
    rather than armed and recording, and that distinction is worth having
    because without it the only way to know whether a punch is going to happen
    is to try it.
*/
inline bool recordingAt (const Range& r, double beat) noexcept
{
    if (! isUsable (r) || ! std::isfinite (beat))
        return false;

    if (r.hasIn && beat < r.inBeat)
        return false;
    if (r.hasOut && beat >= r.outBeat)
        return false;

    return true;
}

/** Beats into a folder, as seconds into it.

    The comp is in seconds and the punch arrives in beats, so this conversion
    happens once, when the punch lands, and never again. That is the whole
    reason `CompModel.h` is in seconds: convert on every read instead and
    changing the project tempo afterwards would drag the joins through the
    performance, so the line that was punched in would stop being the line that
    was punched in.

    Negative is a real answer and is not clamped: a punch can begin before the
    clip it is punching into, and the caller needs to know by how much.
*/
inline double secondsInto (double beat, double folderStartBeat, double bpm) noexcept
{
    if (! std::isfinite (beat) || ! std::isfinite (folderStartBeat) || ! (bpm > 0.0))
        return 0.0;

    return (beat - folderStartBeat) * 60.0 / bpm;
}

//==============================================================================
/** What the new pass has to offer, in seconds into the folder.

    `from` and `to` are where its audio sits, which is wider than the punch
    because the whole pass was captured. `from` may be negative, meaning the
    pass began before the clip being punched into.

    `folderSeconds` is how long the folder will be once the pass is in it,
    which is not necessarily how long it was: a punch can run past the end of
    what was there.
*/
struct Material
{
    double from          = 0.0;
    double to            = 0.0;
    double folderSeconds = 0.0;
};

/** The comp region a punch becomes, in folder seconds, plus the crossfade the
    folder has to carry for the geometry to hold.

    `from` and `to` are already shifted off the punch points by half a width,
    so they are comp boundaries and not punch boundaries. They are the numbers
    to hand to `setCompRegion`; `applyTo` does that and is the ordinary way in.
*/
struct Result
{
    double from             = 0.0;
    double to               = 0.0;
    double crossfadeSeconds = 0.0;
    bool   valid            = false;

    /** How much of the old material in front of the punch the pre-crossfade
        consumes. Nothing depends on it; it is here because it is the one
        number somebody looking at a punched join will want to know. */
    double leadInSeconds() const noexcept { return crossfadeSeconds; }
};

/** Works out the comp region, the shift, and the width that can actually be
    carried.

    THE WIDTH IS THE SMALLEST THING THAT FITS, and each limit on it is a
    separate fact about the material rather than a safety margin:

      - The pre-crossfade sits in [punch in minus a width, punch in] and has to
        read the new pass there, so it cannot be wider than the pass's own lead
        in. A player who starts recording a millisecond before the punch point
        has given the join nothing to work with, and the honest answer is a
        shorter join rather than a fade that reads off the front of the file
        and clicks.
      - The post-crossfade sits in [punch out, punch out plus a width] and has
        to read the new pass there, so the same holds at the other end.
      - It is not made wider than the punch itself. That one is a decision
        rather than a published figure, and it is recorded as a decision: no
        manual says anything about it, and nothing breaks without it, but
        replacing 20 ms of a performance either side of a 5 ms punch is not
        what somebody asked for.

    One width serves both joins because a folder carries one crossfade length,
    so the tighter of the two ends decides it. That is a real limitation and it
    is written down rather than hidden: a punch whose in point sits right on the
    start of the capture gets no crossfade at its out point either.

    A width of zero is a valid answer and means a butt join, which is what
    Ardour, Logic and Reaper do at a punch boundary anyway. The comp model
    already gives no fade at the folder's own edges for its own reasons, so a
    punch that reaches an edge gets the same treatment there.
*/
inline Result regionFor (const Range& r,
                         double folderStartBeat, double bpm,
                         const Material& m,
                         double requestedCrossfadeSeconds = kDefaultCrossfadeMs * 0.001)
{
    Result out;

    if (! isUsable (r) || ! (m.folderSeconds > 0.0) || ! std::isfinite (m.folderSeconds))
        return out;
    if (! std::isfinite (m.from) || ! std::isfinite (m.to) || m.to <= m.from)
        return out;

    const double inSec  = r.hasIn  ? secondsInto (r.inBeat,  folderStartBeat, bpm) : 0.0;
    const double outSec = r.hasOut ? secondsInto (r.outBeat, folderStartBeat, bpm) : m.folderSeconds;

    if (! std::isfinite (inSec) || ! std::isfinite (outSec))
        return out;
    if (outSec - inSec < kMinPunchSeconds)
        return out;

    // WHERE THE PUNCH CAN ACTUALLY REACH, which is the punch narrowed by two
    // separate facts about the world rather than by any kind of margin.
    //
    // By the clip, because a comp cannot play past the end of the clip it is
    // on, and a punch range is set on the timeline where the clip may not be:
    // the loop locators can sit before a clip starts or after it ends, and a
    // punch that misses has nothing to replace.
    //
    // By the pass, because the player decides when to stop. Somebody who
    // punches four bars and stops after two has recorded two, and the other
    // two have to come back as they were. Clamping here is what makes that
    // happen: without it the comp would hand those two bars to a take with no
    // audio in them, `buildCompSpans` would clamp the read to nothing, and the
    // punch would replace the second half of the phrase with silence. Which is
    // worse than either keeping it or replacing it, and says nothing.
    const double reachFrom = std::max (std::max (inSec,  0.0),             m.from);
    const double reachTo   = std::min (std::min (outSec, m.folderSeconds), m.to);

    if (reachTo - reachFrom < kMinPunchSeconds)
        return out;

    double width = std::isfinite (requestedCrossfadeSeconds)
                     ? std::max (0.0, requestedCrossfadeSeconds)
                     : 0.0;

    // Not wider than the punch, measured over the part of it that is really
    // being replaced rather than over the part that was asked for. A decision
    // rather than a published figure: see above.
    width = std::min (width, reachTo - reachFrom);

    // What the pass can feed each join. A join needs the new take to have
    // audio on the far side of it, and where it has none the honest answer is
    // no fade rather than a fade that reads off the end of the file. Both
    // limits apply whichever boundary the punch range declares, because what
    // decides whether there is a join at a given end is whether the comp has a
    // boundary there, and after the clamping above it may have one at an end
    // the range left open.
    width = std::min (width, std::max (0.0, reachFrom - m.from));
    width = std::min (width, std::max (0.0, m.to - reachTo));

    const double half = width * 0.5;

    // The shift, which is the whole geometry: half a width earlier at the in
    // point and half a width later at the out point puts each centred window
    // entirely outside the punch, where Avid puts it. A boundary that has
    // fallen on the folder's own edge is shifted past it and `setCompRegion`
    // clamps it back, which is right: the comp model gives no fade at an edge
    // because there is nothing on the far side to fade to.
    out.from = reachFrom - half;
    out.to   = reachTo   + half;

    out.crossfadeSeconds = width;
    out.valid            = true;

    return out;
}

/** Puts the punch into a comp, and sets the folder's crossfade to the width
    the boundaries were built for.

    THE TWO ARE ONE OPERATION, which is why they are one function and why the
    folder's width is taken by reference rather than left to the caller. The
    shift is half of a width and the realised half width at a join is at most
    half of whatever the folder carries, so the two have to be the same number
    or the geometry the header is about does not hold. Leave the folder at a
    wider setting than the punch was narrowed to and the fade reaches into the
    punch, attenuating the attack that was the reason for punching, and nothing
    announces it: the punch is still there, still in the right place, just
    quietly ramped at its front. Two separate calls would make forgetting the
    second one easy and the result impossible to see.

    The comp is left normalised, because `setCompRegion` leaves it that way,
    which is what keeps the spans tiling the folder with no gap and no overlap.
    Clamping the region to the folder is `setCompRegion`'s job and is left to
    it rather than repeated here.

    Returns false and changes nothing where the punch could not be worked out,
    so a caller that ignores the return value gets the performance left as it
    was rather than a comp built from a fallback nobody chose.
*/
inline bool applyTo (std::vector<CompSegment>& comp, double& folderCrossfadeMs,
                     const Result& region,
                     int newTake, int numTakes, double folderSeconds)
{
    if (! region.valid || numTakes <= 0)
        return false;
    if (newTake < 0 || newTake >= numTakes)
        return false;

    setCompRegion (comp, region.from, region.to, newTake, numTakes, folderSeconds);
    folderCrossfadeMs = region.crossfadeSeconds * 1000.0;
    return true;
}

//==============================================================================
/** Where a captured pass sits inside the clip it is being punched into.

    THIS IS THE LINE THAT WAS WRONG, so it lives here where it can be tested
    rather than in the window where it could not be.

    A pass's position inside a folder is seconds from the folder's start, and a
    folder's start is its clip's start. For an ordinary recording those are the
    same thing, because the clip is created at the first pass's own position,
    and the window's existing arithmetic measures from there. A punch lands in a
    clip that is already somewhere else, and the transport rolled in a bar
    before the punch rather than at the clip, so measuring from the roll in puts
    the audio in the folder offset by the distance between the two.

    What that produced was not a wrong number anybody would see. It was the
    punched take sitting far enough along the folder that the comp's read of it
    clamped to nothing, so the bars that were punched went silent in both
    playback and the bounce, with the window reporting "Punched in over bars 5
    to 7. The rest of the take is untouched." Where the pass was long enough to
    cover the region anyway, it played three bars of the wrong part of the
    performance at full level with a clean crossfade either side. Nothing
    errored either way.

    `head` is the pass's own pre-roll, the audio in front of where the pass is
    heard from, which `RecordAlign.h` keeps in the file so a trim can be taken
    back. `duration` is the whole file. The answer is in the three numbers a
    `Take` needs, and it never carries a negative position: a pass that began
    before the clip has the difference taken off its front instead, because the
    comp cannot read in front of the folder, so that audio has no part to play
    and a take sitting before the folder it is in is a second case for every
    lookup to handle.
*/
struct Placement
{
    double start  = 0.0;   // seconds into the folder where the pass begins
    double offset = 0.0;   // seconds into the source where it begins
    double length = 0.0;   // seconds of source it runs for
    bool   valid  = false; // false when there is nothing of it left to hear
};

inline Placement placePass (double passStartBeat, double clipStartBeat, double bpm,
                            double headSeconds, double durationSeconds) noexcept
{
    Placement out;

    if (! std::isfinite (headSeconds) || ! std::isfinite (durationSeconds))
        return out;

    const double head = std::max (0.0, headSeconds);
    const double into = secondsInto (passStartBeat, clipStartBeat, bpm);

    if (! std::isfinite (into))
        return out;

    // How much of the pass sits before the clip it is being punched into, and
    // is therefore unreachable by the comp.
    const double before = std::max (0.0, -into);

    out.start  = std::max (0.0, into);
    out.offset = head + before;
    out.length = durationSeconds - out.offset;

    // A pass entirely in front of the clip, or shorter than its own pre-roll,
    // has nothing left to be heard. Saying so beats a take of no length in a
    // folder, which draws as a lane with nothing in it.
    out.valid = out.length > 0.0;

    return out;
}

/** Whether a punch would land on a clip that is already carrying something,
    which is the question that decides whether this is a punch at all or just
    a recording. Both in beats, the clip's end exclusive.

    A punch over empty timeline is an ordinary take and must stay one: wrapping
    it in a folder would mean every recording made with the punch left on came
    back as something that needs comping.
*/
inline bool overlaps (const Range& r, double clipStartBeat, double clipEndBeat) noexcept
{
    if (! isUsable (r) || ! std::isfinite (clipStartBeat) || ! std::isfinite (clipEndBeat))
        return false;
    if (clipEndBeat <= clipStartBeat)
        return false;

    const double from = r.hasIn  ? r.inBeat  : clipStartBeat;
    const double to   = r.hasOut ? r.outBeat : clipEndBeat;

    return to > clipStartBeat && from < clipEndBeat;
}

} // namespace Punch
