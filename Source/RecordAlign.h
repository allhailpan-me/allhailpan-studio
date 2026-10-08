#pragma once
#include "AudioDefaults.h"
#include <algorithm>
#include <cmath>
#include <vector>

//==============================================================================
/** Where a recorded take belongs on the timeline.

    THE FAULT THIS EXISTS TO FIX

    A player hears the arrangement out of the speakers, plays along with it,
    and their playing arrives back at the studio later than they played it. The
    studio used to write the take down at the transport position of the block
    the audio arrived in, which is not where it was played: it is where it was
    played plus the whole way round the loop through both converters and
    through whatever the mixer is holding back for a lookahead plugin. So every
    overdub landed late, by five milliseconds on a good interface and by
    twenty or more on plain Windows Audio with its 480 sample buffer.

    It is the worst kind of wrong, which is the kind that does not announce
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
    not move the clip earlier. It starts keeping the capture that many samples
    later in the incoming stream and writes it down at the transport position,
    which is the same alignment arrived at from the other end and has two
    advantages: no take ever needs a negative start, and every pass of a loop
    recording comes out stamped with the beat it was actually recorded at, so
    the passes stay aligned with each other inside a take folder.

    What it costs is the first few milliseconds of the capture, which is the
    player's response to something before the record point and therefore
    belongs before the record point. It also means the take now ends that much
    earlier, because the studio stops capturing when the transport stops.
    Ardour buys that back by keeping the capture running past the stop point
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

    /** One pass of the capture that is kept: the half open range of capture
        frames it covers, and the beat its first frame is written down at. */
    struct Pass
    {
        int    firstFrame = 0;
        int    lastFrame  = 0;      // exclusive
        double startBeat  = 0.0;

        int frames() const noexcept { return lastFrame - firstFrame; }
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
        const int roundTrip = AudioDefaults::roundTripSamples (f.inputLatency, f.outputLatency,
                                                               f.bufferSize);

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
        // other direction moves every beat stamp later instead. Exactly one of
        // these is ever non-zero.
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

        int    from     = std::min (frameShift, totalFrames);
        double fromBeat = stamp (rawStartBeat);

        auto emit = [&result] (int first, int last, double beat)
        {
            if (last > first)
                result.push_back ({ first, last, beat });
        };

        for (int i = 0; i < numWraps; ++i)
        {
            const int at = shiftFrame (wraps[i].frame, from);
            emit (from, at, fromBeat);
            from     = at;
            fromBeat = stamp (wraps[i].beat);
        }

        emit (from, totalFrames, fromBeat);

        return result;
    }

    /** Where a frame of the capture ended up, or a negative number when it was
        not kept at all.

        Here rather than in the test so that the property being asserted is
        written down next to the code that has to produce it. Linear, because
        it is only ever walked over by a test.
    */
    inline double placementOf (const std::vector<Pass>& p, int frame, double beatsPerSample) noexcept
    {
        for (const auto& pass : p)
            if (frame >= pass.firstFrame && frame < pass.lastFrame)
                return pass.startBeat + (double) (frame - pass.firstFrame) * beatsPerSample;

        return -1.0;
    }
}
