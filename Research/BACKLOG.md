# Research backlog

A running record of what each night looked at, what it built, what it rejected,
and why. Rejected ideas stay listed so a later night does not revisit them
blindly. Newest first.

---

## 2026-10-13: pan law, and the difference between panning and balancing

### What was looked at

| Candidate | Verdict |
|---|---|
| **Pan law, and a mono panner distinct from the stereo balance control** | **Chosen.** |
| MIDI record alignment, and the instrument tap with it | Deferred again, and the reason is the one the last night named rather than a new one: the figure is easy and the hard part is knowing whether the keyboard is being played live or a pattern is being bounced off the grid. Getting that wrong drags a bounce off the beat, which is worse than the few milliseconds it would fix. Still the right next thing once that distinction exists. |
| Clip fade in and fade out handles, and splice crossfades | Deferred again. Still the largest real gap, still reaches `Project`, `ProjectIO`, the playlist's drag handling and both render paths at once, and most of the work is in the window where nothing tests it. Worth a night that does only that. |
| A tuner | Deferred. Needs a published pitch detector, which is a night on its own, and the useful half is a window. |
| Punch in and punch out recording | Deferred. |
| Fader law | Noted as a real gap for a later night, not chosen. `processInsert` turns the fader position into gain with `(v / 0.8)^2`, so the travel is a squared taper with unity at 0.8 and about +7.75 dB at the top of the range. Every studio tapers a fader in decibels instead, so equal travel is equal decibels through the useful region and the number next to the fader means something. Smaller daily cost than panning, because a fader is ridden by ear anyway, and it changes the meaning of every stored `volume` in every saved project, which needs a version check. |
| A true stereo panner, with position and width | Rejected for tonight, recorded because it came out of the research. Ardour's `2in2out` panner treats each side of a stereo signal as its own mono source and panes both through the same law, which is what gives it a width control; Cubase calls the same idea the Stereo Combined Panner and warns in its own manual that collapsing the two positions to one point "increases the volume of the signal". It is a real feature and the groundwork below is what it would be built on, but it is of little use to somebody recording mono guitar and mono vocals, which is what this studio is for. |

### What was chosen, and why

**Panning a mono source off centre makes it quieter, and there is nothing to
set against that.**

`processInsert` turns the pan control into gain with `min(1, 1 - p)` and
`min(1, 1 + p)`. That is not an approximation of anything: it is exactly
Ardour's stereo balance panner, which does the same thing in
`Pannerbalance::update` (centre leaves both sides alone, and moving off centre
linearly attenuates the opposite side). As a balance control it is correct and
there is nothing wrong with it.

The problem is that it is the only thing here, and it is applied to every
insert whatever is on it. A guitar in one socket of a UMC1820 arrives as a
mono source, centred, which means the same signal in both halves of the
insert's buffer. Pan that hard left and the right half is multiplied by zero,
so the power halves: the guitar drops 3.01 dB the moment it is moved off
centre, and the fader has to be ridden back up to put it where it was. Every
mono track in the session has this, which for somebody who records guitar and
vocals is every track that carries a performance.

Every studio researched treats these as two different controls, chosen by what
is on the track:

- **Logic** gives a mono channel strip a Pan knob and a stereo strip a Balance
  knob by default, and says Balance "differs from Stereo Pan mode in that it
  controls the relative levels of two signals (Left and Right) at their
  outputs".
- **Ardour** ships three separate panners and lets the panner be chosen by
  right clicking it: `1in2out` for a mono source, `2in2out` for a stereo one,
  and `stereobalance` for the case where the content is arbitrary. Its manual
  recommends balance when the content cannot be trusted, because it "works
  with arbitrary content without danger of introducing comb filter artefacts".
- **Cubase** picks the panner per channel from its input and output
  configuration and lets it be changed from the pan control's context menu.
- **Reaper**'s default track pan mode is named for both halves at once,
  stereo balance for a stereo track and mono pan for a mono one.

So the gap is not a wrong formula, it is a missing control. What was built is
the mono panner, and the law it uses, with the balance control left exactly as
it is and still the default.

### Where the behaviour came from

- **The named laws and what each one means** come from Logic's own General
  Audio project settings page, which is the clearest published statement of
  the set and is where the word "compensated" is defined. It offers 0 dB,
  -3 dB, -3 dB Compensated, -4.5 dB, -4.5 dB Compensated, -6 dB and -6 dB
  Compensated, and defines them by the centre: -3 dB routes a centred signal
  "to the left and right output channels with a -3 dB level decrease" while
  the extremes pass "without any level change", and -3 dB Compensated routes a
  centred signal at "the same level" while the extremes get "a +3 dB level
  increase". So the number in a law's name is the gain applied to each side at
  centre, and "compensated" moves the whole curve up so that centre is unity
  instead.
- **The curves** come from Oland and Dannenberg's pan law reading for Carnegie
  Mellon's Introduction to Computer Music, which gives all three with their
  formulas: linear, `L = (pi/2 - theta)(2/pi)` and `R = theta(2/pi)`, half gain
  at centre and a "hole-in-the-middle"; constant power, `L = cos(theta)` and
  `R = sin(theta)`, about 0.71 at centre and total power flat at every
  position; and the -4.5 dB compromise, "the square root of the product of the
  linear and constant power laws", about 0.59 at centre. Those centre gains
  are 0.5, 0.707107 and 0.594604, which are -6.0206 dB, -3.0103 dB and
  -4.5154 dB, so each curve lands on the law Logic names it by without any
  fitting. Nothing here was chosen; the arithmetic was read and checked.
- **Logic's 0 dB law turns out to be the formula already here.** A centred
  signal passing at full level on both sides and an extreme passing at full
  level on one is `min(1, 1 - p)` and `min(1, 1 + p)` exactly. That is worth
  recording because it makes the change safe: the studio has been running
  Logic's 0 dB pan law all along, so keeping it as the default changes no
  existing mix, and Logic's own warning about it, that "a signal panned to the
  center position may sound louder than signals panned hard left or right", is
  a description of the complaint.
- **Constant power as the sensible default for the new mode** is derived here
  rather than copied, because the studios disagree about defaults. -3 dB
  Compensated is the only one of the seven that is both flat in power across
  the sweep, so moving a track does not change its loudness, and unity at
  centre, so switching an insert from balance to pan while it sits centred
  changes nothing at all and cannot click. Every other option fails one of
  those two.

### What was built

`Source/PanLaw.h`, with no JUCE in it: the two modes, the seven laws, the
stored round trip, and the gain pair for a position. `processInsert` keeps only
the load and the two `applyGain` calls, and because playback and `renderOffline`
both go through it, a bounce gets the law for free.

### Deliberately left for later

- **A true stereo panner with width**, as above.
- **Clip gain and the fader law**, both noted above.
- **A per-insert law, rather than one for the project.** Logic and Reaper both
  put the law in project settings, which is what was followed, and a law that
  varied per strip would make a session harder to reason about rather than
  easier.
- **Clipping at the extremes under a compensated law.** A compensated law is
  above unity everywhere but the centre, by up to +6.02 dB at the ends of the
  -6 dB law, which is inherent to what "compensated" means and is what Logic
  documents. Nothing warns about it. A meter already exists on every strip, so
  it is visible, but a strip that clips only when panned is worth a thought
  later.
- **Putting a strip fed from a mono socket into pan mode by itself.** Logic and
  Cubase both pick the panner from the channel's configuration, and
  `InputSource.h` already knows whether an input choice was mono, so the one
  case the studio could infer is exactly the case that matters most here. It
  was left out tonight because assigning an input would then change the level
  of a strip already panned off centre, with nothing to say so, which is the
  shape of fault this file keeps a record of. Worth doing with a visible
  notice, not silently.
- **Smoothing the pan gains.** `processInsert` applies one gain for the whole
  block, so changing the mode or the law during playback steps it at a block
  boundary. Under a compensated law or the 0 dB law a mode change at centre is
  silent, which is the case that matters, but an uncompensated law jumps by its
  own figure. The fader and the pan position already behave this way, so this
  is the mixer's existing habit rather than something new, and it should be
  fixed for all of them at once rather than for the pan law alone.
- **`MixerComponent.h` opens by naming another product to say what this mixer
  is.** Out of scope for a pan law change, and against this project's own rule
  about how to describe itself. One line, worth a tidy pass.

### What the audit found

Twenty five deliberate breakages of `PanLaw.h` were tried and twenty four
failed the test. The survivor is a genuine no-op and is now documented as one
in both the header and the test: taking the pan mode out of the compensation
condition changes nothing, because balance mode has already forced the flat
curve and the flat curve is unity at centre.

A separate agent audited the change against the real JUCE 9.0.3 headers and
found four things worth recording here, all fixed before merging:

1. **The test pinned the properties of every law and the shape of none of
   them.** Constant power could be replaced by a square root crossfade and
   every check still passed: same centre gain, same extremes, same flat power,
   same monotonicity, and 2.89 dB out at half pan. That is the exact failure
   `CLAUDE.md` describes as a number that is present, plausible and wrong, in
   the default law. The test now checks every law against its published curve
   at four thousand positions, which kills that and two more of the same kind.
2. **The law was read inside the insert loop**, so a change arriving mid block
   would have been applied to some inserts and not others, and an export, which
   runs on its own thread while the mixer stays live, could have been mixed
   under one law for its first half and another for its second. It is now read
   once per block and latched once per render, the way the record alignment
   figure is latched when recording starts.
3. **The master strip was offered a pan mode.** Meaningless by definition and
   actively harmful: an uncompensated law there would drop the whole mix by the
   law's figure into every bounce. The menu no longer offers it and the loader
   forces insert 0 to balance whatever the file says.
4. **Three comments described something other than what the code did**,
   including one above `centreGainDb` claiming it returns the figure in a
   law's name when for a compensated law it returns zero. That is the same
   fault `CLAUDE.md` records twice for `updateLatency`, so it is worth naming
   rather than quietly fixing.

The audit also raised the format version bump as a judgement call rather than a
defect: it is unconditional, so a project saved by this build is refused by the
previous release even when every insert is still a balance control, which will
be almost all of them for a while. Writing 3 in that case would keep those
files openable. It was left unconditional, matching versions 2 and 3, because a
format version that depends on the contents of the file is a rule that will be
got wrong by whoever adds the next piece of state.

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

**A take does not land where it was played, and what put part of it back was
in the wrong place and wrong in three ways.**

First pass at the diagnosis, recorded here because it was wrong and the
correction is the useful part: the engine was searched for a record offset,
none was found, and the conclusion drawn was that there had never been one.
There had. `MainComponent::finishAudioRecording` read past the head of every
pass by `engine.getRoundTripLatencySamples()` and set the clip's offset to it.
The audit found it, after the first version of this change had been written
and would have applied the correction twice, putting every take early by a
whole round trip. **The lesson for later nights: grep the consumer, not only
the producer.** `Recorder::Take` is produced in the engine and consumed in the
window, and half the behaviour lived at the far end.

What was actually wrong, once that was straight:

1. **The studio's own compensation was left out.** The offset used only the
   device's reported round trip. Put a lookahead limiter on the master and
   every take lands late again by its whole lookahead, because what a player
   hears is the mix after that delay. Nothing says so.
2. **A loop recording lost audio.** The head of each pass was read past, but
   the split points stayed where the transport wrapped, so the last
   milliseconds of each performance were handed to the next pass and then
   trimmed off its head. The last note before the loop came round went in the
   bin.
3. **It was clamped to half a pass**, so a short take was quietly half
   corrected rather than corrected or refused.
4. **It had no test**, because it was in the window. The three above are each
   exactly the kind of silent arithmetic `CLAUDE.md` says to keep out of
   there.

And the thing it has no answer for at all: **a driver that reports nothing.**
JUCE's ASIO backend zeroes both figures when the driver's getLatencies call
fails, so the old offset was zero on exactly that hardware, and there was no
way for anybody to correct it. Every other studio offers one: Pro Tools has a
record offset, Cubase has "Adjust for Record Latency", Logic a recording
delay, Reaper a manual offset per direction, and Ardour its measured systemic
latency.

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

- **MIDI record alignment, and the instrument tap with it.** This is the next
  thing to do here. The MIDI tap stamps `midiRecordStart` and every event beat
  straight from the transport, with no offset, and never had one. The figure
  is different and smaller than the audio one: a MIDI note never passes
  through the input converter, so it is the studio's own compensation plus the
  output latency and no input latency. Ardour does exactly that, setting
  `_accumulated_capture_offset = _playback_offset` on its MIDI path.

  Two consequences, derived rather than guessed:

  - **Rec: Audio and MIDI** captures a socket and a keyboard into one take.
    The audio half is now exact and the MIDI half is late by
    `engine + output`, so they sit `engine + output` apart. Before this
    change the audio half was late by `engine` and the MIDI half by
    `engine + output`, so they sat `output` apart. The gap grew, and it is
    never zero, because no device reports an output latency of zero.
  - **The instrument recorder** has the same open question and gets no offset
    for a reason that is a choice rather than a fact. It taps a channel's
    output inside the mixer, so there is nothing to put back on the converter
    side, but a player using the keyboard live is still following backing they
    hear `engine + output` late, so their performance lands that much late.
    When what is being bounced is a pattern off the arrangement there is
    nothing to correct and an offset would drag the bounce off the grid. The
    mode cannot tell a live performance from a bounce, and a bounce off the
    grid is the worse failure, so zero stands. Solving MIDI properly probably
    means knowing which of the two is happening, which is the real work here.
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
