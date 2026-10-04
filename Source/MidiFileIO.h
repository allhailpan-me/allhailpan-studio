#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include "Project.h"
#include "MidiTimebase.h"

// Standard MIDI File import and export.
//
// This is how a part written here leaves the studio and how a part written
// elsewhere comes in, so what matters most is that it is exact: a file
// exported and imported again gives back the same notes, with the same
// starts, lengths, pitches and velocities. The arithmetic that makes that
// true lives in MidiTimebase.h, with no JUCE in it, so Tests/ can prove it;
// this layer only turns whole ticks into MIDI messages and back, using
// juce::MidiFile for the bytes rather than writing a parser.
//
// Automation is deliberately not exported. A MIDI file can only carry it as
// controller messages aimed at a controller number, and the lanes here are
// plugin parameters by index, which means any mapping would be an invention
// that a receiving studio would read as the wrong thing. Notes are exported,
// and the import and export both say so plainly rather than implying the
// whole clip travelled.
namespace MidiFileIO
{
    inline const juce::String wildcard { "*.mid;*.midi" };

    inline bool isMidiFile (const juce::File& f)
    {
        return f.existsAsFile() && f.hasFileExtension ("mid;midi");
    }

    /** One track read out of a file: its notes in beats from the start of the
        file, and the name it carried, if any. */
    struct ImportedTrack
    {
        juce::String          name;
        std::vector<MidiNote> notes;
        int  midiChannel = 1;    // the channel its notes were on, 1..16
    };

    struct ImportResult
    {
        bool ok = false;
        juce::String error;

        std::vector<ImportedTrack> tracks;

        juce::String sequenceName;      // the file's own name for the sequence
        double  bpm = 0.0;              // 0 when the file states no tempo
        bool    hasTempoChanges = false;  // more than one distinct tempo
        int     fileType = 1;           // 0, 1 or 2 as the header declares
        double  lengthBeats = 0.0;

        int  notesRead = 0;
        int  droppedOverlaps = 0;       // notes one channel could not describe
        bool splitByChannel = false;    // a track carried more than one channel
    };

    /** Reads a .mid into one set of notes per track.

        Multi-track files are the normal case, so tracks are kept apart rather
        than flattened. A track carrying more than one MIDI channel is split
        by channel, which is what a format 0 file always is and what a badly
        written format 1 file sometimes is; without that, a sixteen part
        arrangement would arrive as one unplayable pile.

        Tracks with no notes are left out. The first one usually is the tempo
        map, which is where the file's name and tempo come from.
    */
    ImportResult read (const juce::File&);

    /** What to write: one track per entry, in order. */
    struct ExportTrack
    {
        juce::String          name;
        std::vector<MidiNote> notes;    // beats from the start of the file
        int  midiChannel = 1;           // 1..16
    };

    struct ExportResult
    {
        bool ok = false;
        juce::String error;

        int notesWritten    = 0;
        int tracksWritten   = 0;
        int droppedOverlaps = 0;   // same pitch, same tick, so unrepresentable
        bool hadAutomation  = false;   // lanes were present and not exported
    };

    /** Writes a format 1 file: a tempo track, then one track per entry.

        Format 1 means "one or more simultaneous tracks", which is what an
        arrangement is, and putting the tempo and the sequence name in a first
        track of their own is the convention every studio both writes and
        expects to read.
    */
    ExportResult write (const juce::File&, const std::vector<ExportTrack>&,
                        double bpm, const juce::String& sequenceName,
                        bool hadAutomation = false);

    // ---- the arrangement ------------------------------------------------

    /** Gathers the project's MIDI clips into one export track per instrument
        channel, at their arrangement positions.

        A clip is a window onto its pattern, so only the notes the window
        actually plays are written, placed and clamped exactly as the engine
        places and clamps them when it plays the clip. Several clips on one
        channel merge into that channel's track, which is what makes the
        result read as the arrangement rather than as a pile of fragments.

        Groove is not baked in. It is applied as the arrangement is handed to
        the engine and never changes the stored notes, so writing the swung
        timing into the file would make the feel permanent in a way that
        turning it off here could not undo.

        When onlySelected is set, only selected clips are gathered and the
        result is shifted so the earliest of them starts at the beginning of
        the file, which is what exporting a few bars to take elsewhere means.
    */
    std::vector<ExportTrack> gather (const Project&, bool onlySelected);
}
