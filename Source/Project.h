#pragma once
#include <juce_events/juce_events.h>
#include "SampleData.h"
#include "WarpMap.h"
#include "AutomationCurve.h"
#include "CompModel.h"
#include <set>
#include <array>
#include <map>

// ---------------------------------------------------------------------------
// The arrangement. Positions are in beats.
// Audio clip lengths are in seconds of source audio (before stretching);
// MIDI clip lengths are in beats.
// ---------------------------------------------------------------------------

static constexpr int kNumChannels = 16;   // instrument slots
static constexpr int kNumInserts  = 17;   // 0 = master, 1..16 = inserts
static constexpr int kNumFxSlots  = 8;
static constexpr int kNumSends    = 2;   // aux sends per insert
static constexpr int kMaxOutBuses = 16;  // output buses a plugin may be split across

// The tempo a new project starts at. Named because more than one place needs
// to ask whether the tempo is still the one nobody chose: importing a MIDI
// file adopts the file's own tempo only in that case, since overriding a
// tempo the producer set would be worse than ignoring the file's.
static constexpr double kDefaultBpm = 128.0;

// The shortest loop range the transport will take. A sixteenth note is short
// enough for any musical purpose and long enough that a wrap cannot happen
// more than once inside an audio block, which is what keeps the recorder's
// pass splitting honest.
static constexpr double kMinLoopBeats = 0.25;

struct Track
{
    juce::String name;
    bool muted  = false;
    bool armed  = false;
    int  insert = 1;          // mixer insert its audio clips play through
};

struct MidiNote
{
    double start    = 0.0;    // beats from the pattern start
    double length   = 0.25;
    int    note     = 60;
    float  velocity = 0.8f;

    bool operator== (const MidiNote&) const = default;
};

struct AutoPoint
{
    double beat  = 0.0;       // beats from the pattern start
    float  value = 0.0f;      // normalised 0..1

    bool operator== (const AutoPoint&) const = default;
};

struct AutoLane
{
    int          paramIndex = 0;
    juce::String name;
    std::vector<AutoPoint> points;   // kept sorted by beat

    bool operator== (const AutoLane& o) const
    {
        return paramIndex == o.paramIndex && name == o.name && points == o.points;
    }

    float valueAt (double beat) const
    {
        if (points.empty())                return 0.0f;
        if (beat <= points.front().beat)   return points.front().value;
        if (beat >= points.back().beat)    return points.back().value;
        auto it = std::upper_bound (points.begin(), points.end(), beat,
                                    [] (double b, const AutoPoint& p) { return b < p.beat; });
        const auto& b = *it;
        const auto& a = *(it - 1);
        const double t = (beat - a.beat) / std::max (1e-9, b.beat - a.beat);
        return (float) (a.value + (b.value - a.value) * t);
    }

    void sort()
    {
        std::stable_sort (points.begin(), points.end(),
                          [] (const AutoPoint& a, const AutoPoint& b) { return a.beat < b.beat; });
    }
};

//=============================================================================
// Modulators.
//
// A modulator is a shape that runs in time and is wired to any number of
// plugin parameters, each with its own depth. It is the difference between
// drawing a filter sweep by hand every eight bars and saying "this moves".
// Because it drives ordinary parameters, it works on any hosted plugin rather
// than only on built in devices.
//
// Rates are in beats, so everything stays locked to the tempo.
//=============================================================================

enum class ModShape { sine = 0, triangle, saw, rampUp, square, randomStep, sampleHold };

inline const char* modShapeName (ModShape s)
{
    switch (s)
    {
        case ModShape::sine:       return "Sine";
        case ModShape::triangle:   return "Triangle";
        case ModShape::saw:        return "Saw down";
        case ModShape::rampUp:     return "Ramp up";
        case ModShape::square:     return "Square";
        case ModShape::randomStep: return "Random";
        case ModShape::sampleHold: return "Sample and hold";
    }
    return "Sine";
}

// What a moving value is pointed at. One scheme for both of the things that
// move values: a modulator, which generates its own shape, and an automation
// curve, which is drawn. They used to be separate because only instrument
// channel parameters could be reached; now that the mixer can be reached too,
// a second scheme would mean two places to extend every time something else
// becomes automatable, and two places for them to disagree.
//
// The kind itself, and which range a reading means for each kind, live in
// AutomationCurve.h, because that part is arithmetic and arithmetic belongs
// somewhere the tests can reach it.
struct AutoTarget
{
    AutoTargetKind kind = AutoTargetKind::channelParam;

    int channel    = 0;   // channelParam: which instrument slot
    int insert     = 0;   // insert*: which mixer insert, 0 being the master
    int fxSlot     = 0;   // insertFxParam: which of the insert's effect slots
    int paramIndex = 0;   // channelParam and insertFxParam

    // Remembered for display, so a curve still says what it was aimed at when
    // the plugin it pointed into is not on this computer.
    juce::String paramName;

    bool operator== (const AutoTarget&) const = default;

    bool isInsert() const noexcept { return kind != AutoTargetKind::channelParam; }
};

struct ModTarget : AutoTarget
{
    float depth = 0.5f;   // -1..1, added to the parameter's own value

    bool operator== (const ModTarget&) const = default;
};

//=============================================================================
// An automation curve on the playlist.
//
// Automation used to live only inside a MIDI clip, as a lane beside its notes.
// That ties a movement to a pattern, which is the wrong place for most of
// them: a filter opening over an eight bar section is a property of the
// section, not of whichever part happens to be playing through it, and a
// movement on a mixer insert has no pattern to live in at all.
//
// So a curve is a clip in its own right. It sits on a playlist track and is
// moved, copied, trimmed and deleted with the same gestures as any other clip,
// which means the arrangement is edited as an arrangement rather than through
// a separate automation mode. The in-clip lanes still work exactly as they
// did, because a movement that genuinely belongs to a part should travel with
// the part when it is dragged elsewhere.
//=============================================================================

struct AutoCurve
{
    AutoTarget target;
    std::vector<AutoCurvePoint> points;   // kept normalised: see AutomationCurve.h

    bool operator== (const AutoCurve&) const = default;

    /** Puts the points back into the form every lookup assumes. Call after
        anything that moves, adds or removes one, including loading. */
    void tidy() { normaliseAutoCurve (points); }

    double lastBeat() const noexcept { return autoCurveLastBeat (points); }

    double valueAt (double beatsIn) const noexcept
    {
        return autoCurveValueAt (points, beatsIn);
    }
};

struct Modulator
{
    juce::String name  = "LFO";
    ModShape shape     = ModShape::sine;
    double   rateBeats = 4.0;     // length of one cycle
    double   phase     = 0.0;     // 0..1 offset into the cycle
    bool     bipolar   = true;    // swings either side of the value, or only up
    bool     enabled   = true;
    std::vector<ModTarget> targets;

    bool operator== (const Modulator&) const = default;

    /** The modulator's output at a point in the song, in -1..1 (bipolar) or
        0..1 (unipolar). */
    float valueAt (double beat) const
    {
        const double cycles = beat / std::max (1.0e-6, rateBeats) + phase;
        const double t = cycles - std::floor (cycles);

        double v = 0.0;
        switch (shape)
        {
            case ModShape::sine:     v = std::sin (t * 6.283185307179586); break;
            case ModShape::triangle: v = 4.0 * std::abs (t - 0.5) - 1.0;   break;
            case ModShape::saw:      v = 1.0 - 2.0 * t;                    break;
            case ModShape::rampUp:   v = 2.0 * t - 1.0;                    break;
            case ModShape::square:   v = t < 0.5 ? 1.0 : -1.0;             break;

            case ModShape::randomStep:
            case ModShape::sampleHold:
            {
                // Hashing the cycle number gives a value that is random but
                // repeatable, so a project sounds the same on every play.
                const auto step = (juce::uint32) (juce::int64) std::floor (cycles);
                juce::uint32 h = step * 2654435761u;
                h ^= h >> 15;
                h *= 2246822519u;
                h ^= h >> 13;
                v = (h / 4294967296.0) * 2.0 - 1.0;
                break;
            }
        }

        return (float) (bipolar ? v : (v + 1.0) * 0.5);
    }
};

//=============================================================================
// Groove.
//
// Modelled on what hardware samplers and the better studios actually mean by
// these words, rather than on a single "swing" knob.
//
// Swing is the position of the offbeat within its pair, as a percentage: at
// 50 the pair is even and the feel is straight, at 66.67 the offbeat lands on
// the third triplet, which is a full shuffle. That is the definition the MPC
// established and the one other studios and drummers mean by a swing number,
// so a pattern moved between them keeps its feel. Most records sit between
// 52 and 62.
//
// Swing alone is not groove, though. Ableton's grooves carry a resolution the
// groove is measured at, an amount to apply, a velocity effect and a random
// amount, and that combination is what turns a grid into a performance. The
// same four are here.
//
// Nothing below changes the stored notes. Groove is applied when the
// arrangement is handed to the engine, so it can be dialled while playing and
// turned off without having lost anything.
//=============================================================================

struct Groove
{
    // The note value the groove is measured against. Pairs are formed at this
    // resolution, so a 1/16 groove swings sixteenths and leaves eighths alone.
    enum class Base { eighth = 0, sixteenth, thirtySecond };

    Base   base     = Base::sixteenth;
    double swing    = 0.5;    // 0.5 straight, 0.6667 a triplet shuffle
    double amount   = 1.0;    // how much of the timing shift to apply
    double velocity = 0.0;    // how much offbeats are softened, 0 to 1
    double random   = 0.0;    // humanising, in fractions of a step
    bool   enabled  = false;

    bool operator== (const Groove&) const = default;

    /** Beats per step at this groove's resolution. */
    double stepBeats() const noexcept
    {
        switch (base)
        {
            case Base::eighth:       return 0.5;
            case Base::sixteenth:    return 0.25;
            case Base::thirtySecond: return 0.125;
        }
        return 0.25;
    }

    /** Where a note sits once the groove is applied.

        Steps are numbered from the clip's own origin, and every second one is
        an offbeat. An offbeat's position inside its pair is the swing
        fraction, so the shift is the difference between that and the even
        halfway point.

        The random part is derived from the note's own position rather than
        drawn fresh, so a pattern plays the same way twice and an export
        matches what was heard. A groove that wandered on every pass would be
        unusable.
    */
    double place (double beatFromOrigin, int note) const
    {
        if (! enabled)
            return beatFromOrigin;

        const double step = stepBeats();
        const double index = beatFromOrigin / step;
        const auto   whole = (juce::int64) std::llround (index);

        // Only notes sitting on the grid are moved. Anything already played
        // off the grid is left where the performance put it, which is what
        // Base means in the studios this follows.
        if (std::abs (index - (double) whole) > 0.02)
            return beatFromOrigin;

        double shifted = beatFromOrigin;

        if ((whole & 1) != 0)
        {
            // The pair spans two steps, so the straight offbeat is at one half
            // of it and the swung offbeat is at the swing fraction of it.
            const double pair = step * 2.0;
            shifted += (swing * pair - step) * amount;
        }

        if (random > 0.0)
        {
            juce::uint32 h = (juce::uint32) (whole * 2654435761u) ^ (juce::uint32) (note * 40503u);
            h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
            const double jitter = (h / 4294967296.0) * 2.0 - 1.0;
            shifted += jitter * random * step * 0.25;
        }

        return std::max (0.0, shifted);
    }

    /** Offbeats are played a little softer, which is most of what makes a
        swung pattern sound played rather than programmed. */
    float shade (double beatFromOrigin, float noteVelocity) const
    {
        if (! enabled || velocity <= 0.0)
            return noteVelocity;

        const double step = stepBeats();
        const double index = beatFromOrigin / step;
        const auto   whole = (juce::int64) std::llround (index);

        if (std::abs (index - (double) whole) > 0.02 || (whole & 1) == 0)
            return noteVelocity;

        return (float) juce::jlimit (0.02, 1.0, noteVelocity * (1.0 - velocity * 0.4));
    }
};

struct MidiPattern
{
    std::vector<MidiNote> notes;
    std::vector<AutoLane> lanes;

    bool operator== (const MidiPattern& o) const { return notes == o.notes && lanes == o.lanes; }

    AutoLane& laneFor (int paramIndex, const juce::String& name)
    {
        for (auto& l : lanes)
            if (l.paramIndex == paramIndex)
                return l;
        lanes.push_back ({ paramIndex, name, {} });
        return lanes.back();
    }

    double lastBeat() const
    {
        double end = 0.0;
        for (auto& n : notes) end = std::max (end, n.start + n.length);
        for (auto& l : lanes) if (! l.points.empty()) end = std::max (end, l.points.back().beat);
        return end;
    }
};

struct ChannelInfo
{
    juce::String name;        // plugin name, or empty
    int insert = 1;

    // Multi output plugins. A drum machine like Microtonic puts each of its
    // eight sounds on its own output, and splitting them gives each drum its
    // own mixer strip, with its own effects and fader, the way a hardware
    // machine's individual outs would be patched.
    bool splitBuses = false;
    std::array<int, kMaxOutBuses> busInsert {};   // 0 means follow the channel

    // Channel rack. rackNote is what a step plays, which matters for drum
    // plugins that put every sound on a different key. rackTrack is the
    // playlist track this channel's rack clips live on, assigned the first
    // time a step is drawn so rows stay put.
    int rackNote  = 60;
    int rackTrack = -1;

    // A plugin the project uses but this computer doesn't have. Kept so that
    // saving again doesn't throw its settings away.
    std::shared_ptr<juce::XmlElement> missingPlugin;
};

/** What a target is called, for a clip label and for the mixer's own display.
    Insert zero is the master, which is worth naming as such rather than as
    "insert 0", since that is not what it is called anywhere else. */
inline juce::String autoTargetName (const AutoTarget& t, const std::vector<ChannelInfo>& channels)
{
    const auto param = t.paramName.isNotEmpty() ? t.paramName
                                                : "Parameter " + juce::String (t.paramIndex + 1);
    const auto insertName = t.insert == 0 ? juce::String ("Master")
                                          : "Insert " + juce::String (t.insert);

    switch (t.kind)
    {
        case AutoTargetKind::insertVolume:  return insertName + "  Volume";
        case AutoTargetKind::insertPan:     return insertName + "  Pan";
        case AutoTargetKind::insertFxParam: return insertName + "  FX " + juce::String (t.fxSlot + 1) + "  " + param;

        case AutoTargetKind::channelParam:
        {
            const int c = juce::jlimit (0, kNumChannels - 1, t.channel);
            const auto& name = channels[(size_t) c].name;
            return juce::String (c + 1) + "  " + (name.isNotEmpty() ? name : juce::String ("Empty channel"))
                     + "  " + param;
        }
    }
    return param;
}

enum class ClipType { audio, midi, automation };

//=============================================================================
// Take folders, and the comp over them.
//
// Recording a loop over and over is how a vocal or a solo actually gets made:
// eight bars round and round, ten passes, and then one performance built from
// the best moments of each. Keeping those passes as ten clips stacked on one
// track loses which of them belong together, and keeping only the last one
// throws the morning away. So the passes live in a take folder: one clip in
// the arrangement holding several alternatives, with a comp saying which of
// them is heard where. The takes are never edited by comping, so a choice can
// be remade at any point, including after the project has been closed and
// reopened.
//
// A take is described in seconds throughout, as the comp over it is: `start`
// is seconds into the folder where this pass begins, and `offset` and
// `length` say which seconds of its own source file it plays, exactly as they
// do for an ordinary audio clip. Loop recording gives every pass the same
// start and very nearly the same length, but a pass that was armed late, or
// that dropped samples, does not cover the whole folder, and the comp has to
// cope with that rather than read off the end of it.
//=============================================================================

struct Take
{
    juce::String name;
    std::shared_ptr<SampleData> sample;

    double start  = 0.0;   // seconds into the folder where this pass begins
    double offset = 0.0;   // seconds into the source where the take begins
    double length = 0.0;   // seconds of source the take runs for

    bool operator== (const Take& o) const
    {
        return name == o.name && sample == o.sample
            && start == o.start && offset == o.offset && length == o.length;
    }

    /** Where this take sits inside the folder, which is what the comp needs to
        know to keep from reading past the end of it. */
    CompTakeSpan span() const noexcept { return { start, length }; }
};

struct TakeFolder
{
    std::vector<Take>        takes;
    std::vector<CompSegment> comp;   // kept normalised: see CompModel.h

    // The width of the crossfade at a comp join. There is no standard for
    // this: the requirement is only that it be long enough to hide the slope
    // discontinuity between two different performances and short enough not to
    // smear a consonant, and ten milliseconds sits comfortably between those.
    // It is per folder rather than global because a drum comp wants it shorter
    // than a string pad does.
    double crossfadeMs = 10.0;

    bool operator== (const TakeFolder& o) const
    {
        return takes == o.takes && comp == o.comp && crossfadeMs == o.crossfadeMs;
    }

    int size() const noexcept { return (int) takes.size(); }

    const Take* take (int index) const noexcept
    {
        return juce::isPositiveAndBelow (index, (int) takes.size()) ? &takes[(size_t) index] : nullptr;
    }

    /** The longest any take runs, in seconds of source. The folder clip is as
        long as its longest pass, so a take that overran is not cut off by the
        folder having been sized to a shorter one. */
    double longestSeconds() const noexcept
    {
        double longest = 0.0;
        for (const auto& t : takes)
            longest = std::max (longest, t.length);
        return longest;
    }

    std::vector<CompTakeSpan> spans() const
    {
        std::vector<CompTakeSpan> out;
        out.reserve (takes.size());
        for (const auto& t : takes)
            out.push_back (t.span());
        return out;
    }

    void tidy (double folderSeconds) { normaliseCompSegments (comp, size(), folderSeconds); }
};

struct Clip
{
    int      id   = 0;
    ClipType type = ClipType::audio;

    std::shared_ptr<SampleData>  sample;    // audio
    std::shared_ptr<MidiPattern> pattern;   // midi
    std::shared_ptr<AutoCurve>   curve;     // automation
    int channel = 0;                        // midi: which instrument slot

    // Several takes of this same stretch, with a comp over them. An audio clip
    // with a folder reads its takes through the comp instead of reading
    // `sample`, which is then null. It is an audio clip in every other
    // respect, which is the point: a take folder is dragged, trimmed, muted,
    // gained and deleted by the playlist's existing code rather than by a
    // parallel set of operations that could drift from it.
    std::shared_ptr<TakeFolder> folder;

    int    track   = 0;
    double start   = 0.0;    // beats
    double offset  = 0.0;    // audio: seconds into source   midi: beats into pattern
    double length  = 0.0;    // audio: seconds of source     midi: beats
    double stretch = 1.0;    // audio: output time / source time
    double pitch   = 0.0;    // audio: semitones

    // Tempo matching. sourceBpm is the tempo the audio was recorded at; with
    // followTempo set, its stretch is kept at sourceBpm/projectBpm, so the clip
    // stays on the grid at any project tempo. Rubber Band stretches time
    // without touching pitch, so a vocal matched this way stays in key.
    double sourceBpm   = 0.0;   // 0 = unknown
    bool   followTempo = false;

    // Warp markers, for a performance that drifts within the take rather than
    // sitting at one steady tempo. Each pins a point in the source audio to a
    // beat after the clip's start, and the audio between two of them is
    // stretched to fit. Empty means the single `stretch` ratio governs the whole
    // clip, which is the behaviour every project had before warping existed, so
    // old projects load unchanged. Kept normalised at all times: sorted, and
    // strictly increasing in both coordinates. See WarpMap.h.
    std::vector<WarpMarker> warp;

    float  gainDb  = 0.0f;
    bool   muted   = false;

    bool   isAudio() const noexcept      { return type == ClipType::audio; }
    bool   isTakeFolder() const noexcept { return type == ClipType::audio && folder != nullptr && ! folder->takes.empty(); }
    bool   isMidi() const noexcept       { return type == ClipType::midi; }
    bool   isAutomation() const noexcept { return type == ClipType::automation; }
    bool   isWarped() const noexcept     { return isAudio() && ! warp.empty(); }

    // Source seconds per beat for an unwarped clip, and for the stretch of a
    // warped one that runs before its first marker.
    double fallbackSlope (double bpm) const noexcept { return warpFallbackSlope (bpm, stretch); }

    /** Source seconds read at a beat measured from the clip's start. */
    double sourceAtBeat (double bpm, double beatsIn) const noexcept
    {
        return warpSourceAtBeat (warp, offset, fallbackSlope (bpm), beatsIn);
    }

    /** The beat, measured from the clip's start, at which a source position is
        read. The inverse of the above, which is what the clip's own length in
        beats is: how far into the arrangement its last source sample lands.
    */
    double beatAtSource (double bpm, double sourceSeconds) const noexcept
    {
        return warpBeatAtSource (warp, offset, fallbackSlope (bpm), sourceSeconds);
    }

    /** The rate the clip reads at around a beat, in source seconds per beat. */
    double slopeAtBeat (double bpm, double beatsIn) const noexcept
    {
        return warpSlopeAtBeat (warp, offset, fallbackSlope (bpm), beatsIn);
    }

    double lengthBeats (double bpm) const noexcept
    {
        if (! isAudio())
            return length;
        if (warp.empty())
            return length * stretch * bpm / 60.0;   // unchanged for every unwarped clip
        return beatAtSource (bpm, offset + length);
    }
    double endBeat (double bpm) const noexcept { return start + lengthBeats (bpm); }

    /** Drops markers that the clip's current edges cannot honour, and puts the
        rest back in order. Call after anything that moves `offset`. */
    void tidyWarp() { normaliseWarpMarkers (warp, offset); }

    /** The stretches of this folder the comp actually plays, in seconds into
        the folder. Empty for a clip that is not a take folder. No tempo comes
        into it, which is the point: see CompModel.h. */
    std::vector<CompSpan> compSpans() const
    {
        if (! isTakeFolder())
            return {};
        return buildCompSpans (folder->comp, folder->spans(), length,
                               folder->crossfadeMs * 0.001);
    }

    // Copies share nothing editable with the original
    Clip deepCopy() const
    {
        Clip c = *this;
        if (pattern != nullptr)
            c.pattern = std::make_shared<MidiPattern> (*pattern);
        if (curve != nullptr)
            c.curve = std::make_shared<AutoCurve> (*curve);
        if (folder != nullptr)
            c.folder = std::make_shared<TakeFolder> (*folder);
        return c;
    }

    juce::String displayName (const std::vector<ChannelInfo>& channels) const
    {
        if (isTakeFolder())
            return juce::String (folder->size()) + " takes";
        if (isAudio())
            return sample != nullptr ? sample->name : juce::String ("Missing audio");
        if (isAutomation())
            return curve != nullptr ? autoTargetName (curve->target, channels)
                                    : juce::String ("Automation");
        const auto& ch = channels[(size_t) juce::jlimit (0, kNumChannels - 1, channel)];
        return juce::String (channel + 1) + "  " + (ch.name.isNotEmpty() ? ch.name : juce::String ("Empty channel"));
    }
};

// One step of undo history. Patterns inside are never edited in place;
// unchanged ones are shared between steps to keep memory small.
struct ProjectState
{
    std::vector<Track> tracks;
    std::vector<Clip>  clips;
    std::set<int>      selection;
};

class Project : public juce::ChangeBroadcaster
{
public:
    static constexpr int numTracks = 24;

    Project()
    {
        for (int i = 0; i < numTracks; ++i)
            tracks.push_back ({ "Track " + juce::String (i + 1), false, false, (i % 16) + 1 });
        channels.resize (kNumChannels);
        for (int i = 0; i < kNumChannels; ++i)
            channels[(size_t) i].insert = i + 1;
        committed = capture (nullptr);
    }

    std::vector<Track>       tracks;
    std::vector<ChannelInfo> channels;
    std::vector<Clip>        clips;
    std::set<int>            selection;
    std::vector<Modulator>   modulators;
    Groove                   groove;
    double bpm = kDefaultBpm;

    // ---- loop range ----
    // The span the transport repeats over, set by dragging in the playlist
    // ruler. An empty range means no range, and playback then loops at the end
    // of the last clip exactly as it always has, which is a different thing
    // and worth keeping: it is what you want when auditioning an arrangement
    // and never what you want when recording takes.
    //
    // This is not part of the undo history, for the same reason arming is not:
    // it is where you are working, not what the arrangement is, and having
    // Ctrl+Z move the loop instead of undoing the edit you just made would be
    // maddening. It is saved with the project, because where you were working
    // is worth coming back to.
    double loopStart = 0.0, loopEnd = 0.0;

    bool hasLoopRange() const noexcept
    {
        return loopEnd - loopStart >= kMinLoopBeats
            && std::isfinite (loopStart) && std::isfinite (loopEnd);
    }

    /** Sets the range, or clears it if the drag was too short to be one.
        Clearing on a short drag rather than clamping up to the minimum is
        what makes a click in the ruler a way to get rid of the loop. */
    void setLoopRange (double from, double to)
    {
        if (! std::isfinite (from) || ! std::isfinite (to))
            return;
        if (to < from)
            std::swap (from, to);

        from = std::max (0.0, from);
        to   = std::max (from, to);

        if (to - from < kMinLoopBeats)
        {
            loopStart = loopEnd = 0.0;
        }
        else
        {
            loopStart = from;
            loopEnd   = to;
        }
        changed();
    }

    void clearLoopRange()
    {
        loopStart = loopEnd = 0.0;
        changed();
    }

    double loopLengthBeats() const noexcept { return hasLoopRange() ? loopEnd - loopStart : 0.0; }

    // ---- channel rack ----
    // The rack edits a region of the arrangement rather than owning its own
    // kind of data: every step is a real note in a real MIDI clip, so the same
    // bar can be opened in the piano roll and edited there.
    double rackStart     = 0.0;   // beat the pattern begins on
    int    rackBars      = 1;
    int    rackStepsPerBar = 16;

    double rackStepBeats() const noexcept { return 4.0 / std::max (1, rackStepsPerBar); }
    int    rackStepCount() const noexcept { return std::max (1, rackBars) * std::max (1, rackStepsPerBar); }
    double rackLengthBeats() const noexcept { return std::max (1, rackBars) * 4.0; }

    // Effects that couldn't be loaded, by insert * kNumFxSlots + slot
    std::map<int, std::shared_ptr<juce::XmlElement>> missingFx;

    // Back to an empty project (tracks, channels, clips). History is cleared.
    void clearAll()
    {
        clips.clear();
        selection.clear();
        missingFx.clear();
        tracks.clear();
        for (int i = 0; i < numTracks; ++i)
            tracks.push_back ({ "Track " + juce::String (i + 1), false, false, (i % 16) + 1 });
        channels.assign ((size_t) kNumChannels, {});
        for (int i = 0; i < kNumChannels; ++i)
            channels[(size_t) i].insert = i + 1;

        modulators.clear();
        groove = {};

        loopStart = loopEnd = 0.0;

        rackStart       = 0.0;
        rackBars        = 1;
        rackStepsPerBar = 16;

        resetHistory();
        changed();
    }

    // Makes the current state the start of history (after loading)
    void resetHistory()
    {
        undoStack.clear();
        redoStack.clear();
        committed = capture (nullptr);
    }

    int addClip (Clip c)
    {
        c.id = nextId++;
        clips.push_back (std::move (c));
        changed();
        return clips.back().id;
    }

    Clip* find (int id)
    {
        for (auto& c : clips)
            if (c.id == id)
                return &c;
        return nullptr;
    }

    // ---- channel rack ----------------------------------------------------
    // Steps are stored as ordinary notes in an ordinary MIDI clip, so nothing
    // here is a second kind of arrangement data. The rack only ever touches a
    // clip whose channel, start and length match the pattern exactly, which
    // keeps it from disturbing anything else on the playlist.

    Clip* rackClip (int channel)
    {
        const double len = rackLengthBeats();
        for (auto& c : clips)
            if (! c.isAudio() && c.channel == channel
                && std::abs (c.start - rackStart) < 1.0e-6
                && std::abs (c.length - len) < 1.0e-6)
                return &c;
        return nullptr;
    }

    /** The track this channel's rack clips sit on, claiming a free one the
        first time it is needed so rows do not wander between patterns. */
    int rackTrackFor (int channel)
    {
        auto& info = channels[(size_t) channel];

        if (info.rackTrack >= 0 && info.rackTrack < (int) tracks.size())
            return info.rackTrack;

        const double from = rackStart;
        const double to   = rackStart + rackLengthBeats();

        std::set<int> taken;
        for (auto& c : channels)
            if (c.rackTrack >= 0)
                taken.insert (c.rackTrack);

        for (int t = 0; t < (int) tracks.size(); ++t)
        {
            if (taken.count (t))
                continue;

            bool occupied = false;
            for (auto& c : clips)
                if (c.track == t && c.start < to && c.endBeat (bpm) > from)
                {
                    occupied = true;
                    break;
                }

            if (! occupied)
            {
                info.rackTrack = t;
                return t;
            }
        }

        info.rackTrack = juce::jlimit (0, (int) tracks.size() - 1, channel);
        return info.rackTrack;
    }

    Clip* rackClipFor (int channel)
    {
        if (auto* existing = rackClip (channel))
            return existing;

        Clip c;
        c.type    = ClipType::midi;
        c.channel = channel;
        c.track   = rackTrackFor (channel);
        c.start   = rackStart;
        c.length  = rackLengthBeats();
        c.pattern = std::make_shared<MidiPattern>();

        return find (addClip (std::move (c)));
    }

    /** The note a step would occupy, if there is one. */
    MidiNote* rackNoteAt (int channel, int step)
    {
        auto* clip = rackClip (channel);
        if (clip == nullptr || clip->pattern == nullptr)
            return nullptr;

        const double stepBeats = rackStepBeats();
        const double at    = step * stepBeats;
        const double slack = stepBeats * 0.5;
        const int    pitch = channels[(size_t) channel].rackNote;

        for (auto& n : clip->pattern->notes)
            if (n.note == pitch && std::abs (n.start - at) < slack)
                return &n;

        return nullptr;
    }

    bool rackStepOn (int channel, int step) { return rackNoteAt (channel, step) != nullptr; }

    void setRackStep (int channel, int step, bool on, float velocity = 0.8f)
    {
        if (on)
        {
            if (auto* existing = rackNoteAt (channel, step))
            {
                existing->velocity = velocity;
                changed();
                return;
            }

            auto* clip = rackClipFor (channel);
            if (clip == nullptr || clip->pattern == nullptr)
                return;

            const double stepBeats = rackStepBeats();
            MidiNote n;
            n.start    = step * stepBeats;
            n.length   = stepBeats * 0.9;      // a touch short, so repeats retrigger
            n.note     = channels[(size_t) channel].rackNote;
            n.velocity = velocity;
            clip->pattern->notes.push_back (n);
        }
        else
        {
            auto* clip = rackClip (channel);
            if (clip == nullptr || clip->pattern == nullptr)
                return;

            const double stepBeats = rackStepBeats();
            const double at    = step * stepBeats;
            const double slack = stepBeats * 0.5;
            const int    pitch = channels[(size_t) channel].rackNote;
            auto& notes = clip->pattern->notes;

            notes.erase (std::remove_if (notes.begin(), notes.end(),
                                         [&] (const MidiNote& n)
                                         {
                                             return n.note == pitch && std::abs (n.start - at) < slack;
                                         }),
                         notes.end());
        }

        changed();
    }

    void clearRackRow (int channel)
    {
        if (auto* clip = rackClip (channel))
            if (clip->pattern != nullptr)
            {
                clip->pattern->notes.clear();
                changed();
            }
    }

    /** Moves a channel's existing rack notes to a new key, so picking a
        different drum does not appear to wipe the row. */
    // ---- automation clips -------------------------------------------------
    //
    // Everything here goes through find() and leaves the clip list alone
    // otherwise, so an automation clip is moved, trimmed, copied and deleted
    // by the playlist's existing code rather than by a parallel set of
    // operations that could drift from it.

    /** The curve a clip holds, or null if it does not hold one. */
    AutoCurve* curveFor (int id)
    {
        auto* c = find (id);
        return (c != nullptr && c->isAutomation()) ? c->curve.get() : nullptr;
    }

    /** Puts a new automation clip on a track, aimed at a target, holding a
        single point at `startValue` so it reads as a flat offset until
        something is drawn on it. Starting from where the control already sits
        rather than from zero is what keeps drawing a curve on a balanced mix
        from throwing the balance away.

        `startValue` is in the curve's own 0..1, so callers go through
        autoCurveValueFor to convert whatever the control reads.
    */
    int addAutomationClip (const AutoTarget& target, int track, double start,
                           double lengthBeats, double startValue)
    {
        Clip c;
        c.type   = ClipType::automation;
        c.track  = juce::jlimit (0, (int) tracks.size() - 1, track);
        c.start  = std::max (0.0, start);
        c.length = std::max (0.25, lengthBeats);
        c.curve  = std::make_shared<AutoCurve>();
        c.curve->target = target;
        c.curve->points.push_back ({ 0.0, std::clamp (startValue, 0.0, 1.0), 0.0 });

        return addClip (std::move (c));
    }

    // ---- take folders and comping ----------------------------------------
    //
    // As with automation clips, everything here goes through find() and leaves
    // the clip list alone otherwise, so a take folder is moved, trimmed, muted
    // and deleted by the playlist's existing code rather than by a second set
    // of operations that could drift from it.

    /** The folder a clip holds, or null if it does not hold one. */
    TakeFolder* folderFor (int id)
    {
        auto* c = find (id);
        return (c != nullptr && c->isTakeFolder()) ? c->folder.get() : nullptr;
    }

    /** An existing take folder covering the same stretch of the same track, so
        that recording a second round of passes over the same loop joins the
        folder already there instead of burying it. */
    Clip* takeFolderAt (int track, double startBeat, double lengthSeconds)
    {
        for (auto& c : clips)
            if (c.isTakeFolder() && c.track == track
                && std::abs (c.start - startBeat) < 1.0e-6
                && std::abs (c.length - lengthSeconds) < 1.0e-3)
                return &c;
        return nullptr;
    }

    /** Sizes a folder clip to its longest take and puts its comp back into the
        form every lookup assumes. Call after adding or removing a take. */
    void tidyFolder (Clip& c)
    {
        if (c.folder == nullptr)
            return;

        // The folder is as long as its longest pass. Sizing it to the shortest
        // would cut the end off a take that ran on, and sizing it to the first
        // would mean the order the passes arrived in decided the length.
        c.length = c.folder->longestSeconds();
        c.offset = 0.0;

        // A folder reads its takes directly, one short read per comped
        // stretch, so the things that put something between a clip and its
        // audio do not apply to it: warp markers describe one piece of audio
        // and a folder holds several, and stretching would need a stretched
        // copy of every pass rather than of one sample. Both are turned off
        // here rather than guarded against in the renderer.
        c.warp.clear();
        c.stretch     = 1.0;
        c.pitch       = 0.0;
        c.followTempo = false;

        c.folder->tidy (c.length);
    }

    /** Puts a folder of takes on a track. The last pass is heard to begin
        with, because it is the one just recorded and the one the producer is
        waiting to hear; every earlier pass is still there to comp from.
    */
    int addTakeFolder (int track, double startBeat, std::vector<Take> takes)
    {
        Clip c;
        c.type   = ClipType::audio;
        c.track  = juce::jlimit (0, (int) tracks.size() - 1, track);
        c.start  = std::max (0.0, startBeat);
        c.folder = std::make_shared<TakeFolder>();
        c.folder->takes = std::move (takes);
        c.folder->comp.push_back ({ 0.0, std::max (0, c.folder->size() - 1) });
        tidyFolder (c);

        return addClip (std::move (c));
    }

    /** Appends a pass to a folder already on the playlist, and makes it the
        one heard, which is what recording another take means. */
    int addTake (int id, Take take)
    {
        auto* c = find (id);
        if (c == nullptr || ! c->isAudio())
            return -1;

        if (c->folder == nullptr)
            c->folder = std::make_shared<TakeFolder>();
        if ((size_t) c->folder->size() >= kMaxCompTakes)
            return -1;

        c->folder->takes.push_back (std::move (take));
        const int index = c->folder->size() - 1;
        tidyFolder (*c);
        setCompRegion (c->folder->comp, 0.0, c->length, index, c->folder->size(), c->length);
        changed();
        return index;
    }

    /** Throws a pass away. The comp is renumbered with it, so the stretches
        that used a surviving take still use that same take rather than
        whichever one slid into its index. */
    void removeTake (int id, int index)
    {
        auto* c = find (id);
        if (c == nullptr || c->folder == nullptr
            || ! juce::isPositiveAndBelow (index, c->folder->size()))
            return;

        c->folder->takes.erase (c->folder->takes.begin() + index);
        tidyFolder (*c);
        compTakeRemoved (c->folder->comp, index, c->folder->size(), c->length);

        // A folder with nothing left in it is not a clip any more.
        if (c->folder->takes.empty())
        {
            const int gone = c->id;
            c->folder.reset();
            selection.erase (gone);
            clips.erase (std::remove_if (clips.begin(), clips.end(),
                                         [gone] (const Clip& x) { return x.id == gone; }),
                         clips.end());
        }

        changed();
    }

    /** The comping gesture: this stretch of the folder comes from this pass.
        Positions are seconds into the folder, which is what the comp is kept
        in. One call is one finished swipe, so it is one undo step. */
    void setCompTake (int id, double fromSeconds, double toSeconds, int take)
    {
        auto* c = find (id);
        if (c == nullptr || ! c->isTakeFolder())
            return;

        setCompRegion (c->folder->comp, fromSeconds, toSeconds, take,
                       c->folder->size(), c->length);
        changed();
    }

    void setCompCrossfadeMs (int id, double ms)
    {
        if (auto* f = folderFor (id); f != nullptr && std::isfinite (ms))
        {
            f->crossfadeMs = std::clamp (ms, 0.0, 250.0);
            changed();
        }
    }

    /** An automation clip aimed at this target that overlaps a span, if there
        is one.

        Two curves aiming at one control at the same time is a mix that cannot
        be reasoned about, and the one underneath would be invisible, so a
        caller about to put a curve somewhere asks this first. Two curves on
        one control at different points in the arrangement are ordinary and
        wanted, which is why this asks about a span rather than about the
        target alone.
    */
    Clip* automationClipFor (const AutoTarget& target, double fromBeat, double toBeat)
    {
        for (auto& c : clips)
        {
            if (! c.isAutomation() || c.curve == nullptr || ! (c.curve->target == target))
                continue;
            if (c.start < toBeat && c.start + c.length > fromBeat)
                return &c;
        }
        return nullptr;
    }

    /** Adds a point, or moves the one already at that beat. Returns its index,
        or -1 if there was no curve to add it to.

        Points are addressed by index rather than by identity, and the index is
        only valid until the next edit, because normalising re-sorts the list.
        That is deliberate: a drag re-reads the index it is given back each
        time it moves a point, so a point dragged past its neighbour swaps with
        it and keeps being dragged rather than being left behind.
    */
    int setCurvePoint (int id, double beatsIn, double value, double bend)
    {
        auto* curve = curveFor (id);
        if (curve == nullptr || ! std::isfinite (beatsIn))
            return -1;

        const AutoCurvePoint wanted { beatsIn, std::clamp (value, 0.0, 1.0),
                                      std::clamp (bend, -1.0, 1.0) };
        curve->points.push_back (wanted);
        curve->tidy();
        changed();

        for (size_t i = 0; i < curve->points.size(); ++i)
            if (curve->points[i] == wanted)
                return (int) i;

        return -1;
    }

    /** Moves an existing point. Returns where it ended up, which may not be
        the index it started at once the list is back in order. */
    int moveCurvePoint (int id, int index, double beatsIn, double value)
    {
        auto* curve = curveFor (id);
        if (curve == nullptr || ! juce::isPositiveAndBelow (index, (int) curve->points.size())
            || ! std::isfinite (beatsIn))
            return index;

        auto moved = curve->points[(size_t) index];
        moved.beat  = beatsIn;
        moved.value = std::clamp (value, 0.0, 1.0);

        curve->points.erase (curve->points.begin() + index);
        curve->points.push_back (moved);
        curve->tidy();
        changed();

        for (size_t i = 0; i < curve->points.size(); ++i)
            if (curve->points[i] == moved)
                return (int) i;

        return index;
    }

    /** Bends the segment leaving a point. The last point's bend shapes
        nothing, which is checked here rather than left to the caller so that
        dragging the handle off the end of a curve does nothing rather than
        something invisible. */
    void setCurveBend (int id, int index, double bend)
    {
        auto* curve = curveFor (id);
        if (curve == nullptr || ! juce::isPositiveAndBelow (index, (int) curve->points.size()))
            return;
        if ((size_t) index + 1 >= curve->points.size())
            return;

        curve->points[(size_t) index].bend = std::clamp (bend, -1.0, 1.0);
        changed();
    }

    /** Removes a point, except the last one standing: a curve with no points
        at all would read as nothing automated, which is indistinguishable from
        the clip not being there and leaves the producer with a clip they
        cannot see the effect of. Deleting the clip is how to mean that. */
    void removeCurvePoint (int id, int index)
    {
        auto* curve = curveFor (id);
        if (curve == nullptr || curve->points.size() <= 1
            || ! juce::isPositiveAndBelow (index, (int) curve->points.size()))
            return;

        curve->points.erase (curve->points.begin() + index);
        changed();
    }

    // ---- modulators ----

    Modulator* modulator (int index)
    {
        return juce::isPositiveAndBelow (index, (int) modulators.size())
                 ? &modulators[(size_t) index] : nullptr;
    }

    int addModulator()
    {
        Modulator m;
        m.name = "LFO " + juce::String ((int) modulators.size() + 1);
        m.rateBeats = 4.0;
        modulators.push_back (std::move (m));
        changed();
        return (int) modulators.size() - 1;
    }

    void removeModulator (int index)
    {
        if (! juce::isPositiveAndBelow (index, (int) modulators.size()))
            return;
        modulators.erase (modulators.begin() + index);
        changed();
    }

    /** Wires a modulator to a parameter, replacing any existing wiring between
        the same pair rather than stacking duplicates. */
    void assignModulation (int index, int channel, int paramIndex,
                           const juce::String& paramName, float depth)
    {
        auto* m = modulator (index);
        if (m == nullptr)
            return;

        for (auto& t : m->targets)
            if (t.kind == AutoTargetKind::channelParam
                && t.channel == channel && t.paramIndex == paramIndex)
            {
                t.depth = depth;
                t.paramName = paramName;
                changed();
                return;
            }

        ModTarget t;
        t.kind       = AutoTargetKind::channelParam;
        t.channel    = channel;
        t.paramIndex = paramIndex;
        t.paramName  = paramName;
        t.depth      = depth;
        m->targets.push_back (std::move (t));
        changed();
    }

    void removeModulation (int index, int targetIndex)
    {
        auto* m = modulator (index);
        if (m == nullptr || ! juce::isPositiveAndBelow (targetIndex, (int) m->targets.size()))
            return;
        m->targets.erase (m->targets.begin() + targetIndex);
        changed();
    }

    // ---- warp markers ----------------------------------------------------
    //
    // Editing rules worth knowing, all of them chosen to match what Live does,
    // because that is what a producer coming from Live will expect:
    //
    //  - Adding a marker does not change the sound. It pins whatever the clip
    //    is already reading at that beat, so the line it describes is the same
    //    line as before, just with one more point on it. Only dragging it
    //    afterwards warps anything.
    //  - A marker cannot be dragged past its neighbours. It stops just short
    //    instead, because crossing would make the clip read backwards.
    //  - Removing one merges the two segments either side of it.

    /** Pins whatever the clip reads at `beatsIn` to that beat. Returns the new
        marker's index, or -1 if it was too close to an existing one to be
        usable. */
    int addWarpMarker (int id, double beatsIn)
    {
        auto* c = find (id);
        // A warp map describes one piece of audio and a folder holds several,
        // so a folder has no markers; see tidyFolder.
        if (c == nullptr || ! c->isAudio() || c->isTakeFolder() || beatsIn <= 0.0)
            return -1;

        // Not past the clip's own end either. Snapping can round a click near
        // a short clip's edge beyond it, and a marker out there could never be
        // seen or grabbed again. Adding one does not move the line, so the
        // length measured here is the length afterwards too.
        if (beatsIn >= c->lengthBeats (bpm))
            return -1;

        const WarpMarker m { c->sourceAtBeat (bpm, beatsIn), beatsIn };
        c->warp.push_back (m);
        c->tidyWarp();
        retuneWarpRatio (id);
        changed();

        for (size_t i = 0; i < c->warp.size(); ++i)
            if (c->warp[i].beat == m.beat && c->warp[i].source == m.source)
                return (int) i;

        return -1;
    }

    /** Moves a marker along the grid, keeping it on the same point in the
        audio. Returns where it ended up, which may be short of where it was
        asked to go: a marker stops before its neighbours rather than crossing
        them. */
    double moveWarpMarker (int id, int index, double beatsIn)
    {
        auto* c = find (id);
        if (c == nullptr || ! juce::isPositiveAndBelow (index, (int) c->warp.size()))
            return 0.0;

        const int last = (int) c->warp.size() - 1;
        const double lo = (index == 0 ? 0.0 : c->warp[(size_t) index - 1].beat) + kMinWarpBeatSpan;
        double wanted = std::max (lo, beatsIn);
        if (index < last)
            wanted = std::min (wanted, c->warp[(size_t) index + 1].beat - kMinWarpBeatSpan);

        if (c->warp[(size_t) index].beat != wanted)
        {
            c->warp[(size_t) index].beat = wanted;
            changed();
        }
        return wanted;
    }

    void removeWarpMarker (int id, int index)
    {
        auto* c = find (id);
        if (c == nullptr || ! juce::isPositiveAndBelow (index, (int) c->warp.size()))
            return;
        c->warp.erase (c->warp.begin() + index);
        retuneWarpRatio (id);
        changed();
    }

    /** Puts a warped clip's stretch ratio back to the clip's own average rate.

        The markers set the timing; the ratio only decides how much of the work
        Rubber Band does, and therefore how much is left to plain interpolation.
        At the average rate the stretched copy is read at close to real time, so
        only the drift the markers correct is repitched, and by very little.

        Called when a marker edit has finished rather than while one is in
        progress: the ratio is part of what keys the rendered copy, so changing
        it mid-drag would start a fresh render on every mouse move.
    */
    void retuneWarpRatio (int id)
    {
        auto* c = find (id);
        if (c == nullptr || ! c->isWarped() || c->length <= 1.0e-9 || bpm <= 0.0)
            return;

        const double wanted = juce::jlimit (0.1, 10.0, c->lengthBeats (bpm) * 60.0 / bpm / c->length);
        if (std::abs (wanted - c->stretch) > 1.0e-9)
        {
            c->stretch = wanted;
            changed();
        }
    }

    /** Turns warping off, leaving the clip on its single stretch ratio.

        The ratio is set so the clip keeps the length it had while warped,
        rather than snapping back to whatever the ratio happened to be before
        the first marker was placed.
    */
    void clearWarp (int id)
    {
        auto* c = find (id);
        if (c == nullptr || ! c->isWarped())
            return;

        const double beats = c->lengthBeats (bpm);
        c->warp.clear();
        if (c->length > 1.0e-9 && beats > 1.0e-9)
            c->stretch = juce::jlimit (0.1, 10.0, beats * 60.0 / bpm / c->length);
        c->followTempo = false;   // its length no longer came from a source tempo
        changed();
    }

    /** Re-stretches every clip that follows the tempo. Called when the project
        tempo changes, so matched audio tracks the grid instead of drifting. */
    void retuneTempoFollowers()
    {
        bool any = false;
        if (bpm <= 0.0)
            return;

        auto retune = [&any] (Clip& c, double wanted)
        {
            wanted = juce::jlimit (0.1, 10.0, wanted);
            if (std::abs (wanted - c.stretch) > 1.0e-9)
            {
                c.stretch = wanted;
                any = true;
            }
        };

        for (auto& c : clips)
        {
            if (! c.isAudio())
                continue;

            if (c.isWarped())
            {
                // A warped clip always follows the tempo, whether or not it was
                // asked to: its markers are in beats, so they hold their grid
                // positions on their own and the clip's length in beats does not
                // change. What does need recomputing is the stretch ratio, which
                // is no longer what sets the timing but is still what decides how
                // much of the work Rubber Band does. Keeping it at the clip's own
                // average rate means the stretched copy is read at close to real
                // time, so the pitch stays where it was instead of rising with
                // the tempo.
                if (c.length > 1.0e-9)
                    retune (c, c.lengthBeats (bpm) * 60.0 / bpm / c.length);
            }
            else if (c.followTempo && c.sourceBpm > 0.0)
            {
                retune (c, c.sourceBpm / bpm);
            }
        }

        if (any)
            changed();
    }

    void setRackNote (int channel, int note)
    {
        auto& info = channels[(size_t) channel];
        const int from = info.rackNote;
        info.rackNote = juce::jlimit (0, 127, note);

        if (auto* clip = rackClip (channel))
            if (clip->pattern != nullptr)
                for (auto& n : clip->pattern->notes)
                    if (n.note == from)
                        n.note = info.rackNote;

        changed();
    }

    std::vector<Clip> selectedClips() const
    {
        std::vector<Clip> out;
        for (auto& c : clips)
            if (selection.count (c.id))
                out.push_back (c);
        return out;
    }

    void removeSelected()
    {
        clips.erase (std::remove_if (clips.begin(), clips.end(),
                                     [this] (const Clip& c) { return selection.count (c.id) > 0; }),
                     clips.end());
        selection.clear();
        changed();
    }

    void removeClipsOnChannel (int channel)
    {
        clips.erase (std::remove_if (clips.begin(), clips.end(),
                                     [channel] (const Clip& c) { return ! c.isAudio() && c.channel == channel; }),
                     clips.end());
        changed();
    }

    // Pastes clips (with positions relative to 0) starting at a beat.
    void paste (const std::vector<Clip>& relative, double atBeat)
    {
        selection.clear();
        for (auto& c : relative)
        {
            auto copy = c.deepCopy();
            copy.start += atBeat;
            selection.insert (addClip (copy));
        }
        changed();
    }

    void duplicateSelected()
    {
        auto picked = selectedClips();
        if (picked.empty())
            return;

        double first = 1e12, last = 0.0;
        for (auto& c : picked) { first = std::min (first, c.start); last = std::max (last, c.endBeat (bpm)); }
        const double span = std::ceil ((last - first) * 4.0 - 1e-9) / 4.0;   // round up to a step

        selection.clear();
        for (auto& c : picked)
        {
            auto copy = c.deepCopy();
            copy.start += span;
            selection.insert (addClip (copy));
        }
        changed();
    }

    // Cuts a clip in two at a beat. Returns the new right-hand clip id, or 0.
    int split (int id, double atBeat)
    {
        auto* c = find (id);
        if (c == nullptr || atBeat <= c->start + 1e-6 || atBeat >= c->endBeat (bpm) - 1e-6)
            return 0;

        // A comp is measured from the folder's start, and both halves of a cut
        // folder would need the whole of every pass to stay meaningful. Rather
        // than half support that, a folder is not cut: comping is already the
        // way to say which part of it you want.
        if (c->isTakeFolder())
            return 0;

        Clip right = c->deepCopy();
        const double cutBeats = atBeat - c->start;
        right.start = atBeat;

        if (c->isAudio())
        {
            // Cut where the clip is actually reading at that beat, which is the
            // warp map's answer and reduces to the old one when there are no
            // markers.
            const double cutSource = c->sourceAtBeat (bpm, cutBeats) - c->offset;

            // The right hand piece reads at the rate the original was reading
            // at the cut, so splitting a warped clip is inaudible. That matters
            // when the cut lands after the last marker, where the right piece
            // keeps no markers at all and has only its stretch ratio to go on.
            right.stretch = warpStretchForSlope (bpm, c->slopeAtBeat (bpm, cutBeats));

            right.offset = c->offset + cutSource;
            right.length = c->length - cutSource;
            c->length    = cutSource;

            rebaseWarpMarkers (right.warp, cutBeats, right.offset);

            // The left piece keeps every marker, including any past its new
            // end: they do not affect the audio before them, and they are worth
            // keeping in case the edge is dragged back out again.
        }
        else
        {
            right.offset = c->offset + cutBeats;
            right.length = c->length - cutBeats;
            c->length    = cutBeats;
        }
        return addClip (right);
    }

    // Exact end of the last clip: playback loops here.
    double songEndBeats() const
    {
        double end = 0.0;
        for (auto& c : clips)
            end = std::max (end, c.endBeat (bpm));
        return end;
    }

    int firstArmedTrack() const
    {
        for (int i = 0; i < (int) tracks.size(); ++i)
            if (tracks[(size_t) i].armed)
                return i;
        return -1;
    }

    int firstEmptyTrackFrom (int from) const
    {
        for (int t = std::max (0, from); t < (int) tracks.size(); ++t)
        {
            bool used = false;
            for (auto& c : clips)
                used = used || c.track == t;
            if (! used)
                return t;
        }
        return juce::jlimit (0, (int) tracks.size() - 1, from);
    }

    void changed() { sendChangeMessage(); }

    // ---- Undo / redo (unlimited) ------------------------------------------
    // Call commit() once an edit is finished. It records a step only if the
    // arrangement actually changed since the last commit.
    bool commit()
    {
        if (sameArrangement (committed))
            return false;
        auto next = capture (&committed);
        undoStack.push_back (std::move (committed));
        committed = std::move (next);
        redoStack.clear();
        return true;
    }

    bool undo()
    {
        commit();                        // don't lose an edit that wasn't recorded yet
        if (undoStack.empty())
            return false;
        redoStack.push_back (std::move (committed));
        committed = std::move (undoStack.back());
        undoStack.pop_back();
        restore (committed);
        return true;
    }

    bool redo()
    {
        commit();
        if (redoStack.empty())
            return false;
        undoStack.push_back (std::move (committed));
        committed = std::move (redoStack.back());
        redoStack.pop_back();
        restore (committed);
        return true;
    }

    size_t undoSteps() const noexcept { return undoStack.size(); }
    size_t redoSteps() const noexcept { return redoStack.size(); }

private:
    static bool sameTrack (const Track& a, const Track& b)
    {
        // arming isn't an edit, so it doesn't create undo steps
        return a.name == b.name && a.muted == b.muted && a.insert == b.insert;
    }

    static bool sameClip (const Clip& a, const Clip& b)
    {
        if (a.id != b.id || a.type != b.type || a.sample != b.sample || a.channel != b.channel
            || a.track != b.track || a.start != b.start || a.offset != b.offset || a.length != b.length
            || a.stretch != b.stretch || a.pitch != b.pitch || a.gainDb != b.gainDb || a.muted != b.muted
            || a.sourceBpm != b.sourceBpm || a.followTempo != b.followTempo
            || a.warp != b.warp)
            return false;
        if (a.curve != b.curve
            && ! (a.curve != nullptr && b.curve != nullptr && *a.curve == *b.curve))
            return false;
        // A comp edit is an edit: choosing a different take over a bar changes
        // what the song sounds like, so it belongs in the undo history, and
        // one finished swipe has to be one step.
        if (a.folder != b.folder
            && ! (a.folder != nullptr && b.folder != nullptr && *a.folder == *b.folder))
            return false;
        if (a.pattern == b.pattern)
            return true;
        return a.pattern != nullptr && b.pattern != nullptr && *a.pattern == *b.pattern;
    }

    bool sameArrangement (const ProjectState& s) const
    {
        if (s.tracks.size() != tracks.size() || s.clips.size() != clips.size())
            return false;
        for (size_t i = 0; i < tracks.size(); ++i)
            if (! sameTrack (tracks[i], s.tracks[i]))
                return false;
        for (size_t i = 0; i < clips.size(); ++i)
            if (! sameClip (clips[i], s.clips[i]))
                return false;
        return true;
    }

    ProjectState capture (const ProjectState* previous) const
    {
        ProjectState s;
        s.tracks    = tracks;
        s.selection = selection;
        s.clips.reserve (clips.size());

        for (const auto& c : clips)
        {
            Clip copy = c;
            if (c.pattern != nullptr)
            {
                std::shared_ptr<MidiPattern> reuse;
                if (previous != nullptr)
                    for (const auto& old : previous->clips)
                        if (old.id == c.id && old.pattern != nullptr && *old.pattern == *c.pattern)
                            reuse = old.pattern;
                copy.pattern = reuse != nullptr ? reuse : std::make_shared<MidiPattern> (*c.pattern);
            }
            if (c.curve != nullptr)
            {
                std::shared_ptr<AutoCurve> reuse;
                if (previous != nullptr)
                    for (const auto& old : previous->clips)
                        if (old.id == c.id && old.curve != nullptr && *old.curve == *c.curve)
                            reuse = old.curve;
                copy.curve = reuse != nullptr ? reuse : std::make_shared<AutoCurve> (*c.curve);
            }
            if (c.folder != nullptr)
            {
                // Shared rather than copied where nothing changed, which
                // matters more here than anywhere else: a folder holds ten
                // passes of a vocal, and a hundred undo steps each holding
                // their own copy of that list would be a hundred copies of
                // the session. Only the bookkeeping is copied in any case;
                // the audio itself is shared through SampleData.
                std::shared_ptr<TakeFolder> reuse;
                if (previous != nullptr)
                    for (const auto& old : previous->clips)
                        if (old.id == c.id && old.folder != nullptr && *old.folder == *c.folder)
                            reuse = old.folder;
                copy.folder = reuse != nullptr ? reuse : std::make_shared<TakeFolder> (*c.folder);
            }
            s.clips.push_back (std::move (copy));
        }
        return s;
    }

    void restore (const ProjectState& s)
    {
        std::vector<Track> restored = s.tracks;
        for (size_t i = 0; i < restored.size() && i < tracks.size(); ++i)
            restored[i].armed = tracks[i].armed;
        tracks = std::move (restored);

        clips.clear();
        for (const auto& c : s.clips)
            clips.push_back (c.deepCopy());   // live edits never touch history

        selection.clear();
        for (int id : s.selection)
            if (find (id) != nullptr)
                selection.insert (id);

        changed();
    }

    int nextId = 1;
    ProjectState committed;
    std::vector<ProjectState> undoStack, redoStack;
};
