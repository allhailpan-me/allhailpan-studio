#include "MidiFileIO.h"

#include <map>
#include <set>

namespace MidiFileIO
{

namespace
{
    /** Seconds per tick for the SMPTE form of the header's division word,
        where bit 15 is set: the upper byte is the negated frame rate and the
        lower byte is ticks per frame. 29 means 30 frames dropped to
        30000/1001, which is what every piece of video equipment means by it.
    */
    double smpteSecondsPerTick (short timeFormat)
    {
        const int frameCode = (-timeFormat) >> 8;
        const int perFrame  = timeFormat & 0xff;

        double framesPerSecond = 30.0;
        switch (frameCode)
        {
            case 24: framesPerSecond = 24.0; break;
            case 25: framesPerSecond = 25.0; break;
            case 29: framesPerSecond = 30.0 * 1000.0 / 1001.0; break;
            case 30: framesPerSecond = 30.0; break;
            default: break;
        }

        if (perFrame <= 0)
            return 0.0;

        return 1.0 / (framesPerSecond * (double) perFrame);
    }

    MidiNote toMidiNote (const TimebaseNote& n)
    {
        MidiNote out;
        out.start    = n.start;
        out.length   = n.length;
        out.note     = n.note;
        out.velocity = n.velocity;
        return out;
    }

    TimebaseNote toTimebaseNote (const MidiNote& n)
    {
        TimebaseNote out;
        out.start    = n.start;
        out.length   = n.length;
        out.note     = n.note;
        out.velocity = n.velocity;
        return out;
    }
}

// ---------------------------------------------------------------------------
// Import

ImportResult read (const juce::File& file)
{
    ImportResult result;

    if (! file.existsAsFile())
    {
        result.error = file.getFileName() + ": the file isn't there";
        return result;
    }

    juce::FileInputStream stream (file);
    if (! stream.openedOk())
    {
        result.error = file.getFileName() + ": couldn't be opened";
        return result;
    }

    juce::MidiFile midi;

    // Missing note offs are matched for us, which is what makes a file that
    // was truncated or written by a hardware sequencer usable rather than a
    // pile of notes that never end.
    if (! midi.readFrom (stream, true, &result.fileType))
    {
        result.error = file.getFileName() + ": isn't a MIDI file this studio can read";
        return result;
    }

    const short timeFormat = midi.getTimeFormat();
    if (timeFormat == 0)
    {
        result.error = file.getFileName() + ": its header gives no timebase";
        return result;
    }

    // Tempo. The project has one tempo, so the first Set Tempo event is the
    // one that counts; a file carrying several is read as its opening tempo
    // and the caller is told, because silently flattening a tempo map would
    // put everything after the first change in the wrong place.
    juce::MidiMessageSequence tempoEvents;
    midi.findAllTempoEvents (tempoEvents);

    std::set<int> distinctTempos;
    bool haveTempo = false;

    for (int i = 0; i < tempoEvents.getNumEvents(); ++i)
    {
        const auto& m = tempoEvents.getEventPointer (i)->message;
        if (! m.isTempoMetaEvent())
            continue;

        // The event holds a whole number of microseconds per quarter note, so
        // it is rounded back to one before being read as a tempo: the detour
        // through seconds is JUCE's accessor, not the file, and letting its
        // last bit through would make a project reopened at 128 bpm sit at
        // 127.99999 and never compare equal to its own default again.
        const int microseconds = (int) std::llround (m.getTempoSecondsPerQuarterNote() * 1000000.0);
        if (microseconds <= 0)
            continue;

        distinctTempos.insert (microseconds);

        if (! haveTempo)
        {
            result.bpm = midiBpmFromMicrosecondsPerQuarterNote ((double) microseconds);
            haveTempo = true;
        }
    }

    // Several different tempos is a tempo map, which this project cannot
    // follow yet. The opening tempo is used and the caller says so, because
    // flattening the rest silently would put everything after the first
    // change in the wrong place with no hint why.
    result.hasTempoChanges = distinctTempos.size() > 1;

    // A file with no tempo means 120 bpm by convention, but the caller should
    // be able to tell "it said 120" from "it said nothing", so this stays 0
    // and the tempo used for the SMPTE conversion below is the convention.
    const double tempoForSmpte = haveTempo ? result.bpm : 120.0;
    const double secondsPerTick = timeFormat > 0 ? 0.0 : smpteSecondsPerTick (timeFormat);

    if (timeFormat < 0 && secondsPerTick <= 0.0)
    {
        result.error = file.getFileName() + ": its header gives an impossible frame rate";
        return result;
    }

    auto beatsAt = [timeFormat, secondsPerTick, tempoForSmpte] (double ticks)
    {
        if (timeFormat > 0)
            return midiBeatsFromTicks ((long long) std::llround (ticks), timeFormat);

        return midiBeatsFromSmpteTicks ((long long) std::llround (ticks), secondsPerTick, tempoForSmpte);
    };

    // The resolution the notes are quantised to on the way in. For a beat
    // based file that is the file's own, so nothing moves at all; for a SMPTE
    // file there is no beat resolution in the header, so the studio's own is
    // the best available.
    const int ppq = timeFormat > 0 ? (int) timeFormat : kMidiExportPpq;

    for (int t = 0; t < midi.getNumTracks(); ++t)
    {
        const auto* track = midi.getTrack (t);
        if (track == nullptr)
            continue;

        // The track's own name, from a Sequence/Track Name meta event. In the
        // first track of a format 1 file the specification says this names the
        // sequence rather than the track, which is where the file's name
        // comes from.
        juce::String trackName;
        for (int i = 0; i < track->getNumEvents(); ++i)
        {
            const auto& m = track->getEventPointer (i)->message;
            if (m.isTrackNameEvent())
            {
                trackName = m.getTextFromTextMetaEvent().trim();
                break;
            }
        }

        // Which MIDI channels carry notes here. A format 0 file puts every
        // part on one track and tells them apart by channel, so splitting is
        // the only way that file arrives as an arrangement.
        std::set<int> channels;
        for (int i = 0; i < track->getNumEvents(); ++i)
        {
            const auto& m = track->getEventPointer (i)->message;
            if (m.isNoteOn())
                channels.insert (m.getChannel());
        }

        if (channels.empty())
        {
            // No notes: a tempo map, or a marker track. Its name is the
            // sequence name if nothing has claimed that yet.
            if (result.sequenceName.isEmpty())
                result.sequenceName = trackName;
            continue;
        }

        if (channels.size() > 1)
            result.splitByChannel = true;

        if (t == 0 && result.sequenceName.isEmpty())
            result.sequenceName = trackName;

        for (int channel : channels)
        {
            std::vector<TickNote> ticks;

            for (int i = 0; i < track->getNumEvents(); ++i)
            {
                const auto& on = track->getEventPointer (i)->message;
                if (! on.isNoteOn() || on.getChannel() != channel)
                    continue;

                // getTimeOfMatchingKeyUp returns zero for an unmatched note
                // on, which would read as a note ending before it began, so
                // the index is checked instead.
                const int offIndex = track->getIndexOfMatchingKeyUp (i);

                const double onTicks = on.getTimeStamp();
                const double offTicks = offIndex >= 0 ? track->getEventTime (offIndex)
                                                      : track->getEndTime();

                TickNote n;
                n.startTick = midiTicksFromBeats (beatsAt (onTicks), ppq);
                n.endTick   = std::max (n.startTick + 1,
                                        midiTicksFromBeats (beatsAt (std::max (offTicks, onTicks)), ppq));
                n.note      = on.getNoteNumber();
                n.velocity  = on.getVelocity();
                ticks.push_back (n);
            }

            if (ticks.empty())
                continue;

            result.notesRead += (int) ticks.size();

            // The file may describe two notes of one pitch on one channel
            // overlapping, which no studio can play back as written. Settling
            // that here means what lands in the piano roll is what the file
            // will sound like, and that exporting it again gives this file
            // back rather than a different one.
            result.droppedOverlaps += midiResolveSamePitchOverlaps (ticks);

            ImportedTrack imported;
            imported.midiChannel = channel;
            imported.name = trackName;

            if (channels.size() > 1)
                imported.name = (trackName.isNotEmpty() ? trackName + " " : juce::String())
                                    + "ch " + juce::String (channel);

            if (imported.name.isEmpty())
                imported.name = "Track " + juce::String (t + 1);

            for (const auto& n : midiNotesFromTicks (ticks, ppq))
            {
                imported.notes.push_back (toMidiNote (n));
                result.lengthBeats = std::max (result.lengthBeats,
                                               imported.notes.back().start + imported.notes.back().length);
            }

            result.tracks.push_back (std::move (imported));
        }
    }

    if (result.tracks.empty())
    {
        result.error = file.getFileName() + ": there are no notes in it";
        return result;
    }

    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// Export

ExportResult write (const juce::File& file, const std::vector<ExportTrack>& tracks,
                    double bpm, const juce::String& sequenceName, bool hadAutomation)
{
    ExportResult result;
    result.hadAutomation = hadAutomation;

    if (tracks.empty())
    {
        result.error = "There are no MIDI notes to export.";
        return result;
    }

    juce::MidiFile midi;
    midi.setTicksPerQuarterNote (kMidiExportPpq);

    // Track 0 carries the tempo, the time signature and the sequence name and
    // no notes. The specification puts the sequence name in the first track
    // of a format 1 file, and a reader looking for the tempo looks here.
    {
        juce::MidiMessageSequence tempoTrack;

        if (sequenceName.isNotEmpty())
            tempoTrack.addEvent (juce::MidiMessage::textMetaEvent (3, sequenceName), 0.0);

        tempoTrack.addEvent (juce::MidiMessage::tempoMetaEvent (midiMicrosecondsPerQuarterNote (bpm)), 0.0);

        // Four four, because that is the only time signature this studio has.
        // Saying so is better than leaving it out: a reader that finds no time
        // signature is entitled to assume anything.
        tempoTrack.addEvent (juce::MidiMessage::timeSignatureMetaEvent (4, 4), 0.0);
        tempoTrack.addEvent (juce::MidiMessage::endOfTrack(), 0.0);

        midi.addTrack (tempoTrack);
    }

    for (const auto& track : tracks)
    {
        std::vector<TimebaseNote> notes;
        notes.reserve (track.notes.size());
        for (const auto& n : track.notes)
            notes.push_back (toTimebaseNote (n));

        int dropped = 0;
        const auto ticks = midiNotesToTicks (notes, kMidiExportPpq, &dropped);
        result.droppedOverlaps += dropped;

        if (ticks.empty())
            continue;

        const int channel = juce::jlimit (1, 16, track.midiChannel);

        // The events are ordered here rather than left to the sequence,
        // because when one note ends on the same tick another of that pitch
        // begins, the note off has to come first. Written the other way round
        // a reader sees a second note on while the first is still sounding and
        // ends it there, which is the same note but arrived at by accident, and
        // it makes JUCE's own pair matching insert a note off that does not
        // belong in the file.
        struct Event
        {
            long long tick = 0;
            bool      isNoteOff = false;
            int       note = 0;
            int       velocity = 0;
        };

        std::vector<Event> events;
        events.reserve (ticks.size() * 2);

        long long lastTick = 0;

        for (const auto& n : ticks)
        {
            events.push_back ({ n.startTick, false, n.note, n.velocity });
            events.push_back ({ n.endTick,   true,  n.note, 0 });
            lastTick = std::max (lastTick, n.endTick);
        }

        std::stable_sort (events.begin(), events.end(), [] (const Event& a, const Event& b)
        {
            if (a.tick != b.tick)           return a.tick < b.tick;
            if (a.isNoteOff != b.isNoteOff) return a.isNoteOff;    // releases first
            return a.note < b.note;
        });

        juce::MidiMessageSequence sequence;
        sequence.addEvent (juce::MidiMessage::textMetaEvent (3, track.name.isNotEmpty() ? track.name
                                                                                        : juce::String ("Track")), 0.0);

        for (const auto& e : events)
        {
            // The timestamps handed to the writer are whole ticks already, so
            // the rounding it does on the way out cannot move anything. That
            // is the point of doing the conversion here rather than handing it
            // beats and a tempo. Events at equal times keep the order they are
            // added in, so the sort above is what the file ends up holding.
            sequence.addEvent (e.isNoteOff ? juce::MidiMessage::noteOff (channel, e.note)
                                           : juce::MidiMessage::noteOn (channel, e.note,
                                                                        (juce::uint8) e.velocity),
                               (double) e.tick);
        }

        sequence.addEvent (juce::MidiMessage::endOfTrack(), (double) lastTick);

        midi.addTrack (sequence);

        result.notesWritten += (int) ticks.size();
        ++result.tracksWritten;
    }

    if (result.tracksWritten == 0)
    {
        result.error = "There are no MIDI notes to export.";
        return result;
    }

    // Written to a temporary first and moved into place, so a failure part way
    // through cannot leave a half written file where a good one used to be.
    auto temporary = file.getSiblingFile (file.getFileNameWithoutExtension() + ".mid.tmp");
    temporary.deleteFile();

    {
        juce::FileOutputStream out (temporary);
        if (! out.openedOk())
        {
            result.error = "Couldn't write to " + file.getFullPathName();
            return result;
        }

        if (! midi.writeTo (out, 1))
        {
            out.flush();
            temporary.deleteFile();
            result.error = "Couldn't write the MIDI file.";
            return result;
        }

        out.flush();
    }

    file.deleteFile();
    if (! temporary.moveFileTo (file))
    {
        temporary.deleteFile();
        result.error = "Couldn't put the finished file at " + file.getFullPathName();
        return result;
    }

    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------

std::vector<ExportTrack> gather (const Project& project, bool onlySelected)
{
    // One track per instrument channel, in channel order, so a sixteen
    // channel project reads the same way in another studio as it does here.
    std::map<int, std::vector<MidiNote>> byChannel;

    double earliest = 0.0;
    bool   haveEarliest = false;

    for (const auto& clip : project.clips)
    {
        if (clip.isAudio() || clip.pattern == nullptr)
            continue;
        if (onlySelected && project.selection.count (clip.id) == 0)
            continue;

        if (! haveEarliest || clip.start < earliest)
        {
            earliest = clip.start;
            haveEarliest = true;
        }
    }

    if (! haveEarliest)
        return {};

    // The arrangement keeps its absolute positions; a selection is brought to
    // the start of the file, which is what taking a few bars elsewhere means.
    const double shift = onlySelected ? earliest : 0.0;

    for (const auto& clip : project.clips)
    {
        if (clip.isAudio() || clip.pattern == nullptr)
            continue;
        if (onlySelected && project.selection.count (clip.id) == 0)
            continue;

        const int channel = juce::jlimit (0, kNumChannels - 1, clip.channel);

        // Exactly how the engine reads a clip: the pattern is anchored so that
        // its own beat `offset` falls on the clip's start, and only what the
        // clip's length actually plays is taken. Anything else would export a
        // part that does not match the one that was heard.
        const MidiClipWindow window { clip.start, clip.offset, clip.length };

        for (const auto& n : clip.pattern->notes)
        {
            double at = 0.0, length = 0.0;
            if (! midiClipPlaysNote (window, n.start, n.length, at, length))
                continue;

            MidiNote placed = n;
            placed.start  = at - shift;
            placed.length = length;

            if (placed.start < 0.0)
                continue;

            byChannel[channel].push_back (placed);
        }
    }

    std::vector<ExportTrack> tracks;

    for (auto& [channel, notes] : byChannel)
    {
        if (notes.empty())
            continue;

        ExportTrack track;

        const auto& info = project.channels[(size_t) channel];
        track.name = (info.name.isNotEmpty() ? info.name : juce::String ("Channel"))
                         + " " + juce::String (channel + 1);

        // One MIDI channel each, wrapping at sixteen because that is all the
        // format has. Channel 10 is percussion by the General MIDI convention,
        // but this studio's channels are whatever plugin sits in them, so they
        // are laid out in order and not shuffled around that.
        track.midiChannel = (channel % 16) + 1;
        track.notes = std::move (notes);

        tracks.push_back (std::move (track));
    }

    return tracks;
}

} // namespace MidiFileIO
