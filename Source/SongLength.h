#pragma once
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// Where playback wraps when no loop range is set.
//
// It used to wrap at the exact end of the last clip. Drop a short sound into
// an empty playlist, press play, and that is a sound repeated back to back with
// no gap, which is a stutter rather than a bar of music. A pattern is thought
// of in bars, so the arrangement now runs to the end of the bar the last clip
// finishes in, and the rest of that bar is silence.
//
// There is no standard that defines this. Whole bars is what was asked for and
// is what a producer expects when auditioning a beat, so it is the choice here.
// The studio has no time signature setting: the ruler numbers a bar every four
// beats and the metronome accents every fourth, so a bar is four beats here
// too. Pass something else once a time signature exists.
//
// No JUCE in here, so Tests/SongLengthTest.cpp builds it on its own.
// ---------------------------------------------------------------------------

namespace SongLength
{

static constexpr double kBeatsPerBar = 4.0;

/** How far past a bar line a clip may end and still count as ending on it.

    An audio clip's length is its source length in seconds times the tempo, so
    a one bar loop rarely comes out at exactly four beats: a few samples of
    rounding in the file or in a detected tempo puts it a hair over. Without a
    tolerance that hair would add a whole bar of silence to a loop that was cut
    to length, which is the opposite of the fix. A thousandth of a beat is half
    a millisecond at 120 bpm, far below anything audible as a gap and far above
    floating point noise.
*/
static constexpr double kBarLineTolerance = 1.0e-3;

/** The beat playback wraps at, given the end of the last clip in beats.

    Nothing in the playlist, or a value that is not a number, gives 0, which the
    engine reads as "do not loop". Anything else gives a whole number of bars
    that is at least one bar and never earlier than the last clip ends, less
    the tolerance above.
*/
inline double loopEndBeats (double lastClipEnd, double beatsPerBar = kBeatsPerBar) noexcept
{
    if (! std::isfinite (lastClipEnd) || lastClipEnd <= 0.0
        || ! std::isfinite (beatsPerBar) || beatsPerBar <= 0.0)
        return 0.0;

    const double bars = std::ceil (lastClipEnd / beatsPerBar - kBarLineTolerance / beatsPerBar);
    return std::max (1.0, bars) * beatsPerBar;
}

} // namespace SongLength
