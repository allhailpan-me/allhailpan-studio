#pragma once
#include "AudioDefaults.h"
#include <algorithm>
#include <cmath>
#include <vector>

//==============================================================================
/** Where a recorded take belongs on the timeline.

    THE FAULT THIS EXISTS TO FIX

    A player hears the arrangement out of the speakers, plays along with it,
    and their playing arrives back at the studio later than they played it: the
    whole way round through both converters, and behind whatever the mixer is
    holding back for a lookahead plugin. A capture written down at the
    transport position of the block it arrived in is therefore that much later
    than the player's hands.

    Some of this was already being put back, but in the window rather than in
    the engine: MainComponent's finishAudioRecording read past the head of each
    pass by the device's reported round trip. Right idea, wrong place, and
    wrong in four ways.

      - It left out the studio's own compensation. Put a lookahead limiter on
        the master and every take went back to landing late by its whole
        lookahead, because what the player hears is the mix after that delay.
      - It was clamped to half the length of a pass, so a short take was
        quietly half corrected rather than corrected or refused.
      - In a loop recording it threw audio away. The head of each pass was read
        past, but the pass boundaries were left where the transport wrapped, so
        the last few milliseconds of every pass but the final one were handed
        to the next pass and then skipped at its head. The player's last note
        before the loop came round went in the bin.
      - It sat in the window with no test over it, which is how the first three
        survived.

    All of which is the worst kind of wrong, the kind that does not announce
    itself. Nothing errors, nothing crackles, no meter moves. The guitar is
    simply behind the drums, and since nobody else is going to take the blame
    for that, the player does.

    WHAT TO COMPENSATE BY

    JUCE defines the two device figures at the callback boundary, which is what
    makes them add up to the answer. From juce_AudioIODevice.h, 9.0.3:

      getOutputLatencyInSamples  "the delay in samples between a callback
                                  getting a block of data, and that data
                                  actually getting played"
      getInputLatencyInSamples   "the delay in samples between some audio
                                  actually arriving at the soundcard, and the
                                  callback getting passed this block of data"

    So follow one note around. The block rendered in the callback at transport
    position P reaches the player's ears `engine + output` samples later, where
    `engine` is how far behind the playhead the mix leaves the studio: the
    delay compensation holding the tracks together plus the master's own
    effects, which is what AudioEngine::getPluginLatencySamples reports. The
    player plays at that moment. Their note enters the socket there and is
    handed to a callback `input` samples after that. The transport has moved on
    by `input + output + engine` in the meantime, so a note belonging at P is
    delivered when the transport says P plus all three. That sum is what has to
    come back off.

    This is also what Ardour does, read from its source rather than guessed at.
    Route::update_signal_latency sets each processor's capture_offset to the
    route's input latency and its playback_offset to its own signal latency
    plus the output latency; DiskWriter::check_record_status then does

        _first_recordable_sample += _capture_offset + _playback_offset

    for any track whose alignment style is ExistingMaterial, which
    Track::set_align_choice_from_io picks for exactly the tracks that are fed
    from a physical input. The instrument recorder here is the other kind: it
    taps a channel's own output inside the mixer and never touches a socket, so
    it is Ardour's CaptureTime case and gets no offset at all.

    Note the form of Ardour's fix, because it is the one adopted here. It does
    not move the clip earlier. It treats the capture as beginning that many
    samples further in and writes that point down at the transport position,
    which is the same alignment arrived at from the other end and has two
    advantages: no take ever needs a negative start, and every pass of a loop
    recording comes out stamped with the beat it was actually recorded at, so
    the passes stay aligned with each other inside a take folder.

    The audio in front of that point is kept rather than thrown away. It is
    the player's response to something before the record point, so it is not
    heard, but it is written to the file and reported as the pass's pre-roll,
    which is what lets the left edge of a take be dragged back into it and
    what makes a badly set trim recoverable instead of permanent. Only the
    first pass of a loop recording has any: for every pass after it, the audio
    in front of its start is the tail of the pass before, and it is already in
    that pass's own file rather than duplicated into this one.

    What this does cost is the other end. The take now ends a few milliseconds
    earlier than it used to, because the studio stops capturing the moment the
    transport stops and the last of the performance is still in flight. Ardour
    buys that back by keeping the capture running past the stop point
    (_last_recordable_sample is moved by the same amount); here it is left
    alone, because it is five milliseconds of ring-out a second after the last
    note.

    WHY THERE IS A TRIM AS WELL

    Because the figures are only what the driver claims. JUCE's ASIO backend
    reports zero for both when the driver's getLatencies call fails, which
    plenty of drivers never implement at all, so a reported zero means "it did
    not say" and the fallback of two buffers understates a real round trip by
    both converters. Ardour's manual puts it plainly: "the only way to
    accurately learn about the total (additional) latency is to measure it."
    Every other studio offers the same escape hatch, Pro Tools as a record
    offset, Reaper as a manual offset per direction, Cubase alongside its
    "Adjust for Record Latency", so there is one here too, in milliseconds
    because what it corrects is a claim rather than a count.

    No JUCE in here, so Tests/RecordAlignTest.cpp drives all of it. The engine
    keeps only the part that genuinely needs JUCE, which is asking the device
    for two numbers.
*/
namespace RecordAlign
{
    /** The point at which the transport jumped back to the start of the loop:
        how far into the capture it happened, and the beat it landed on. */
    struct Wrap
    {
        int    frame = 0;
        double beat  = 0.0;
    };

    /** One pass of the capture that is kept.

        `firstFrame` to `lastFrame` is the half open range of capture frames
        the pass holds, which is what goes into its file. `preRollFrames` is
        how many frames at the front of that belong before the pass's own
        start, so the audio the pass is heard from begins at
        `firstFrame + preRollFrames` and that is the frame written down at
        `startBeat`.

        The two are kept apart rather than collapsed into one number because
        they answer different questions. Dropping the head would align the
        take just as well, and that is what the first version of this did, but
        it also means the audio is not written down anywhere: a trim set wrong
        would then take the attack off every take recorded under it with no way
        back. Keeping it in the file and starting the clip after it is what
        every other studio does, and it leaves the left edge of a take
        something to be dragged back into.
    */
    struct Pass
    {
        int    firstFrame    = 0;
        int    lastFrame     = 0;      // exclusive
        int    preRollFrames = 0;      // of those, how many sit before startBeat
        double startBeat     = 0.0;

        int frames() const noexcept { return lastFrame - firstFrame; }

        /** The first frame that is heard, which is the one placed at
            startBeat. */
        int audibleFrom() const noexcept { return firstFrame + preRollFrames; }

        /** How much of the pass is heard. Zero means the whole of it belongs
            before its own start, which is a pass with nothing in it. */
        int audibleFrames() const noexcept { return lastFrame - audibleFrom(); }
    };

    /** Everything the offset is made of. All sample counts, except the trim,
        which is a correction to a claim rather than a count of anything. */
    struct Figures
    {
        int    inputLatency  = 0;   /**< what the device reports on the way in */
        int    outputLatency = 0;   /**< and on the way out */
        int    bufferSize    = 0;   /**< used only when it reports neither */
        int    engineLatency = 0;   /**< how far behind the playhead the mix leaves */
        double sampleRate    = 0.0;
        double trimMs        = 0.0; /**< the user's correction, positive pulling takes earlier */
    };

    /** The trim's bounds, in milliseconds.

        Fifty each way is far more than any driver misreports: it is two and a
        half times the round trip of the slowest buffer Windows offers, so
        anything outside it is a different fault being papered over. Positive
        pulls takes earlier, which is the direction somebody reaches for,
        because an uncompensated studio is always late and never early.
    */
    inline constexpr double minTrimMs = -50.0;
    inline constexpr double maxTrimMs =  50.0;

    /** How many samples of the capture belong before the record point, which
        is therefore how many to drop off the front of it.

        Negative is legal and means the opposite: the driver claimed more than
        it should have and the take belongs later than it was captured. Only a
        trim big enough to overcome a real round trip can produce it, and it is
        handled rather than clamped away, because a studio that silently
        ignores half of a control's range is a studio that looks broken.
    */
    inline int captureOffsetSamples (const Figures& f) noexcept
    {
        // Two buffers is the floor, applied here even when the driver did say
        // something. A driver that answers for one direction and not the other
        // is a real shape of answer, and it would otherwise be believed to be
        // instant on the side it went quiet about.
        //
        // The floor is empirical rather than a law of physics: every backend
        // JUCE 9.0.3 ships adds at least one period per direction whenever
        // that direction's device exists. WASAPI returns the driver's figure
        // plus the buffer size, CoreAudio its latency plus a safety offset
        // plus the buffer, DirectSound one and a half buffers each way, ALSA
        // frames times periods minus one, and JACK the port's own total. Only
        // ASIO passes the driver's answer straight through, and that is the
        // one that zeroes both figures when the driver will not answer.
        //
        // Deliberately not pushed back into the driver search's own ranking:
        // that would change which device a first run picks, on hardware that
        // cannot be tried from here.
        const int roundTrip = std::max (AudioDefaults::roundTripSamples (f.inputLatency,
                                                                         f.outputLatency,
                                                                         f.bufferSize),
                                        2 * std::clamp (f.bufferSize, 0,
                                                        AudioDefaults::maxLatencySamples));

        const int engine = std::clamp (f.engineLatency, 0, AudioDefaults::maxLatencySamples);

        int trim = 0;

        // The finite test first: clamping a NaN hands the NaN back, and the
        // conversion to int after that is undefined behaviour rather than a
        // wrong answer. A settings file is not to be trusted that far.
        if (std::isfinite (f.trimMs) && f.sampleRate > 0.0)
            trim = (int) std::lround (std::clamp (f.trimMs, minTrimMs, maxTrimMs)
                                      * std::min (f.sampleRate, 768000.0) / 1000.0);

        return std::clamp (roundTrip + engine + trim,
                           -AudioDefaults::maxLatencySamples, AudioDefaults::maxLatencySamples);
    }

    /** Splits one continuous capture into the passes a loop recording
        produces, with the offset applied.

        `rawStartBeat` is the transport position of the first frame, and each
        wrap is the point inside the capture where the transport went back
        round, both exactly as the audio thread noted them. `offsetSamples` is
        the figure above and `beatsPerSample` the project tempo at the time.

        The whole of the arithmetic is one line of intent: capture frame f ends
        up at the beat the transport was at when f arrived, less the offset.
        Everything below is that, made safe against a capture, a wrap list or a
        tempo that a recording with dropped samples or a hostile project file
        could produce. Tests/RecordAlignTest.cpp asserts the one line directly
        rather than any of the steps.

        That line is stated for a positive offset, which is every offset a
        working studio produces. A negative one, which needs a trim larger than
        the whole round trip, is treated differently on purpose: the take moves
        later as a whole rather than being re-cut. Taken literally the line
        would send the audio just before a loop wrap forward past the loop end
        and into the next pass, which is arithmetic winning an argument with
        music. The test asserts that behaviour separately.

        A pass with no audio in it is dropped rather than returned empty, which
        is what happens when the transport wraps twice inside one block: a loop
        shorter than the buffer size does that.
    */
    inline std::vector<Pass> passes (int totalFrames, double rawStartBeat,
                                     const Wrap* wraps, int numWraps,
                                     int offsetSamples, double beatsPerSample)
    {
        std::vector<Pass> result;

        if (totalFrames <= 0)
            return result;

        if (wraps == nullptr)
            numWraps = 0;

        numWraps = std::max (0, numWraps);

        if (! std::isfinite (beatsPerSample) || beatsPerSample < 0.0)
            beatsPerSample = 0.0;

        // The two directions the offset can go, as two separate quantities,
        // because they are applied to different things. Dropping frames moves
        // audio earlier against the timeline and is how the ordinary positive
        // case is served; there is no way to drop frames backwards, so the
        // other direction moves every beat stamp later instead. Never both at
        // once, and neither of them when the offset is zero.
        const int    frameShift = std::max (0, offsetSamples);
        const double beatShift  = (double) (frameShift - offsetSamples) * beatsPerSample;

        auto stamp = [beatShift] (double rawBeat)
        {
            // A beat that is not a number comes from a tempo or a transport
            // that was already broken, and zero is the only honest answer for
            // it: the take is at the start of the arrangement, where it is
            // visible, rather than at some distance nobody can scroll to.
            if (! std::isfinite (rawBeat))
                return 0.0;

            return std::max (0.0, rawBeat + beatShift);
        };

        auto shiftFrame = [totalFrames, frameShift] (int frame, int notBefore)
        {
            // Clamped against the frame before it as well as against the
            // capture, so the boundaries stay in order however the wrap list
            // arrived. An out of order boundary would otherwise produce a pass
            // reading backwards.
            const long long moved = (long long) frame + (long long) frameShift;
            return (int) std::clamp (moved, (long long) notBefore, (long long) totalFrames);
        };

        // The first pass keeps the audio in front of its own start as
        // pre-roll. Every pass after it does not: what is in front of its
        // start is the previous pass's tail, which is already in the previous
        // pass's own file.
        int    from     = 0;
        int    preRoll  = std::min (frameShift, totalFrames);
        double fromBeat = stamp (rawStartBeat);

        auto emit = [&result] (int first, int last, int pre, double beat)
        {
            // Something audible in it, rather than merely something in it. A
            // pass that is all pre-roll is a pass that belongs entirely before
            // its own start, which is what a stab at the record button
            // shorter than the round trip produces.
            if (last - first > pre)
                result.push_back ({ first, last, pre, beat });
        };

        for (int i = 0; i < numWraps; ++i)
        {
            // Never before this pass's own audible start, so the boundaries
            // stay in order and no pass is handed a negative length however
            // the wrap list arrived.
            const int at = shiftFrame (wraps[i].frame, from + preRoll);
            emit (from, at, preRoll, fromBeat);
            from     = at;
            preRoll  = 0;
            fromBeat = stamp (wraps[i].beat);
        }

        emit (from, totalFrames, preRoll, fromBeat);

        return result;
    }

    /** Where a frame of the capture ended up, or `ifMissing` when it was not
        kept at all. A frame inside a pass's pre-roll answers with where it
        would have gone, which is before that pass's own start: it is in the
        file and the clip's left edge can be dragged back over it.

        Here rather than in the test so that the property being asserted is
        written down next to the code that has to produce it. Linear, because
        it is only ever walked over by a test.
    */
    inline double placementOf (const std::vector<Pass>& p, int frame, double beatsPerSample,
                               double ifMissing = -1.0e9) noexcept
    {
        for (const auto& pass : p)
            if (frame >= pass.firstFrame && frame < pass.lastFrame)
                return pass.startBeat + (double) (frame - pass.audibleFrom()) * beatsPerSample;

        return ifMissing;
    }
}
