// ---------------------------------------------------------------------------
// Checks the conversion between a MIDI file's ticks and this project's beats.
//
// A part that leaves this studio and is opened in another one is judged
// instantly: it either sits with the drums or it does not. The failure mode is
// not a crash, it is a part a few milliseconds early that nobody can explain,
// or a note so short the receiving studio throws it away. So the properties
// here are checked over hundreds of thousands of generated notes rather than
// a few hand picked ones, at the tempos and resolutions that actually break
// this, and the exactness claim is checked as exactness rather than as "close
// enough".
//
// MidiTimebase.h has no JUCE in it for exactly this reason, so there is
// nothing to stub.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include "MidiTimebase.h"

#include <cstdio>
#include <random>
#include <set>
#include <string>

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

static std::string str (double v) { return std::to_string (v); }
static std::string str (long long v) { return std::to_string (v); }

// The resolutions a file in the wild actually uses. 96 is the old sequencer
// default, 480 and 960 are what studios write, 120 and 384 turn up in files
// from trackers and hardware.
static const int ppqs[] = { 96, 120, 192, 240, 384, 480, 960, 1024 };

// ---------------------------------------------------------------------------
// A tick taken to beats and back has to be the same tick.
//
// This is the whole claim, and it is not free: a tick divided by 960 is not a
// number a double holds exactly, so the only reason it survives is that the
// error stays far below half a tick. Checking it at every tick across several
// bars, at every resolution, is cheap and makes that concrete rather than
// assumed.

static void testTickBeatIdentity()
{
    for (int ppq : ppqs)
    {
        for (long long tick = 0; tick <= (long long) ppq * 64; ++tick)
        {
            const double beats = midiBeatsFromTicks (tick, ppq);
            const long long back = midiTicksFromBeats (beats, ppq);

            if (back != tick)
            {
                fail ("tick " + str (tick) + " at " + str ((long long) ppq)
                      + " ppq came back as " + str (back));
                return;
            }
        }
    }

    // And far out in the arrangement, where the error grows with the value.
    for (int ppq : ppqs)
    {
        for (long long tick : { 1LL << 20, 1LL << 24, (1LL << 29) - 1 })
        {
            const double beats = midiBeatsFromTicks (tick, ppq);
            check (midiTicksFromBeats (beats, ppq) == tick,
                   "tick " + str (tick) + " at " + str ((long long) ppq) + " ppq did not survive");
        }
    }
}

// ---------------------------------------------------------------------------
// Every division this studio can produce has to land on a whole tick at the
// resolution it writes, or exported parts quantise themselves on the way out.

static void testProjectDivisionsAreExact()
{
    // Channel rack resolutions: 8, 16 and 32 steps to a four beat bar.
    for (int stepsPerBar : { 8, 16, 32 })
    {
        const double step = 4.0 / (double) stepsPerBar;
        for (int i = 0; i < stepsPerBar * 4; ++i)
        {
            const double beat = (double) i * step;
            const long long tick = midiTicksFromBeats (beat, kMidiExportPpq);
            check (midiBeatsFromTicks (tick, kMidiExportPpq) == beat,
                   "step " + str ((long long) i) + " of " + str ((long long) stepsPerBar)
                       + " per bar is not a whole tick");
        }
    }

    // Note values, straight and in triplets and quintuplets, as the comment in
    // MidiTimebase.h claims.
    const double values[] = { 4.0, 2.0, 1.0, 0.5, 0.25, 0.125, 0.0625, 0.03125,
                              1.0 / 3.0, 0.5 / 3.0, 2.0 / 3.0, 1.0 / 5.0, 1.0 / 6.0 };

    for (double v : values)
    {
        const double ticks = v * (double) kMidiExportPpq;
        check (std::abs (ticks - std::floor (ticks + 0.5)) < 1.0e-9,
               "note value " + str (v) + " beats is " + str (ticks) + " ticks, not a whole number");
    }
}

// ---------------------------------------------------------------------------
// Tempo. The file holds microseconds per quarter note, so the tempo that
// comes back is only as exact as that integer allows, and the test is that the
// error is small enough that nothing drifts audibly, and that a tempo written
// and read is a fixed point.

static void testTempo()
{
    for (double bpm : { 20.0, 60.0, 87.0, 100.0, 120.0, 128.0, 140.0, 174.0, 200.0, 300.0, 999.0 })
    {
        const int us = midiMicrosecondsPerQuarterNote (bpm);
        const double back = midiBpmFromMicrosecondsPerQuarterNote ((double) us);

        check (us >= 1 && us <= 16777215,
               "tempo " + str (bpm) + " wrote " + str ((long long) us)
                   + " microseconds, which does not fit in three bytes");

        // Worst case here is a microsecond of rounding on a quarter note. At
        // 999 bpm that is still under a part in fifty thousand, so a four
        // minute song lands within a few milliseconds of where it started.
        check (std::abs (back - bpm) / bpm < 2.0e-5,
               "tempo " + str (bpm) + " came back as " + str (back));

        // Reading then writing has to give the same bytes, or a project saved
        // and reopened would creep.
        check (midiMicrosecondsPerQuarterNote (back) == us,
               "tempo " + str (bpm) + " is not a fixed point");
    }

    // 128 is this project's own default and divides exactly, so it should be
    // bit for bit identical rather than merely close.
    check (midiMicrosecondsPerQuarterNote (128.0) == 468750, "128 bpm is not 468750 microseconds");
    check (midiBpmFromMicrosecondsPerQuarterNote (468750.0) == 128.0, "468750 microseconds is not 128 bpm");
    check (midiMicrosecondsPerQuarterNote (120.0) == 500000, "120 bpm is not 500000 microseconds");

    // Nonsense must not write a value that reads back as a plausible tempo.
    check (midiMicrosecondsPerQuarterNote (0.0) == 500000, "a zero tempo should fall back to 120 bpm");
    check (midiMicrosecondsPerQuarterNote (-5.0) == 500000, "a negative tempo should fall back to 120 bpm");
    check (midiBpmFromMicrosecondsPerQuarterNote (0.0) == 120.0, "zero microseconds should read as 120 bpm");
}

// ---------------------------------------------------------------------------
// Velocity is seven bits in the file, so it is quantised on the way out. What
// has to hold is that the quantisation is a fixed point, that it never writes
// zero (which is a note off), and that it matches what the engine does when it
// plays a note, so a file and the sound of it agree.

static void testVelocity()
{
    std::set<int> seen;

    for (int i = 0; i <= 1000; ++i)
    {
        const float v = (float) i / 1000.0f;
        const int byteValue = midiVelocityToByte (v);

        check (byteValue >= 1 && byteValue <= 127,
               "velocity " + str (v) + " wrote " + str ((long long) byteValue));

        // The engine's own conversion, from MainComponent's snapshot builder.
        const int engine = std::clamp ((int) std::lround (v * 127.0f), 1, 127);
        check (byteValue == engine,
               "velocity " + str (v) + " wrote " + str ((long long) byteValue)
                   + " but the engine plays " + str ((long long) engine));

        seen.insert (byteValue);
    }

    check (seen.size() == 127, "the velocity mapping does not reach all 127 values");

    // Every byte read and written again is the same byte, which is what makes
    // a second round trip change nothing.
    for (int b = 1; b <= 127; ++b)
        check (midiVelocityToByte (midiVelocityFromByte (b)) == b,
               "velocity byte " + str ((long long) b) + " is not a fixed point");

    check (midiVelocityToByte (0.0f) == 1, "a silent velocity must not be written as a note off");
    check (midiVelocityToByte (-1.0f) == 1, "a negative velocity must clamp up to 1");
    check (midiVelocityToByte (99.0f) == 127, "a velocity above 1 must clamp to 127");
    check (midiVelocityToByte (std::nanf ("")) == 1, "a velocity that is not a number must clamp to 1");
}

// ---------------------------------------------------------------------------
// A note shorter than a tick still has to be a note. This is the case the
// piano roll produces at a fine snap setting, and a note on and note off at
// the same tick is a note most studios discard on load.

static void testShortNotes()
{
    for (int ppq : ppqs)
    {
        const double tickBeats = 1.0 / (double) ppq;

        for (double fraction : { 0.0, 1.0e-9, 0.01, 0.1, 0.49, 0.5, 0.9, 0.999 })
        {
            TimebaseNote n;
            n.start  = 4.0;
            n.length = tickBeats * fraction;

            const auto t = midiNoteToTicks (n, ppq);
            check (t.endTick > t.startTick,
                   "a note " + str (fraction) + " of a tick long collapsed at "
                       + str ((long long) ppq) + " ppq");

            const auto back = midiNoteFromTicks (t, ppq);
            check (back.length >= tickBeats * 0.999,
                   "a note shorter than a tick came back as " + str (back.length) + " beats");
        }

        // A note of exactly zero length is the degenerate case, and the one a
        // stuck note off produces.
        TimebaseNote zero;
        zero.start  = 0.0;
        zero.length = 0.0;
        check (midiNoteToTicks (zero, ppq).endTick == 1, "a zero length note did not become one tick");
    }
}

// ---------------------------------------------------------------------------
// The round trip, over randomly generated patterns.
//
// Notes are generated on the tick grid, because that is what any file this
// studio reads actually contains, and the claim being tested is that a file
// exported and imported again gives back the same notes. Starts, lengths,
// pitches and velocities are compared exactly.

static void testRoundTrip()
{
    std::mt19937 rng (0xA11Du);

    for (int iteration = 0; iteration < 4000; ++iteration)
    {
        const int ppq = ppqs[rng() % (sizeof (ppqs) / sizeof (ppqs[0]))];
        const int count = (int) (rng() % 40);

        std::vector<TickNote> original;
        original.reserve ((size_t) count);

        // A spread of pitches keeps same pitch collisions common rather than
        // rare, since that is the case worth getting right.
        std::uniform_int_distribution<int> pitch (58, 64);
        std::uniform_int_distribution<int> velocity (1, 127);
        std::uniform_int_distribution<long long> start (0, (long long) ppq * 8);
        std::uniform_int_distribution<long long> length (1, (long long) ppq * 2);

        for (int i = 0; i < count; ++i)
        {
            TickNote t;
            t.startTick = start (rng);
            t.endTick   = t.startTick + length (rng);
            t.note      = pitch (rng);
            t.velocity  = velocity (rng);
            original.push_back (t);
        }

        // What a file written from these would hold.
        auto onFile = original;
        midiResolveSamePitchOverlaps (onFile);

        // Resolving is idempotent, which is what keeps a second export
        // identical to the first.
        auto again = onFile;
        const int droppedSecondTime = midiResolveSamePitchOverlaps (again);
        check (droppedSecondTime == 0, "resolving overlaps a second time dropped notes");
        check (again.size() == onFile.size(), "resolving overlaps a second time changed the count");

        for (size_t i = 0; i < again.size() && i < onFile.size(); ++i)
            check (again[i].startTick == onFile[i].startTick
                       && again[i].endTick == onFile[i].endTick
                       && again[i].note == onFile[i].note
                       && again[i].velocity == onFile[i].velocity,
                   "resolving overlaps a second time moved a note");

        // Nothing left on the file may overlap another note of the same pitch,
        // because one channel cannot describe that.
        for (size_t a = 0; a < onFile.size(); ++a)
            for (size_t b = a + 1; b < onFile.size(); ++b)
                if (onFile[a].note == onFile[b].note)
                    check (onFile[a].endTick <= onFile[b].startTick
                               || onFile[b].endTick <= onFile[a].startTick,
                           "two notes of the same pitch still overlap after resolving");

        // Every note still sounds.
        for (const auto& t : onFile)
            check (t.endTick > t.startTick, "resolving overlaps left a note with no length");

        // The round trip itself: ticks to the project's beats and back.
        const auto imported = midiNotesFromTicks (onFile, ppq);
        const auto reExported = midiNotesToTicks (imported, ppq);

        if (reExported.size() != onFile.size())
        {
            fail ("the round trip changed the note count: " + str ((long long) onFile.size())
                  + " became " + str ((long long) reExported.size()));
            return;
        }

        for (size_t i = 0; i < onFile.size(); ++i)
        {
            const auto& was = onFile[i];
            const auto& now = reExported[i];

            if (was.startTick != now.startTick || was.endTick != now.endTick
                || was.note != now.note || was.velocity != now.velocity)
            {
                fail ("the round trip moved a note at " + str ((long long) ppq) + " ppq: "
                      + str (was.startTick) + ".." + str (was.endTick) + " pitch "
                      + str ((long long) was.note) + " velocity " + str ((long long) was.velocity)
                      + "  became  " + str (now.startTick) + ".." + str (now.endTick) + " pitch "
                      + str ((long long) now.note) + " velocity " + str ((long long) now.velocity));
                return;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Off grid notes cannot survive exactly, because a file only holds whole
// ticks. What has to hold is that the error is no worse than half a tick, that
// nothing moves backwards past zero, and that the second round trip is exact,
// so a part does not creep a little further every time it is exported.

static void testOffGridIsBoundedAndSettles()
{
    std::mt19937 rng (0xBEEFu);
    std::uniform_real_distribution<double> beat (0.0, 64.0);
    std::uniform_real_distribution<double> len (0.0, 4.0);

    for (int iteration = 0; iteration < 20000; ++iteration)
    {
        const int ppq = ppqs[rng() % (sizeof (ppqs) / sizeof (ppqs[0]))];
        const double halfTick = 0.5 / (double) ppq;

        TimebaseNote n;
        n.start    = beat (rng);
        n.length   = len (rng);
        n.note     = 60;
        n.velocity = 0.8f;

        const auto onFile   = midiNoteToTicks (n, ppq);
        const auto imported = midiNoteFromTicks (onFile, ppq);

        check (imported.start >= 0.0, "an imported note started before the beginning");
        check (imported.length > 0.0, "an imported note had no length");

        check (std::abs (imported.start - n.start) <= halfTick + 1.0e-12,
               "a start moved " + str (std::abs (imported.start - n.start))
                   + " beats, more than half a tick at " + str ((long long) ppq) + " ppq");

        // The end is a whole tick in the file, so it is either the rounded end
        // or, when rounding the two ends together would have collapsed the
        // note, exactly one tick past the start. Those are the only two
        // outcomes, and stating them separately is worth more than one loose
        // bound covering both: a note that quietly grew by a tick for any
        // other reason would pass a loose bound and is a bug.
        const double endWas = n.start + n.length;
        const double endNow = imported.start + imported.length;
        const double tick   = 1.0 / (double) ppq;

        if (midiTicksFromBeats (endWas, ppq) <= onFile.startTick)
        {
            check (onFile.endTick == onFile.startTick + 1,
                   "a note that rounded to nothing was not held to one tick");
            check (std::abs (imported.length - tick) < 1.0e-12,
                   "a note held to one tick came back as " + str (imported.length) + " beats");
        }
        else
        {
            check (std::abs (endNow - endWas) <= halfTick + 1.0e-12,
                   "an end moved " + str (std::abs (endNow - endWas)) + " beats at "
                       + str ((long long) ppq) + " ppq, more than half a tick");
        }

        // Once it has been through a file it is on the grid, so going round
        // again must not move it at all.
        const auto second = midiNoteToTicks (imported, ppq);
        check (second.startTick == onFile.startTick && second.endTick == onFile.endTick
                   && second.velocity == onFile.velocity && second.note == onFile.note,
               "a second round trip moved a note that was already on the grid");
    }
}

// ---------------------------------------------------------------------------
// The SMPTE form of the division word, where a tick is a fraction of a second.

static void testSmpte()
{
    // 25 frames per second, 40 ticks per frame, which is the millisecond
    // timing the specification gives as an example.
    const double secondsPerTick = (1.0 / 25.0) / 40.0;

    check (std::abs (midiBeatsFromSmpteTicks (1000, secondsPerTick, 120.0) - 2.0) < 1.0e-12,
           "a second of SMPTE ticks at 120 bpm is not two beats");
    check (std::abs (midiBeatsFromSmpteTicks (0, secondsPerTick, 120.0)) < 1.0e-12,
           "tick zero is not beat zero");
    check (midiBeatsFromSmpteTicks (1000, secondsPerTick, 0.0) == 0.0,
           "a zero tempo should not produce a position");
}

// ---------------------------------------------------------------------------
// Values that should never reach here, but do when a file is corrupt.

static void testDegenerateInput()
{
    check (midiTicksFromBeats (-1.0, kMidiExportPpq) == 0, "a negative beat did not clamp to zero");
    check (midiTicksFromBeats (1.0, 0) == 0, "a zero resolution did not clamp to zero");
    check (midiBeatsFromTicks (960, 0) == 0.0, "a zero resolution did not read as beat zero");
    check (midiTicksFromBeats (std::nan (""), kMidiExportPpq) == 0, "a beat that is not a number did not clamp");
    check (midiTicksFromBeats (1.0e30, kMidiExportPpq) == kMidiMaxTick, "an absurd beat did not clamp to the cap");

    TickNote backwards;
    backwards.startTick = 100;
    backwards.endTick   = 20;
    const auto fixed = midiNoteFromTicks (backwards, kMidiExportPpq);
    check (fixed.length > 0.0, "a note off before its note on did not produce a positive length");

    std::vector<TickNote> empty;
    check (midiResolveSamePitchOverlaps (empty) == 0, "resolving an empty pattern reported a loss");
    check (empty.empty(), "resolving an empty pattern produced notes");
}

// ---------------------------------------------------------------------------
// Two notes of the same pitch starting on the same tick cannot both be
// written, and that has to be reported rather than silently losing one.

static void testCollisionsAreReported()
{
    std::vector<TickNote> notes;
    notes.push_back ({ 0,   480, 60, 100 });
    notes.push_back ({ 0,   240, 60, 80 });    // same pitch, same tick
    notes.push_back ({ 0,   480, 62, 100 });   // a different pitch is fine
    notes.push_back ({ 240, 480, 60, 90 });    // overlaps the first, so trims it

    const int dropped = midiResolveSamePitchOverlaps (notes);

    check (dropped == 1, "the duplicate was not reported: " + str ((long long) dropped) + " dropped");
    check (notes.size() == 3, "the wrong number of notes survived");

    // The one kept of the pair is the longer, and it is trimmed to where the
    // next note of that pitch begins.
    for (const auto& n : notes)
        if (n.note == 60 && n.startTick == 0)
            check (n.endTick == 240 && n.velocity == 100,
                   "the surviving duplicate is wrong: ends at " + str (n.endTick)
                       + " velocity " + str ((long long) n.velocity));

    for (const auto& n : notes)
        if (n.note == 62)
            check (n.endTick == 480, "a note of a different pitch was trimmed");
}

// ---------------------------------------------------------------------------
// The clip window, held to the engine's own arithmetic.
//
// A bounce that disagrees with what was heard is the worst bug a studio can
// have, and the export reading a clip differently from the way it is played is
// exactly how that happens. The reference below is transcribed from the
// snapshot builder in MainComponent, which is what the engine plays, so if the
// two ever part company this stops rather than shipping two answers.

static void testClipWindowMatchesTheEngine()
{
    std::mt19937 rng (0xC11Du);
    std::uniform_real_distribution<double> position (-4.0, 16.0);
    std::uniform_real_distribution<double> span (0.0, 8.0);

    for (int iteration = 0; iteration < 200000; ++iteration)
    {
        MidiClipWindow clip;
        clip.start  = position (rng);
        clip.offset = span (rng);
        clip.length = span (rng);

        const double noteStart  = span (rng);
        const double noteLength = span (rng);

        // --- the engine, from MainComponent::pushArrangement ---
        const double engineStart  = clip.start;
        const double engineEnd    = clip.start + clip.length;
        const double engineOrigin = clip.start - clip.offset;
        const double engineOn     = engineOrigin + noteStart;
        const bool   enginePlays  = ! (engineOn < engineStart || engineOn >= engineEnd);
        const double engineOff    = std::min (engineOn + noteLength, engineEnd);

        // --- the export ---
        double at = 0.0, length = 0.0;
        const bool plays = midiClipPlaysNote (clip, noteStart, noteLength, at, length);

        if (plays != enginePlays)
        {
            fail ("the export and the engine disagree about whether a note sounds: clip at "
                  + str (clip.start) + " offset " + str (clip.offset) + " length "
                  + str (clip.length) + ", note at " + str (noteStart));
            return;
        }

        if (! plays)
            continue;

        if (at != engineOn)
        {
            fail ("the export and the engine start a note at different beats: export "
                  + str (at) + " but the engine plays " + str (engineOn));
            return;
        }

        // The engine holds the note's end, the export holds its length, so the
        // two cannot be compared bit for bit: adding the length back gives the
        // end to within a bit or so of the last one. The tolerance is there to
        // allow exactly that and nothing else. Any real disagreement about
        // which window a note belongs to is out by whole beats, so this still
        // catches it.
        const double drift = std::abs ((at + length) - engineOff);
        const double allowed = 8.0 * std::abs (engineOff) * 2.22e-16 + 1.0e-300;

        if (drift > allowed)
        {
            fail ("the export and the engine end a note differently: export "
                  + str (at + length) + " but the engine plays " + str (engineOff)
                  + ", out by " + str (drift));
            return;
        }

        check (length >= 0.0, "a windowed note came out with a negative length");
        check (length <= noteLength + allowed, "a windowed note came out longer than it was written");
        check (at + length <= clip.endBeat() + allowed, "a windowed note ran past the end of its clip");
    }

    // A clip of no length plays nothing, whatever its pattern holds.
    {
        MidiClipWindow empty { 4.0, 0.0, 0.0 };
        double at = 0.0, length = 0.0;
        check (! midiClipPlaysNote (empty, 0.0, 1.0, at, length),
               "a clip of no length still played a note");
    }

    // A note that starts before the clip does not sound, even if it would
    // have run into it. That is the engine's rule, and the export has to
    // agree rather than being helpful.
    {
        MidiClipWindow clip { 0.0, 2.0, 4.0 };   // reads the pattern from beat 2
        double at = 0.0, length = 0.0;
        check (! midiClipPlaysNote (clip, 1.0, 8.0, at, length),
               "a note beginning before the clip's offset was exported");
        check (midiClipPlaysNote (clip, 2.0, 1.0, at, length) && at == 0.0 && length == 1.0,
               "a note on the clip's offset was not placed at the clip's start");
    }

    // A note running past the clip's end is cut off there, not left hanging.
    {
        MidiClipWindow clip { 8.0, 0.0, 4.0 };
        double at = 0.0, length = 0.0;
        check (midiClipPlaysNote (clip, 3.0, 99.0, at, length),
               "a note just inside the clip did not sound");
        check (at == 11.0 && length == 1.0,
               "a note overrunning the clip was not cut off at its end: " + str (at)
                   + " for " + str (length));
    }

    // Values a corrupt project could hold must not produce a note at all
    // rather than a note somewhere impossible.
    {
        MidiClipWindow clip { 0.0, 0.0, 4.0 };
        double at = 0.0, length = 0.0;
        check (! midiClipPlaysNote (clip, std::nan (""), 1.0, at, length),
               "a note at no position was exported");

        MidiClipWindow broken { std::nan (""), 0.0, 4.0 };
        check (! midiClipPlaysNote (broken, 0.0, 1.0, at, length),
               "a clip at no position exported a note");
    }
}

// ---------------------------------------------------------------------------

int main()
{
    std::printf ("Checking the MIDI file timebase\n\n");

    struct { const char* name; void (*run)(); } tests[] =
    {
        { "a tick taken to beats and back is the same tick", testTickBeatIdentity },
        { "every division this studio produces is a whole tick", testProjectDivisionsAreExact },
        { "tempo survives the three byte field", testTempo },
        { "velocity matches what the engine plays", testVelocity },
        { "a note shorter than a tick is still a note", testShortNotes },
        { "notes on the grid round trip exactly", testRoundTrip },
        { "off grid notes move less than half a tick, then settle", testOffGridIsBoundedAndSettles },
        { "the SMPTE timebase reads as beats", testSmpte },
        { "corrupt input cannot produce nonsense", testDegenerateInput },
        { "notes a file cannot hold are reported", testCollisionsAreReported },
        { "the export reads a clip exactly as the engine plays it", testClipWindowMatchesTheEngine },
    };

    for (const auto& t : tests)
    {
        const int before = failures;
        t.run();
        std::printf ("  %s  %s\n", failures == before ? "ok  " : "FAIL", t.name);
    }

    std::printf ("\n");

    if (failures != 0)
    {
        std::printf ("%d check(s) failed.\n", failures);
        return 1;
    }

    std::printf ("All checks passed.\n");
    return 0;
}
