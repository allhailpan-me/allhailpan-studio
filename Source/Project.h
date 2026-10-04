#pragma once
#include <juce_events/juce_events.h>
#include "SampleData.h"
#include "WarpMap.h"
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

struct ModTarget
{
    int   channel    = 0;      // instrument channel the parameter belongs to
    int   paramIndex = 0;
    float depth      = 0.5f;   // -1..1, added to the parameter's own value
    juce::String paramName;    // remembered for display when the plugin is absent

    bool operator== (const ModTarget&) const = default;
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

enum class ClipType { audio, midi };

struct Clip
{
    int      id   = 0;
    ClipType type = ClipType::audio;

    std::shared_ptr<SampleData>  sample;    // audio
    std::shared_ptr<MidiPattern> pattern;   // midi
    int channel = 0;                        // midi: which instrument slot

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

    bool   isAudio() const noexcept { return type == ClipType::audio; }
    bool   isWarped() const noexcept { return isAudio() && ! warp.empty(); }

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

    // Copies share nothing editable with the original
    Clip deepCopy() const
    {
        Clip c = *this;
        if (pattern != nullptr)
            c.pattern = std::make_shared<MidiPattern> (*pattern);
        return c;
    }

    juce::String displayName (const std::vector<ChannelInfo>& channels) const
    {
        if (isAudio())
            return sample != nullptr ? sample->name : juce::String ("Missing audio");
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
    double bpm = 128.0;

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
            if (t.channel == channel && t.paramIndex == paramIndex)
            {
                t.depth = depth;
                t.paramName = paramName;
                changed();
                return;
            }

        m->targets.push_back ({ channel, paramIndex, depth, paramName });
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
        if (c == nullptr || ! c->isAudio() || beatsIn <= 0.0)
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
