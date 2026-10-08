# Research backlog

A running record of what each night looked at, what it built, what it rejected,
and why. Rejected ideas stay listed so a later night does not revisit them
blindly. Newest first.

---

## 2026-10-08: where a recorded take lands on the timeline

### What was looked at

Candidates, judged against daily recording, editing, arranging and mixing of
guitar and vocals through a UMC1820:

| Candidate | Verdict |
|---|---|
| **Record alignment: compensating a take for round trip latency** | **Chosen.** |
| Clip fade in and fade out handles, and splice crossfades | Deferred. Standard in every studio and a real daily gap, but it reaches Project, ProjectIO, the playlist's drag handling and both render paths at once. Worth a night of its own. Note that `CompModel.h` already owns an equal-power crossfade and `AudioClipRT::fade` already carries a per-clip gain shape through both playback and export, so the engine side is mostly built. |
| Pan law | Deferred, but confirmed as a real gap: `mixChannelOutput` pans with `min(1, 1 - p)` and `min(1, 1 + p)`, which is a balance control with a 0 dB law, so a mono source moved off centre loses 3 dB of power and the fader has to be re-ridden. Reaper, Logic and Cubase all expose a pan law choice and all distinguish panning a mono source from balancing a stereo one. Needs research into which laws to offer and how a stereo insert should behave. |
| A tuner | Deferred. Standard in Logic, Cubase, Studio One and Reaper, and daily for a guitarist. Needs a published pitch detector (YIN, de Cheveigne and Kawahara 2002) rather than a guess, and `RealFFT.h` is already here to build on. |
| Dither on export | Rejected for now. Well specified, but exports are already written at 24 bit, where the quantisation floor is below anything audible, so there is nothing for it to fix until a 16 bit export option exists. |
| Punch in and punch out recording | Deferred. Standard everywhere and cheap, but less valuable than alignment: a punch that drops in at the wrong place is only wrong by the same latency this night fixes. |

### What was chosen, and why

**A take is recorded late by the full round trip, and nothing puts it back.**

`Recorder::push` stamps a capture with `beatAtBlockStart` and nothing else.
There is no record offset anywhere in the engine. So every take lands later on
the timeline than it was played, by the device's input latency plus its output
latency plus whatever the project's plugin compensation adds.

Three things made this the night's work:

1. **It is silent.** Nothing errors, nothing crackles. The take simply sits a
   few milliseconds behind the part it was played against, and the player
   assumes it was their timing. At a 480 sample buffer on plain Windows Audio
   it is twenty milliseconds or more, which is a late guitar on every single
   overdub.
2. **A comment claimed it was already handled.** `mixMonitorInput` says
   "the recorder already lines the take itself up with the arrangement", and
   `CLAUDE.md` repeats it: "Aligning the take with the arrangement is handled
   separately, by the recorder." Neither was true. That is precisely the shape
   of fault `CLAUDE.md` warns about in the latency graph: two real bugs there
   "had a comment above them claiming the opposite of what the code did".
3. **It is standard everywhere.** Pro Tools has a record offset, Cubase has
   "Adjust for Record Latency", Logic has a recording delay, Reaper has driver
   reported latency plus a manual offset per direction, and Ardour does it from
   its measured systemic latency.

### Where the behaviour came from

- **JUCE's own contract for the two figures**, read from the 9.0.3 header
  rather than from memory. `juce_audio_devices/audio_io/juce_AudioIODevice.h`:
  `getOutputLatencyInSamples` is "the delay in samples between a callback
  getting a block of data, and that data actually getting played";
  `getInputLatencyInSamples` is "the delay in samples between some audio
  actually arriving at the soundcard, and the callback getting passed this
  block of data". Both are measured at the callback boundary, which is what
  makes them add to exactly the figure wanted here.
- **Ardour as the reference implementation**, read from source. `Route::update
  _signal_latency` sets each processor's `capture_offset` to the route's input
  latency and its `playback_offset` to `_signal_latency + _output_latency`.
  `DiskWriter::check_record_status` then does
  `_first_recordable_sample += _capture_offset + _playback_offset` whenever the
  alignment style is `ExistingMaterial`. So the total is input latency plus
  output latency plus the processing latency in the playback path, and the
  compensation is applied by starting the capture that many samples later in
  the incoming stream rather than by moving the clip.
- **Ardour's two alignment styles** settle the one case that should not be
  compensated. `Track::set_align_choice_from_io` chooses `ExistingMaterial`
  when a track is fed from a physical input and `CaptureTime` when it is not.
  The instrument recorder here taps a channel's own output inside the mixer,
  never a socket, so it is a `CaptureTime` capture and gets no offset.
- **Ardour on the figures being unknowable without measurement**: the manual
  says "the only way to accurately learn about the total (additional) latency
  is to measure it", which is the argument for the manual trim every other
  studio also offers.

### What was built

`Source/RecordAlign.h`, with no JUCE in it, holding two things: the offset
arithmetic, and the alignment of one continuous capture into the passes a loop
recording produces. `Recorder` keeps only the buffer copying.

### Deliberately left for later

- **MIDI record alignment.** The same fault exists on the MIDI tap:
  `midiRecordStart` and every event beat are stamped straight from the
  transport. The offset is different and smaller: a MIDI note never passes
  through the input converter, so it is the playback latency plus the output
  latency and no input latency. Ardour does exactly this, setting
  `_accumulated_capture_offset = _playback_offset` on the MIDI path. Not done
  tonight to keep one change to one claim.
- **The last few milliseconds of a take.** Compensation is applied by starting
  the kept audio later in the capture, which means the tail now ends that much
  earlier, because the engine stops pushing the moment the transport stops.
  Ardour keeps its capture running past the stop point for the same reason
  (`_last_recordable_sample += _capture_offset + _playback_offset`). Doing that
  here means keeping the capture alive after the transport stops, which is
  transport surgery for the sake of five milliseconds of ring-out a second
  after the music ended.
- **Measuring the real round trip with a loopback cable**, the way
  `jack_iodelay` does. That is what makes the manual trim a number instead of
  a guess, and it is a night's work on its own: play a known impulse, record
  it, correlate, report the difference against what the driver claimed.
- **Tapping an instrument recording at the plugin's own reported latency.** The
  instrument recorder reads a channel's output at the playhead, so a latent
  instrument plugin bounces late by its own reported latency. Separate problem,
  separate figure.
