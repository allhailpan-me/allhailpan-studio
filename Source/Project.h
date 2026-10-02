#pragma once
#include <juce_events/juce_events.h>
#include "SampleData.h"
#include <set>
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
    float  gainDb  = 0.0f;
    bool   muted   = false;

    bool   isAudio() const noexcept { return type == ClipType::audio; }
    double lengthBeats (double bpm) const noexcept
    {
        return isAudio() ? length * stretch * bpm / 60.0 : length;
    }
    double endBeat (double bpm) const noexcept { return start + lengthBeats (bpm); }

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
    /** Re-stretches every clip that follows the tempo. Called when the project
        tempo changes, so matched audio tracks the grid instead of drifting. */
    void retuneTempoFollowers()
    {
        bool any = false;

        for (auto& c : clips)
            if (c.isAudio() && c.followTempo && c.sourceBpm > 0.0 && bpm > 0.0)
            {
                const double wanted = c.sourceBpm / bpm;
                if (std::abs (wanted - c.stretch) > 1.0e-9)
                {
                    c.stretch = wanted;
                    any = true;
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
            const double cutSource = cutBeats * 60.0 / bpm / c->stretch;
            right.offset = c->offset + cutSource;
            right.length = c->length - cutSource;
            c->length    = cutSource;
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
            || a.sourceBpm != b.sourceBpm || a.followTempo != b.followTempo)
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
