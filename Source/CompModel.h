#pragma once
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Comping: the arithmetic that turns a pile of takes into one performance.
//
// A take folder is one clip on the arrangement holding several recordings of
// the same section, which is what loop recording produces: set a loop over
// eight bars, sing it ten times, and the ten passes are ten takes of the same
// span rather than ten clips on top of each other. The comp is the choice of
// which take is heard where, and it is what plays and what exports. The takes
// themselves are untouched, so a choice made at two in the morning can be
// unmade the next day. That is what every studio means by a take folder: Logic
// calls the choosing "quick swipe comping" and Pro Tools calls the takes "comp
// lanes", and in both the point is that the comp is a decision laid over the
// recordings rather than an edit of them.
//
// The choice is per section, not per take. Dragging across the clip assigns a
// region to one take, which is the difference between comping and merely
// switching takes: the first verse can come from pass three and its last line
// from pass seven.
//
// A boundary between two takes is crossfaded. Cutting at a zero crossing is
// not enough here, because the two sides are different performances: the
// waveform is continuous at the join but its slope is not, and a voice clicks
// on that. The crossfade is equal power, meaning the two gains satisfy
// gIn^2 + gOut^2 = 1 at every point, which is the shape recommended for
// material that is not phase coherent: two takes of one singer are not the
// same waveform, so their powers add rather than their amplitudes, and a
// linear fade would dip audibly in the middle of every join. Writing it as
// sin and cos of a quarter cycle makes the identity exact rather than
// approximate, since sin^2 + cos^2 = 1 is the Pythagorean identity, and it is
// the same law the Web Audio API's equal-power panner is defined by.
//
// Everything here is in seconds measured from the folder's own start, not in
// beats. That is the one choice in this header worth arguing about, and the
// reason is that a comp is a decision about a performance rather than about
// the grid: "take the line from pass seven" means a stretch of singing, and if
// the project tempo is changed afterwards the stretch of singing has to still
// be the same stretch of singing. Boundaries kept in beats would slide along
// the audio by exactly the tempo ratio and the comp would come apart. The
// crossfade is in seconds for the same reason only more so: what it has to be
// long enough to hide is a click, and a click has never heard of a tempo.
//
// Keeping this in seconds also keeps it free of JUCE and of the project, so it
// compiles on its own in a second under sanitizers. See
// Tests/CompModelTest.cpp.
// ---------------------------------------------------------------------------

/** One boundary in the comp: from `at` until the next boundary, take `take`
    is heard. The folder's start is an implicit boundary, so a normalised list
    always has one at zero and the whole folder is always covered. */
struct CompSegment
{
    double at = 0.0;     // seconds into the folder
    int    take = 0;     // index into the folder's takes

    bool operator== (const CompSegment&) const = default;
};

/** Where one take's audio actually sits inside the folder. Loop recording
    gives every pass the same span, but a pass that was armed late, or stopped
    early, does not cover the whole folder, and a comp must not be allowed to
    read past the end of one. */
struct CompTakeSpan
{
    double start  = 0.0;   // seconds into the folder where this take begins
    double length = 0.0;   // seconds of it there are

    double end() const noexcept { return start + length; }
};

// Two boundaries closer together than this ask for a segment shorter than the
// crossfade that would sit inside it, and come from a double click or a drag
// that went nowhere. They are dropped rather than defended against further
// down, the same way warp markers are.
inline constexpr double kMinCompSpan = 1.0e-4;   // seconds, a tenth of a millisecond

// Far more of each than a session produces: a hundred passes over eight bars
// is a long morning, and a comp with four thousand joins in it is not a comp.
// The caps are here so a corrupt or hostile project file cannot hand the audio
// thread a table of any size it likes.
inline constexpr size_t kMaxCompSegments = 4096;
inline constexpr size_t kMaxCompTakes    = 512;

/** The equal-power pair at a point `t` through a crossfade, t running 0 to 1.
    The outgoing take uses the first, the incoming take the second, and their
    squares sum to exactly one at every t. */
inline double compFadeOutGain (double t) noexcept
{
    return std::cos (std::clamp (t, 0.0, 1.0) * 1.5707963267948966);
}

inline double compFadeInGain (double t) noexcept
{
    return std::sin (std::clamp (t, 0.0, 1.0) * 1.5707963267948966);
}

/** The ramps at the two ends of one comp span. Deliberately unitless: the
    model builds these in folder seconds, and the engine converts them to
    arrangement beats once, when it builds its snapshot, because beats are
    what it has per sample. An empty window (to <= from) means no ramp, which
    is the case at the folder's own edges and for every clip that is not
    comped, so the audio thread pays two comparisons for the ordinary case. */
struct CompFade
{
    double inFrom = 0.0, inTo = 0.0;      // equal-power rise across this window
    double outFrom = 0.0, outTo = 0.0;    // equal-power fall across this window
};

/** The gain a comped source is read at. The two windows never overlap, which
    is what `buildCompSpans` limits the crossfade widths to guarantee, so this
    is only ever inside one of them and the product never attenuates twice. */
inline double compFadeGain (const CompFade& f, double at) noexcept
{
    double gain = 1.0;

    if (f.inTo > f.inFrom)
        gain *= compFadeInGain ((at - f.inFrom) / (f.inTo - f.inFrom));

    if (f.outTo > f.outFrom)
        gain *= compFadeOutGain ((at - f.outFrom) / (f.outTo - f.outFrom));

    return gain;
}

/** Puts a segment list into the form everything below relies on: sorted,
    strictly increasing, starting at zero, inside the folder, with every take
    index in range and no two neighbours naming the same take.

    Where two segments land on the same spot the later one wins, because an
    edit is appended to the list and then normalised, so "later" means "what
    the producer just did".
*/
inline void normaliseCompSegments (std::vector<CompSegment>& segments,
                                   int numTakes, double folderSeconds)
{
    if (numTakes <= 0 || ! (folderSeconds > 0.0) || ! std::isfinite (folderSeconds))
    {
        segments.clear();
        return;
    }

    const int lastTake = std::min (numTakes, (int) kMaxCompTakes) - 1;

    // Anything that is not a number goes first. Sorting with one still in the
    // list breaks the comparator's strict weak ordering, which is undefined
    // behaviour, and a project file is not to be trusted that far.
    segments.erase (std::remove_if (segments.begin(), segments.end(),
                                    [] (const CompSegment& s) { return ! std::isfinite (s.at); }),
                    segments.end());

    for (auto& s : segments)
    {
        s.at = std::max (0.0, s.at);
        s.take = std::clamp (s.take, 0, lastTake);
    }

    // A boundary at or past the folder's end governs nothing.
    segments.erase (std::remove_if (segments.begin(), segments.end(),
                                    [folderSeconds] (const CompSegment& s)
                                    {
                                        return s.at >= folderSeconds - kMinCompSpan;
                                    }),
                    segments.end());

    std::stable_sort (segments.begin(), segments.end(),
                      [] (const CompSegment& a, const CompSegment& b) { return a.at < b.at; });

    std::vector<CompSegment> kept;
    kept.reserve (segments.size() + 1);

    for (const auto& s : segments)
    {
        // Later wins at the same position, so the newest edit is the one that
        // survives a drag that landed on an existing boundary.
        if (! kept.empty() && s.at - kept.back().at < kMinCompSpan)
            kept.back() = s;
        else
            kept.push_back (s);

        if (kept.size() >= kMaxCompSegments)
            break;
    }

    // The folder's start is an implicit boundary. A list that does not open
    // at zero has a stretch at the front belonging to nobody, which can only come
    // from a file written by something else, and the first take is the honest
    // answer for it: that is what an uncomped folder plays there anyway.
    if (kept.empty() || kept.front().at > 0.0)
        kept.insert (kept.begin(), CompSegment { 0.0, kept.empty() ? 0 : kept.front().take });
    else
        kept.front().at = 0.0;

    // Two neighbours naming the same take are one span with a join drawn
    // through it, and that join would be crossfaded between a take and itself,
    // which for phase coherent material is the one case where equal power
    // overshoots. Merging them is both tidier and more correct.
    segments.clear();
    for (const auto& s : kept)
        if (segments.empty() || segments.back().take != s.take)
            segments.push_back (s);
}

/** Which take is heard at a point in the folder. */
inline int compTakeAt (const std::vector<CompSegment>& segments, double at) noexcept
{
    if (segments.empty())
        return 0;

    auto it = std::upper_bound (segments.begin(), segments.end(), at,
                                [] (double a, const CompSegment& s) { return a < s.at; });
    if (it == segments.begin())
        return segments.front().take;
    return (it - 1)->take;
}

/** Assigns a region of the folder to one take, which is the comping gesture:
    drag across the part of a pass that is the good one and it takes over
    there, leaving everything either side as it was.

    The list is left normalised, so a region dragged over an existing boundary
    swallows it, and a region that reaches the folder's start or end simply
    extends to it.
*/
inline void setCompRegion (std::vector<CompSegment>& segments,
                           double fromSeconds, double toSeconds, int take,
                           int numTakes, double folderSeconds)
{
    if (! std::isfinite (fromSeconds) || ! std::isfinite (toSeconds) || numTakes <= 0)
        return;

    if (toSeconds < fromSeconds)
        std::swap (fromSeconds, toSeconds);

    fromSeconds = std::max (0.0, fromSeconds);
    toSeconds   = std::min (toSeconds, folderSeconds);

    if (toSeconds - fromSeconds < kMinCompSpan)
        return;

    // Read before writing: what played at the far edge is what has to carry on
    // from there, or the new region would run to the end of the clip.
    normaliseCompSegments (segments, numTakes, folderSeconds);
    const int after = compTakeAt (segments, toSeconds);

    segments.erase (std::remove_if (segments.begin(), segments.end(),
                                    [fromSeconds, toSeconds] (const CompSegment& s)
                                    {
                                        return s.at >= fromSeconds && s.at <= toSeconds;
                                    }),
                    segments.end());

    segments.push_back ({ fromSeconds, take });
    if (toSeconds < folderSeconds - kMinCompSpan)
        segments.push_back ({ toSeconds, after });

    normaliseCompSegments (segments, numTakes, folderSeconds);
}

/** Repairs a comp after a take has been deleted, given the index it had.

    Takes are addressed by position, so removing one in the middle renumbers
    every take above it, and a comp that was not renumbered with it would
    quietly start playing the wrong pass. A segment that pointed at the take
    that went falls back to whichever take now sits at that position, which is
    the next pass down if there was one. `numTakes` is the count afterwards.
*/
inline void compTakeRemoved (std::vector<CompSegment>& segments, int removedIndex,
                             int numTakes, double folderSeconds)
{
    if (numTakes <= 0)
    {
        segments.clear();
        return;
    }

    const int fallback = std::clamp (removedIndex, 0, numTakes - 1);

    for (auto& s : segments)
    {
        if (s.take == removedIndex)      s.take = fallback;
        else if (s.take > removedIndex)  --s.take;
    }

    normaliseCompSegments (segments, numTakes, folderSeconds);
}

/** One stretch of the comp: a take, the boundaries it owns, the crossfades at
    those boundaries, and the span of the clip over which it is actually read.

    The nominal boundaries tile the folder exactly: the first starts at zero,
    each one's end is the next one's start, and the last ends at the folder's
    length. The read span is wider than that by the crossfade half widths,
    because a take has to start sounding before its boundary for the fade to be
    a fade, and narrower where the take's own audio runs out.
*/
struct CompSpan
{
    int    take = 0;
    double start = 0.0, end = 0.0;                // the boundaries this take owns, in folder seconds
    double fadeIn = 0.0, fadeOut = 0.0;           // half widths, centred on those boundaries
    double readFrom = 0.0, readTo = 0.0;          // where the take is read, clamped to its audio

    bool isAudible() const noexcept { return readTo > readFrom; }

    /** The ramps, in the same folder seconds. */
    CompFade fade() const noexcept
    {
        return { start - fadeIn, start + fadeIn,
                 end   - fadeOut, end   + fadeOut };
    }
};

/** Turns a comp into the spans that are played.

    `crossfadeSeconds` is the full width of a join, so each side of it is half
    that. A join is narrowed where the spans either side are too short to hold
    it, which keeps the two windows at consecutive boundaries from overlapping.
    That non-overlap is what makes the equal-power identity hold: inside a
    window exactly two takes are sounding, and their squares sum to one. Let
    three overlap and the sum is anybody's guess.
*/
inline std::vector<CompSpan> buildCompSpans (const std::vector<CompSegment>& segments,
                                             const std::vector<CompTakeSpan>& takes,
                                             double folderSeconds,
                                             double crossfadeSeconds)
{
    std::vector<CompSpan> spans;

    if (takes.empty() || ! (folderSeconds > 0.0) || ! std::isfinite (folderSeconds))
        return spans;

    auto working = segments;
    normaliseCompSegments (working, (int) takes.size(), folderSeconds);
    if (working.empty())
        return spans;

    const size_t count = working.size();
    const double half  = std::isfinite (crossfadeSeconds) ? std::max (0.0, crossfadeSeconds) * 0.5 : 0.0;

    auto boundary = [&] (size_t i) { return i < count ? working[i].at : folderSeconds; };

    // The half width usable at a boundary is limited by both spans touching
    // it, so that the window cannot reach the next boundary's window. The
    // folder's own edges get none: there is nothing on the far side to fade
    // to, and a comp that faded in at the start would quietly swallow the
    // first syllable of the performance.
    auto halfWidthAt = [&] (size_t i) -> double
    {
        if (i == 0 || i >= count)
            return 0.0;
        const double before = boundary (i)     - boundary (i - 1);
        const double after  = boundary (i + 1) - boundary (i);
        return std::min ({ half, before * 0.5, after * 0.5 });
    };

    spans.reserve (count);

    for (size_t i = 0; i < count; ++i)
    {
        CompSpan s;
        s.take         = working[i].take;
        s.start   = boundary (i);
        s.end     = boundary (i + 1);
        s.fadeIn  = halfWidthAt (i);
        s.fadeOut = halfWidthAt (i + 1);

        const auto& audio = takes[(size_t) std::clamp (s.take, 0, (int) takes.size() - 1)];

        // Reading one sample past a take's own audio is how a comp acquires a
        // click that no crossfade explains, so the range is clamped here
        // rather than relied on being in range further down.
        s.readFrom = std::max (s.start - s.fadeIn,  audio.start);
        s.readTo   = std::min (s.end   + s.fadeOut, audio.end());
        if (s.readTo < s.readFrom)
            s.readTo = s.readFrom;

        spans.push_back (s);
    }

    return spans;
}

/** What the comp sums to at a point, counting every span sounding there. One
    inside a span, one at a join, and zero only where no take has any audio.
    Used by the tests to show the joins neither dip nor peak.
*/
inline double compTotalPower (const std::vector<CompSpan>& spans, double at) noexcept
{
    double power = 0.0;
    for (const auto& s : spans)
    {
        if (at < s.readFrom || at >= s.readTo)
            continue;
        const double g = compFadeGain (s.fade(), at);
        power += g * g;
    }
    return power;
}
