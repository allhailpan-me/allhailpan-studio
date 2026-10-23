// ---------------------------------------------------------------------------
// Checks where playback wraps when no loop range is set.
//
// The fault this guards against was a short sound dropped into the playlist
// repeating back to back, because the loop ended exactly where the clip did.
// It should run to the end of its bar and leave the rest of the bar silent.
//
// The property, over thousands of generated clip ends: the loop end is a whole
// number of bars, at least one bar, never earlier than the clip ends (less the
// tolerance), and less than one bar after it. A clip that ends on a bar line,
// or a hair past one through rounding, must not gain a bar of silence.
// ---------------------------------------------------------------------------

#include "SongLength.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <string>

static int failures = 0;

static void check (bool ok, const std::string& what)
{
    if (! ok)
    {
        std::printf ("  FAIL  %s\n", what.c_str());
        ++failures;
    }
}

static void checkEqual (double a, double b, const std::string& what)
{
    check (a == b, what + "  (" + std::to_string (a) + " vs " + std::to_string (b) + ")");
}

int main()
{
    using SongLength::loopEndBeats;

    // The report itself: a one beat sound plays, then three beats of silence.
    checkEqual (loopEndBeats (1.0), 4.0, "a one beat sound loops at the end of bar one");
    checkEqual (loopEndBeats (0.05), 4.0, "a click a twentieth of a beat long still gets a bar");
    checkEqual (loopEndBeats (1.0e-4), 4.0, "a clip shorter than the tolerance still loops, not stops looping");
    checkEqual (loopEndBeats (3.5), 4.0, "three and a half beats round up to the bar");
    checkEqual (loopEndBeats (4.5), 8.0, "spilling into bar two plays bar two out");
    checkEqual (loopEndBeats (13.0), 16.0, "a clip ending in bar four plays to the end of bar four");

    // A clip cut to the bar must not gain a bar.
    checkEqual (loopEndBeats (4.0), 4.0, "exactly one bar stays one bar");
    checkEqual (loopEndBeats (16.0), 16.0, "exactly four bars stay four bars");
    checkEqual (loopEndBeats (4.0 + 4.0e-4), 4.0, "a loop a few samples over the bar line does not gain a bar");
    checkEqual (loopEndBeats (64.0 - 1.0e-12), 64.0, "floating point noise under a bar line stays on it");
    checkEqual (loopEndBeats (4.01), 8.0, "a hundredth of a beat over is a real overhang");

    // Nothing in the playlist means no loop, which the engine reads from 0.
    checkEqual (loopEndBeats (0.0), 0.0, "an empty playlist does not loop");
    checkEqual (loopEndBeats (-2.0), 0.0, "a negative end does not loop");
    checkEqual (loopEndBeats (std::numeric_limits<double>::quiet_NaN()), 0.0, "NaN does not loop");
    checkEqual (loopEndBeats (std::numeric_limits<double>::infinity()), 0.0, "infinity does not loop");
    checkEqual (loopEndBeats (2.0, 0.0), 0.0, "a bar of no beats does not loop");

    // Another bar length, for when there is a time signature.
    checkEqual (loopEndBeats (1.0, 3.0), 3.0, "three four rounds to three beats");
    checkEqual (loopEndBeats (7.0, 3.0), 9.0, "three four rounds to whole bars of three");

    std::mt19937 rng (20261022);
    std::uniform_real_distribution<double> ends (1.0e-6, 2000.0);
    std::uniform_int_distribution<int> barChoice (2, 7);
    for (int i = 0; i < 20000 && failures < 20; ++i)
    {
        const double perBar = (double) barChoice (rng);
        const double end    = ends (rng);
        const double loop   = loopEndBeats (end, perBar);
        const std::string at = "end " + std::to_string (end) + ", " + std::to_string (perBar) + " per bar, loop " + std::to_string (loop);

        const double bars = loop / perBar;
        check (bars == std::floor (bars), "whole bars: " + at);
        check (loop >= perBar, "at least one bar: " + at);
        check (loop >= end - SongLength::kBarLineTolerance, "not before the clip ends: " + at);
        check (loop < end + perBar, "less than a bar past the clip: " + at);
    }

    if (failures > 0)
    {
        std::printf ("SongLengthTest: %d failure(s)\n", failures);
        return 1;
    }
    std::printf ("SongLengthTest: all checks passed\n");
    return 0;
}
