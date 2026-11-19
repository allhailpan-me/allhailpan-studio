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

        /** The device's callback size. A floor under what the driver claims,
            and, for a MIDI capture, a term of the offset in its own right:
            see midiCaptureOffsetSamples. */
        int    bufferSize    = 0;

        int    engineLatency = 0;   /**< how far behind the playhead the mix leaves */

        /** What the MIDI path itself delays by, which is not the audio
            converter's figure and is not any part of it.

            Held at zero, and that is a documented default rather than a claim
            that it is zero. A note from a hardware keyboard waits on USB MIDI
            polling, or on DIN MIDI's 320 microseconds a byte, and then on the
            operating system's MIDI stack, and nothing in JUCE reports any of
            it: grepping the whole of juce_audio_devices/midi_io for "latency"
            in 9.0.3 returns nothing at all.

            Ardour is in the same position and answers it the same way. Its
            backends default the per device systemic MIDI input latency to zero
            (alsa_audiobackend.h:313-322) and expose it as a user figure
            (audio_backend.h:509-518), filled from a MIDI loopback measurement
            with its own Calibrate button, because a number nobody can measure
            is a number nobody can set correctly.

            So the slot is here and the measurement is what is missing. The
            unlock is a MIDI loopback probe, the counterpart of LatencyProbe.h.
            See Research/BACKLOG.md. */
        int    midiInputLatency = 0;

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

    /** The round trip the device claims, with the floor applied: everything
        the offset is made of except the engine and the trim.

        Pulled out of captureOffsetSamples rather than left inside it because
        LatencyProbe::trimMsFor has to subtract exactly this quantity to turn a
        measurement into a trim, and two definitions of it would be free to
        drift apart. A trim computed against a slightly different claim than
        the one the compensation then adds back is a studio that is out by the
        difference, with nothing to say so.
    */
    inline int claimedRoundTripSamples (const Figures& f) noexcept
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
        return std::max (AudioDefaults::roundTripSamples (f.inputLatency,
                                                          f.outputLatency,
                                                          f.bufferSize),
                         2 * std::clamp (f.bufferSize, 0,
                                         AudioDefaults::maxLatencySamples));
    }

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
        const int roundTrip = claimedRoundTripSamples (f);

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


    //==========================================================================
    // WHERE A RECORDED MIDI NOTE BELONGS
    //
    // The same question as the audio one above, for a note played on a
    // keyboard rather than a sound played into a socket, and the answer has
    // the same shape with one term swapped and one term added.
    //
    // Everything the audio derivation says still holds: the block rendered in
    // the callback at transport position P reaches the player's ears
    // `engine + output` samples later, and the player plays at that moment.
    // What differs is the way back in.
    //
    //  - A MIDI note never passes through the input converter, so the audio
    //    input latency is not in it. That much was already written down in the
    //    engine and is what Ardour does: _playback_offset is _signal_latency +
    //    _output_latency (route.cc:4885) and is subtracted from every captured
    //    event time (disk_writer.cc:596-604, :701-706), while _capture_offset
    //    comes from the input port's own latency (route.cc:4890), which for a
    //    MIDI port is not the converter's figure.
    //
    //  - But a whole audio block is, and that is the term the engine's comment
    //    did not have. juce::MidiMessageCollector re-times every message
    //    against a wall clock, and it anchors the END of the buffer it fills
    //    to the instant the callback drained it. Follow the code, 9.0.3:
    //
    //      addMessageToQueue           samplePosition = (t_a - t_prev) * fs
    //      removeNextBlockOfMessages   numSourceSamples = (t_now - t_prev) * fs
    //                                  startSample = numSamples - numSourceSamples
    //                                  position = samplePosition + startSample
    //
    //    which is position = numSamples - (t_now - t_a) * fs. So sample index
    //    `numSamples` is the drain instant and sample 0 is one block before
    //    it: a note played during one block is handed over in the next one and
    //    is written down a block late. The engine stamps it with
    //    blockBeats[position], so that block goes straight into the answer.
    //
    //    This is not a JUCE defect and copying Ardour verbatim would not avoid
    //    it. Its ALSA backend has the identical one period lag: alsa_midi.cc
    //    postpones an event past the window end, and alsa_audiobackend.cc
    //    calls sync_time after the drain, so during cycle N the window base is
    //    still cycle N-1's start. Ardour does not subtract it, because it
    //    expects it to be inside the user's per device MIDI input latency,
    //    which it has a loopback measurement for and this studio does not.
    //    Subtracting the part that is derivable is strictly better than
    //    leaving all of it; see Figures::midiInputLatency for the part that is
    //    not.
    //
    // The direction is an assumption and is worth naming as one, the way
    // Ardour names it at audioengine.cc:521: "We assume the player listens and
    // plays along. Recorded region is moved back by playback_latency." A
    // player monitoring a soft instrument they are playing is the other case
    // and wants no offset, or Cubase's opposite one, which its per track ASIO
    // Latency Compensation provides for exactly that reason. Both follow one
    // rule, that a note belongs at the musical position of whatever the player
    // was hearing when they played it, and they differ because the players are
    // listening to different things. This studio's MIDI tap is fed only from
    // the hardware input and the on-screen keyboard, so it is always the first
    // case.

    /** The output side of the round trip the device claims.

        The output half of claimedRoundTripSamples, and it floors for the same
        reason: a reported zero means "the driver did not say" rather than
        "instant", and every backend JUCE 9.0.3 ships adds at least one period
        per direction whenever that direction's device exists. The round trip
        floors at two buffers because it is two directions; one direction
        floors at one.

        Not written as half of claimedRoundTripSamples, because it is not:
        max (in + out, 2b) is not max (in, b) + max (out, b), and a MIDI note
        has no claim about the input side to fall back on.
    */
    inline int claimedOutputSamples (const Figures& f) noexcept
    {
        const int reported = std::clamp (f.outputLatency,
                                         -AudioDefaults::maxLatencySamples,
                                         AudioDefaults::maxLatencySamples);

        const int oneBuffer = std::clamp (f.bufferSize, 0, AudioDefaults::maxLatencySamples);

        return std::max (reported, oneBuffer);
    }

    /** How many samples a captured live MIDI event belongs earlier than the
        beat it was written down at.

        Compare captureOffsetSamples, which is the audio answer. Where neither
        figure's floor is binding the two differ by `bufferSize +
        midiInputLatency - inputLatency - trim/2`, and that relationship is
        asserted in the test rather than left to be noticed: the usual way for
        a pair like this to go wrong is for somebody to edit one of them.

        The floors do bind on a driver that answers for one direction or
        neither, and then the relation is false rather than approximate: the
        round trip floors at two buffers where this floors at one, so a silent
        driver makes the two offsets equal. The test pins that case to its own
        literal instead of loosening the check, and the universal form it
        asserts is `midi - audio == block + midiIn - (roundTrip - output) -
        trimShare`.

        THE TRIM IS HALVED, AND THAT IS A CONVENTION

        The trim is what a loopback measurement found the device to be
        misreporting by: measured round trip less claimed round trip, see
        LatencyProbe::trimMsFor. It is an **audio** measurement and the
        residual is unattributed, because a loopback cable cannot say which of
        the two converters the error was in. Only the output half of it sits in
        front of a MIDI note.

        Ardour splits exactly this residual evenly, and does so for both paths:
        engine_dialog.cc:3222-3236 computes one_way = (measured - signal_delay)
        / 2 for audio and writes it into both directions, and :3202-3213 does
        the same for its MIDI loopback. So half is the published convention for
        an unattributed round trip residual, and half is what this uses.

        It is a convention and not a derivation, which is said here rather than
        implied. The only honest way to do better is a second measurement that
        isolates one direction, which for MIDI means a MIDI loopback, which is
        the same unlock Figures::midiInputLatency is waiting on.
    */
    inline int midiCaptureOffsetSamples (const Figures& f) noexcept
    {
        const int output = claimedOutputSamples (f);

        const int engine = std::clamp (f.engineLatency, 0, AudioDefaults::maxLatencySamples);

        // The collector's one block. Latched with everything else, so it is
        // the device's callback size rather than the size of whichever block
        // an event happens to arrive in. A device that varies its block size
        // leaves a residual of the variation, which is smaller than the
        // wake-up jitter the within-block position already carries.
        const int block = std::clamp (f.bufferSize, 0, AudioDefaults::maxLatencySamples);

        // Nothing reports this and it is zero until a MIDI loopback can
        // measure it. Clamped rather than trusted: it reaches here from a
        // settings file.
        const int midiIn = std::clamp (f.midiInputLatency, 0, AudioDefaults::maxLatencySamples);

        int trim = 0;

        // Finite first, for the reason captureOffsetSamples gives: clamping a
        // NaN hands the NaN back and the conversion to int after it is
        // undefined behaviour rather than a wrong answer.
        if (std::isfinite (f.trimMs) && f.sampleRate > 0.0)
            trim = (int) std::lround (0.5 * std::clamp (f.trimMs, minTrimMs, maxTrimMs)
                                      * std::min (f.sampleRate, 768000.0) / 1000.0);

        return std::clamp (output + engine + block + midiIn + trim,
                           -AudioDefaults::maxLatencySamples, AudioDefaults::maxLatencySamples);
    }

    /** Where one captured MIDI event ends up, or that it belongs nowhere. */
    struct MidiPlacement
    {
        bool   keep = false;  /**< false only for an input that is not a number */
        double beat = 0.0;
    };

    /** Moves one captured event back by the offset.

        `rawBeat` is the transport position the event was written down at,
        `offsetBeats` is midiCaptureOffsetSamples in beats, and
        `recordStartBeat` is the transport position recording began at.

        One rule: the event moves back by the offset, and no further back than
        the point recording began. The clamp is the whole of the subtlety, so
        it is worth saying what it is not.

        WHY THE EARLIEST REPRESENTABLE POINT RATHER THAN A DROP

        Ardour drops: disk_writer.cc:701-706 skips an event whose time is below
        _first_recordable_sample, "Event out of range". The first version here
        copied that and it is wrong for this studio, for a reason about this
        studio rather than about Ardour.

        A player pushes the downbeat. That is not an edge case, it is most
        players most of the time, and the correction is of the same order as
        the push: ten to sixty milliseconds. So the first note of a take, the
        one on the count-in's downbeat, lands a hair before the record point
        more often than not. Dropped, it is gone, and
        MainComponent::finishRecording would drop it a second time anyway: it
        rebases every note by the record start and keeps only `n.start >= 0.0`.
        There is no second chance the way there is for audio, where this header
        argues at length that the pre-roll is kept in the file precisely so a
        wrongly set trim cannot take the attack off a take with no way back. A
        MIDI clip has no pre-roll to keep it in.

        So the earliest point the take can represent is where it goes. The cost
        is that notes played inside the first `offsetBeats` of a take pile onto
        its first instant, which for a pushed downbeat is exactly where the
        player meant it; the alternative cost is losing the note.

        WHY THERE IS NO LOOP WRAPPING HERE

        There was, and it was wrong twice over. It wrapped a corrected beat
        back inside the loop range on the argument that a loop is a cycle and
        its start and end are the same musical instant. Both halves failed:

        It broke note pairs. Each event is placed on its own, so a note on
        landing a hair before the loop start wrapped to the top of the cycle
        while its note off, a beat later, did not. The pair then reaches
        MainComponent::collectRecordedMidi out of order and it can only clamp
        the note to a sixty-fourth. An ordinary slightly early note came back
        as a blip at the far end of the pattern, which is worse than the
        lateness being fixed, and the on-screen keyboard hit it every time.

        And the premise was imported rather than read. This studio does not
        fold loop passes into one cycle for MIDI: finishRecording builds a
        single clip starting at the record point and long enough for the whole
        take, so the timeline a recorded MIDI event lives on is continuous and
        not circular. The circular model came from how other studios record
        patterns, which is exactly the move CLAUDE.md warns about, and it was
        caught by an audit rather than by the test, because the test placed
        single events and so blessed the behaviour that corrupted the pairs.
    */
    inline MidiPlacement placeMidiEvent (double rawBeat, double offsetBeats,
                                         double recordStartBeat) noexcept
    {
        const double corrected = rawBeat - offsetBeats;

        // Covers a non-finite rawBeat and a non-finite offset together: IEEE
        // subtraction never makes a finite number out of one that is not, so
        // asking about the result is asking about both operands.
        if (! std::isfinite (corrected))
            return {};

        // A record point that is not a number is a studio that has lost track
        // of when it started. Nothing to clamp against, so the correction
        // stands on its own rather than everything being thrown away.
        if (std::isfinite (recordStartBeat) && corrected < recordStartBeat)
            return { true, recordStartBeat };

        return { true, corrected };
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
