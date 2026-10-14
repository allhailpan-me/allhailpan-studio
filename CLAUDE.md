# Notes for working on this codebase

ALLHAILPAN Studio is a digital audio workstation: a JUCE application in C++20,
about twenty two thousand lines across sixty files in `Source`, with another nine
thousand of standalone tests in `Tests`, built with CMake and FetchContent. It
ships with its own instrument, hosts VST3 plugins, records audio and MIDI,
warps and comps takes, and exports finished mixes.

This file is what a newcomer would otherwise have to learn by breaking things.

## Build

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

JUCE is fetched automatically and pinned to a release tag, not master, so the
build is reproducible. Rubber Band is fetched the same way and compiled from
its single-file build.

**JUCE 9 moved the plugin hosting base classes.** `AudioPluginFormat`,
`AudioPluginFormatManager`, `AudioPluginInstance` and `AudioProcessor` are no
longer in `juce_audio_processors`: they live in a new headless module,
`juce_audio_processors_headless`, which the former depends on. There is no
`juce_AudioPluginFormat.h` under `juce_audio_processors` at all any more.
Anything written from memory of where these were in JUCE 7 or 8 will not
compile, and since JUCE cannot be built in the sandbox the first sign of it is
a red CI run twenty minutes later. When working against a JUCE API that
cannot be compiled here, read the real header first:

```
git clone --depth 1 --branch 9.0.3 --filter=blob:none --sparse \
    https://github.com/juce-framework/JUCE.git juce
cd juce && git sparse-checkout set modules/juce_audio_processors_headless
```

Fetching raw.githubusercontent.com is blocked and the GitHub contents API is
limited to this repository, so a sparse clone is the way in.

For the record, `AudioPluginFormat`'s pure virtuals in 9.0.3 are `getName`,
`findAllTypesForFile`, `fileMightContainThisPluginType`,
`getNameOfPluginFromIdentifier`, `pluginNeedsRescanning`,
`doesPluginStillExist`, `canScanForPlugins`, `isTrivialToScan`,
`searchPathsForPlugins`, `getDefaultLocationsToSearch` and
`requiresUnblockedMessageThreadDuringCreation`, plus a protected
`createPluginInstance` taking a
`std::function<void (std::unique_ptr<AudioPluginInstance>, const String&)>`.
`AudioPluginInstance` adds one of its own, `fillInPluginDescription`, on top
of everything `AudioProcessor` already requires.

Every push is built for Windows, macOS and Linux. A failing build echoes its
compiler errors back as GitHub annotations, so the reason is readable from the
run summary without downloading the log:

```
gh api repos/allhailpan-me/allhailpan-studio/check-runs/<job_id>/annotations
```

Releases are published by dispatching the build workflow with a tag, which
builds all three platforms and attaches them in the same run:

```
gh api -X POST repos/allhailpan-me/allhailpan-studio/actions/workflows/build.yml/dispatches \
  -f ref=main -f 'inputs[release_tag]=v0.3.0'
```

The notes come from `RELEASE_NOTES.md`, so they are written and reviewed
alongside the code they describe.

## How the audio flows

```
16 instrument channels ──┐
playlist audio clips ────┼──> inserts 1..16 ──> master insert 0 ──> device
input monitoring ────────┘         │  ▲
                                   └──┘ sends, forward only
```

Each insert holds eight effect slots. A channel's output normally folds to
stereo into one insert, or, when the channel is split, each output bus goes to
its own insert, which is how a multi-output drum machine gets a strip per drum.

## Rules that are easy to break

**The audio thread must not allocate, lock or log.** `audioDeviceIOCallbackWithContext`
runs there. It only ever *tries* the graph lock and skips the block if it
cannot get it. The message thread takes that lock properly, but holds it just
long enough to swap pointers. Anything that needs memory allocates on the
message thread and is handed over under the lock.

**Playback and export are two code paths over the same graph.** `renderOffline`
duplicates the mixing loop. Anything added to playback and not to the export
makes a bounce sound different from what was heard, which is among the worst
bugs a studio can have. Where the logic is non-trivial, factor it into one
function both call: `mixChannelOutput` and `routeSends` exist for exactly this
reason. Check the export path every time you touch the signal flow.

**Delay compensation has to be recomputed whenever a signal path changes.**
`updateLatency` must be called, under the graph lock, after loading or removing
a plugin, bypassing an effect, moving a channel to another insert, changing a
send, or splitting a channel's buses. Forget it and tracks drift apart by an
amount that depends on which plugins happen to be loaded, which is maddening to
diagnose from a bug report.

The arithmetic itself is in `LatencyGraph.h`, which carries no JUCE and is
tested. `updateLatency` is only the part that needs JUCE: reading each plugin's
reported latency, reading the routing out of the atomics, and pushing the
answers into the delay lines. Keep it that way. Decisions moved back into the
engine stop being testable, and this is the calculation here least able to
announce that it is wrong: nothing errors, nothing crackles, tracks just sit a
few milliseconds apart. Two real bugs lived in it, and both had a comment above
them claiming the opposite of what the code did.

It also reuses one `Setup` and one `Result`, held as members, because it runs
with the graph lock held and the audio thread only ever *tries* that lock. An
allocation in there is a chance of a dropped block every time somebody clicks a
bypass button.

**A take folder reads its takes directly, and its comp is in seconds.** A
folder clip has no `sample`: `Clip::compSpans` turns its comp into one read per
comped stretch, and `pushArrangement` turns each of those into one
`AudioClipRT` with the crossfade ramps converted to beats. That is why the
export gets the comp for free, which for a comp matters more than for anything
else: the comp is the performance. The comp itself stays in source seconds so
that changing the tempo moves the folder on the grid without moving the joins
through the performance. Warping, stretching, trimming and slicing are turned
off on a folder rather than half supported, in `Project::tidyFolder` and at the
three call sites that could reach them.

**A warped clip's markers are kept normalised, always.** `WarpMap.h`'s lookups
assume the marker list is sorted and strictly increasing in both beat and source
position, and that everything in it is after the clip's left edge, which is an
implicit marker. That invariant is what makes the mapping monotonic and
invertible. Every path that changes markers or moves `Clip::offset` has to go
back through `normaliseWarpMarkers` (`Clip::tidyWarp`), including loading a
project. A list that breaks it makes a clip read backwards, which sounds like a
stutter, and it makes the clip's own length in beats meaningless.

**Sends may only feed a higher numbered insert.** That one rule is what keeps
the mixer free of feedback loops and lets a single pass over the inserts
resolve every route. Do not relax it without replacing it with a real cycle
check and a topological order.

**New state needs four things, not one:** saved in `ProjectIO::save`, restored
in `load`, reset in `Project::clearAll`, and included in `Project::capture` and
`restore` if it should be undoable. Missing the reset is the one that survives
testing and then leaks between projects.

**Undo is one step per finished edit, not per mouse movement.** Components
expose an `isDragging` style method, and `MainComponent::changeListenerCallback`
uses it to hold off committing while a gesture is in progress.

## Research before building

Do not implement from memory or intuition anything that has an established
definition in the audio world. Look it up, find the actual specification or the
reference implementation, and work from that. Then say in the commit or the
pull request what the source was and how the result was checked against it.

This is not pedantry. A DAW is judged against other DAWs by people who know
exactly how the real thing behaves, and a plausible approximation is worse than
useless: it sounds almost right, which is the hardest kind of wrong to
diagnose. Several features here are correct only because they were built from
the specification rather than from a guess:

- Loudness follows ITU-R BS.1770-4 and is verified against the EBU Tech 3341
  compliance test signals, which define the exact reading a given tone must
  produce. The K-weighting filters are designed from the analogue prototypes
  rather than copied as 48 kHz coefficients, so they stay correct at any sample
  rate. Guessing here would have produced numbers that looked reasonable and
  disagreed with every other meter.
- Swing is defined as the position of the offbeat within its pair, where fifty
  percent is straight and sixty-six is a triplet shuffle, which is what
  hardware samplers have meant by the number since the MPC. Implementing it as
  "delay every other note a bit" would not match what a drummer or another
  studio means by a swing percentage.
- Band limited oscillators exist because naive saw and square waves alias, and
  aliasing is the single thing that makes a soft synth sound amateur.
- True peak is measured by oversampling four times through the interpolator
  BS.1770-4 Annex 2 tabulates, because the peak of the waveform a converter
  reconstructs is not the largest sample in the file. The meter here once used
  the largest sample, which read three decibels low on exactly the material
  where it matters and told the user a clipping master was safe. That is the
  shape of the failure this section is about: a number that is present,
  plausible, and wrong.

When adopting an idea from another studio, read its documentation and model the
real behaviour, including the parameters it exposes and why. Ableton's grooves,
for instance, are not a single swing amount: they carry a base resolution, a
timing amount, a velocity amount and a randomisation amount, and the useful
part is that a feel can be extracted from one performance and applied to
another.

Where a standard genuinely does not exist, say so plainly in the comment and
explain the choice, rather than implying an authority that is not there.

## Verifying things

JUCE cannot be compiled in the development sandbox, so CI is the compile check.
But most of the dangerous logic here is arithmetic, and arithmetic can be tested
without JUCE, in seconds, with sanitizers on.

A test that nothing runs is a comment that took longer to write, so these live
in `Tests/` and run on every push, as their own CI job that reports before the
platform builds have finished fetching their dependencies. A red test does not
stop the build, because knowing whether the code still compiles everywhere is
useful while a test is red, but it does stop a release from publishing.

```
./Tests/run.sh
```

That builds and runs every `Tests/*Test.cpp` with address and undefined
behaviour sanitizers, halting on the first undefined behaviour so the exit
code means something. Two rules make a new test fit:

- Name it `*Test.cpp`. It is compiled with `-I Source` and nothing else, so the
  header it covers must build without JUCE. Keeping a header that way where it
  can be is worth it on its own: `WarpMap.h` is pure arithmetic for exactly
  this reason. Where a header really does need a few JUCE types, stub them in
  the test file ahead of the include, as `LatencyDelayTest.cpp` does for
  `juce::AudioBuffer` and the `jmax` family.
- Exit non-zero when something is wrong, and print what. There is no framework.
- A `*Probe.cpp` is not run: that is a measurement that justified a design
  decision, kept so the decision can be rechecked, and it may need third party
  sources and minutes. `WarpStretcherProbe.cpp` is the example.

This has repeatedly caught real bugs before they shipped:

- A delay line read one sample past the end of its buffer, but only when the
  write position exactly equalled a tap length that had rounded up. It would
  have presented as a plugin that crashed rarely and unreproducibly.
- Tempo detection read an eight bar loop at 174 bpm as four bars at 87, because
  the candidates were being judged against a fixed anchor rather than the
  project's own tempo.
- Patch names generated in the wrong order, because C++ does not sequence the
  operands of `+` and two random draws were happening inside one expression.

Test the arithmetic that would be silently wrong, not the code that would fail
loudly. Randomised testing over thousands of generated configurations has been
more useful here than hand-picked cases, especially for the latency graph.

## How to talk about this project

This is its own tool, not a clone or a reimplementation of another one. Do not
describe it as having "an FL Studio workflow", as an FL Studio alternative, or
as anything-like-something-else, in the README, in release notes, in commit
messages or in the repository description. Other studios are inspiration and
sometimes a reference for how an established behaviour should work, which is
worth citing in a comment or a pull request where it explains a decision. That
is different from borrowing another product's name to say what this one is.

Describe it by what it does: patterns, a channel rack and step sequencer, a
piano roll, a playlist, a mixer with sends, plugin hosting, and an engine that
gets delay compensation and loudness right.

## The team

`.claude/agents/` holds job descriptions for specialists. They are checked in
rather than kept anywhere personal, because they encode this project's
standards and a session that does not have them produces work that has to be
sent back.

| Who | When to hand it to them |
|---|---|
| `dsp-researcher` | Before implementing anything with an established definition. Returns the specification and its source, and writes no code |
| `test-author` | After new arithmetic lands, or when a test is suspected of being weaker than it looks. Writes the test and mutation tests it |
| `auditor` | Before merging anything touching audio, timing, file format or the audio thread. Reviews and reports; does not fix |
| `build-doctor` | When CI goes red on a platform that cannot be compiled here, including telling a real failure from a runner that fell over |
| `release-manager` | When a user visible change has merged and the studio should ship |

Two rules about using them.

**Hand out work that is independent and well specified.** Research, test
writing, auditing and triage all fan out well, because each produces a result
that does not depend on what the others found. Anything where one measurement
decides the next question does not: the wavefolder took a chain of
measurements where the oversampling plateaued, so the antialiasing order
became the suspect, so an antiderivative needed checking, which found a
missing term. Split across four agents that would have produced four partial
answers.

**The auditor must not be the author.** The point of it is a reader who has
not spent the last hour convincing themselves the code is right. That has
caught shipped-breaking faults here more than once, including the one where a
driver reporting zero latency would have defeated the entire ASIO fix.

## Conventions

Comments explain **why**, not what. A comment restating the code is noise; a
comment explaining why a non-obvious choice was made is the reason the next
person does not undo it.

Never use em dashes, in code or in prose, including commit messages and pull
request descriptions. Use a colon, a comma, or a new sentence.

Commit messages say what was wrong and why the change is right, not just what
changed. No attribution lines, no generated-by trailers, no co-authors.

JUCE naming: camelCase members, `juce::` prefixed types, four space indent,
braces on their own line.

## Where things live

| File | What it holds |
|---|---|
| `AudioEngine.{h,cpp}` | The device callback, the mixer graph, latency compensation, offline rendering. The heart of the application |
| `Project.h` | Everything an `.ahp` file describes: clips, patterns, channels, modulators, the undo history |
| `ProjectIO.{h,cpp}` | Saving and loading, including plugin state and missing-file recovery |
| `MainComponent.{h,cpp}` | The window, the transport, and the wiring between every view |
| `PlaylistComponent.{h,cpp}` | The arrangement: clips, dragging, trimming, stretching |
| `PianoRollComponent.{h,cpp}` | Notes and automation lanes inside one MIDI clip |
| `ChannelRackComponent.h` | The step sequencer. Steps are ordinary notes in ordinary MIDI clips |
| `MixerComponent.h` | Strips, effect racks, sends |
| `ModulatorPanel.h` | Shapes wired to plugin parameters |
| `MixReportPanel.h` | What the finished master measures |
| `Preferences.h` | Application settings: bounds, what a stored value has to pass, and the arithmetic over it. No JUCE |
| `PreferencesWindow.h` | The preferences window, and the one path from a stored setting to its effect |
| `LoudnessMeter.h` | ITU-R BS.1770-4, verified against EBU Tech 3341 |
| `TruePeak.h` | True peak to BS.1770-4 Annex 2: the 4x oversampling the standard specifies. No JUCE |
| `PanLaw.h` | What the pan control means: a balance control or a panner, and the law a panner follows. No JUCE |
| `LatencyDelay.h` | The fixed delay used to line signal paths up |
| `RecordAlign.h` | Where a recorded take belongs: the round trip taken back off a capture, and the split into loop passes. No JUCE |
| `LatencyGraph.h` | Which path gets which delay, including the monitoring exemption. No JUCE, so the whole compensation graph is tested |
| `AudioDefaults.h` | The driver preference order and the buffer size arithmetic for first run. No JUCE |
| `InputSource.h` | Which input channel feeds which mixer insert, Auto-map, and the rule that a mono source is centred rather than pinned to one speaker. No JUCE |
| `RealFFT.h` | A radix-2 transform for real signals. No JUCE, so the spectral chain is testable end to end |
| `Spectrogram.h` | Short time analysis and weighted overlap-add resynthesis: the window, the hop, and the sum that has to come back flat. No JUCE |
| `SpectralEdit.h` | A region of the time against frequency plane and the gain applied to it, with tapered edges so a repair does not ring. No JUCE |
| `WarpMap.h` | Warp markers: the piecewise beat to source mapping. No JUCE, so it can be tested on its own |
| `CompModel.h` | Take folders: which take is heard where, and the equal-power crossfade at each join. In seconds, not beats, so a comp survives a tempo change. No JUCE |
| `PluginScanner.h` | Scanning in a child process, so a crashing plugin cannot take the studio down |
| `Fx/Oversampler.h` | Running a nonlinearity at a multiple of the sample rate and bringing it back, with the filter design that decides whether that is worth doing. No JUCE |
| `Fx/WaveFolder.h` | The west coast wavefolder: the characteristic, its two antiderivatives, and the antialiasing they are there for. No JUCE |
| `Fx/Saturator.h` | Drive: three curves, the antialiasing each one can carry, and the makeup that keeps the level where it was. No JUCE |
| `Fx/Svf.h` | The filter the effects are built on: two poles, every response a two pole section has, plus the bell and shelf forms an equaliser needs. No JUCE |
| `Synth/WaveTable.h` | Band limited wavetables and the oscillator that reads them: the mipmap, the morph between frames, and the interpolation. No JUCE |
| `Synth/ModMatrix.h` | What drives what in the instrument: the named sources and destinations, the routing table, and the arithmetic that turns one into the other. No JUCE |
| `Synth/LowPassGate.h` | The west coast gate: one control taking brightness and level together, through a vactrol that is slow and lopsided. No JUCE |
| `Synth/Slope.h` | A rise and a fall that will go round forever: an envelope, an LFO and an oscillator depending only on how fast it is set. No JUCE |
| `Synth/Voice.h` | One voice with everything wired through the matrix: the oscillators, the folder, the drive, the filter, the gate, the envelopes and the slopes meeting, and the oversampled section they run in. No JUCE |
| `PanOne.{h,cpp}` | The built in instrument: the parameters built from the table, the state saved by id, the shared wavetables and the MIDI plumbing |
| `PanOneParams.h` | Every control PAN One has and every preset, as data: the ranges, the choice lists, the groups, and the translation of what version one saved. No JUCE |
| `PanOneEditor.h` | The front panel, laid out from the table |
| `Synth/WaveBank.h` | The wavetables the instrument ships with, as definitions rather than recordings, plus the shape to spectrum method they are built by. No JUCE |
| `Tests/` | Standalone checks on the arithmetic, run by `./Tests/run.sh` and by CI on every push |
| `Assets/make_app_icon.py` | Builds both app icon masters from the logo, and carries the reasoning for how they are built |

## Things worth knowing about the design

The channel rack does not store steps. Each step is a real note in a real MIDI
clip, so a pattern drawn in the rack opens in the piano roll and edits there.
Resist adding a parallel representation.

Modulators drive ordinary plugin parameters rather than a special internal
graph, which is why they work on any hosted plugin. Modulation is added on top
of whatever the parameter already sits at, and the engine tells its own writes
apart from a user's by comparing against what it wrote last block.

Audio clips are read by mapping a beat to a source time, in
`AudioEngine::renderAudioClips`. Playlist audio is delayed by reading further
back in the arrangement rather than through a delay line, which costs nothing.

Input monitoring deliberately skips delay compensation, and so do the sends
out of the insert being monitored through. That compensation aligns internal
paths with each other; on a monitor path it would only be latency the player
feels. Aligning the take itself with the arrangement is a separate job, done
once per take in `startAudioRecording` from the figure `RecordAlign.h` works
out.

It used to be done in `MainComponent::finishAudioRecording` instead, by
reading past the head of each pass by the device's reported round trip. Three
things were wrong with that: it left out the studio's own compensation, so a
lookahead plugin on the master put every take late again; it was clamped to
half a pass, so a short one was half corrected; and it left the loop split
points where the transport wrapped, so the last milliseconds of every pass
but the final one were handed to the next pass, skipped at its head, and
lost. All three survived for releases, and the fourth thing wrong with it is
why: the figure was in the window, where nothing tests it.

Both halves matter. A send is a copy of the insert taken after its fader, so on
a monitored insert it carries the player's own input, and compensating the copy
puts back exactly what was taken off the original: the player then hears
themselves once immediately and again at send level, a slap back that moves
whenever a plugin is loaded elsewhere. Putting a reverb on a vocal while
tracking it is the ordinary way to use a send, so this is not an exotic case.
The price is that anything else parked on that insert travels early too, down
the dry path and the sends alike, so a mix should not be judged with monitoring
left on.

The monitored insert is never insert 0. The master is what every other insert
is lined up to, so there is nothing to exempt it from, and a monitor routed
there would run through the whole master chain and pick up its lookahead, which
is the exact thing the exemption exists to avoid.

**Panning a mono source and balancing a stereo one are two different
controls.** An insert's pan knob is one or the other, chosen per insert in its
context menu, and balance is the default because it is what every version up
to now did to every insert, so no saved project moves. Balance leaves a centred
signal alone and attenuates the opposite side in proportion, which is right for
a stereo recording and costs a mono source 3 dB the moment it is moved off
centre, because a mono source is the same signal in both halves of the buffer.
The panner is what fixes that, and the law it follows is one setting for the
project, the way Logic and Reaper keep it. The arithmetic and the sources are
in `PanLaw.h`. Insert 0 is always a balance control: the master is what every
other insert is summed into, so there is no source on it to place, and a law
there would move the level of the whole mix. The law is latched once per block
in the device callback and once per render in `renderOffline`, so one block, and
one whole bounce, is mixed under one law.

**On Windows, the driver decides the monitoring latency and the buffer size
control cannot rescue it.** JUCE registers three WASAPI modes, and in the plain
shared one, `WASAPIDeviceMode::shared`, it offers a single buffer size and
ignores any request to change it, because the wakeup period belongs to the
driver. So a buffer size setting on "Windows Audio" genuinely does nothing,
which is what a bug report said and which reading
`juce_WASAPI_windows.cpp` confirmed. "Windows Audio (Low Latency Mode)" is the
`IAudioClient3` path and is the one to want: it enumerates from the driver's
minimum period upward and honours what is asked for, while still sharing the
device. Microsoft document the inbox HDAudio driver as supporting 128 to 480
samples, and a driver has to opt in to the small end. The makers of audio
interfaces mostly did not, because they ship an ASIO driver and expect it to be
used, so on that hardware Low Latency Mode reports the same ten milliseconds as
the default and there is nothing to be had from it.

**ASIO is the one that matters on Windows, and the builds have it.** Until 15
October 2025 they could not: the SDK's licence was proprietary only and
incompatible with this project's AGPL-3.0. On that date Steinberg dual licensed
it under GPL-3.0, which AGPL-3.0 may combine with through section 13 of each,
so CI now downloads it, checks it against a known SHA-256 and builds with
`JUCE_ASIO=1`. A Windows build that ends up without it **fails**: the configure
step matches on CMake's own "ASIO support enabled from" line, because a
silently ASIO-less Windows build is a studio that cannot reach an interface's
driver at all, and that is invisible from outside. The studio also says in
Preferences > Audio whether the running build has ASIO, so "not on this
machine" and "not compiled in" can be told apart. Three days went into the
wrong problem for want of exactly that distinction.

"Windows Audio (Exclusive Mode)" holds the endpoint, so nothing else on the
machine plays through that device while the studio is open. It is tried **last**
rather than refused. Refusing outright sounds careful and is wrong on the
hardware that needs it: somebody with an interface is not listening to the
system through it, so taking it costs them nothing, and refusing leaves them
unable to play. The order lives in `AudioDefaults::searchOrder`, and the test
asserts the property rather than the list: nothing that takes the device may be
tried before something that shares it.

**Each mixer insert can take one input from the interface, and any number of
them record at once.** That is the capability a studio needs to record a band
rather than one person, and it is modelled on how Image-Line document FL
Studio's mixer, because the behaviour is established and nobody should have to
learn a second set of rules for the same job: one input per track, a menu with
the stereo pairs above the mono inputs, selecting an input arms the track, and
a right click offering Auto-map, which gives every input on the device a strip
of its own working upward. The routing model is in `InputSource.h` and is
tested; the mixer strip owns the menu and the arm button.

A few consequences worth keeping straight:

- Insert 0 never takes an input. It is where every insert is summed after each
  has been lined up, so a live input there would run through the whole master
  chain and pick up its lookahead, which is the thing the monitoring exemption
  exists to prevent.
- Every insert carrying a live input is exempt from delay compensation, not
  just one. `LatencyGraph::Insert::monitored` is per insert for that reason.
- **A bounce is fully compensated whatever monitoring is doing.** The
  exemption is the right trade while tracking and the wrong one in a rendered
  file, where it would leave most of the mixer uncompensated with nothing to
  say why. `renderOffline` sets `exportingFullyCompensated` and recomputes the
  graph around the render.
- Each armed insert gets its own recorder and its own playlist track. The
  track is found empty rather than counted off: stepping blindly drops one
  player onto another player's clips, and running off the end and clamping
  appends one performance into another's take folder, which is a comp.
- A recorder that cannot read its input for a block is **padded with silence**,
  not skipped. Skipping shortens the take and shifts everything after it
  earlier against the arrangement with nothing to say so.
- **A take is written down earlier than it arrived, by the round trip plus the
  mixer's own compensation.** The arithmetic is `RecordAlign.h` and the figure
  is latched once when recording starts, the way Ardour refuses an alignment
  change while a `DiskWriter` is running: a figure that moved partway through
  a capture would put a join in the middle of a performance the moment
  somebody loaded a plugin on another track. The instrument recorder is the
  exception and gets no offset, because it taps a channel's output inside the
  mixer rather than a socket, and there is no round trip in front of that.

**One instrument in one socket is a mono input, and mono means centred.** The
engine took input channel 1 as the left of a stereo pair and input channel 2 as
the right, always, in both monitoring and recording. That is right for a stereo
keyboard and wrong for everything else: a guitar in input 1 of an interface,
with nothing in input 2, was heard out of one speaker and recorded as a stereo
take with silence down one side. Which input is listened to is now chosen, in
`InputSource.h`, and a mono choice resolves both sides to the same channel.

Worth remembering for the shape of it as much as the fault. It presents as an
**output** problem, because one speaker is an output symptom, so that is where
the person goes looking and nothing they find there can help. The test asserts
the property directly: a mono source is centred, for every channel of every
device size.

**A reported latency of zero means "the driver did not say", never "instant".**
JUCE's ASIO backend zeroes both figures when `getLatencies` fails, and zero is
the best possible score, so a search that believes it stops on the one driver
that would not answer and then falls back to the slow one. `playable` therefore
requires a round trip above zero, and `roundTripOrEstimate` falls back to two
buffers, which is the floor any device can have. An audit caught this before it
shipped; the first version of the ASIO support would have been defeated by it.

**The app icon is plated, and there are two masters.** The mark is white line
art on transparency, which is invisible on a light background, and a Win32
`.ico` carries one image with no light and dark variants. So the icon was
there in the file and absent on screen for anybody on the Windows light
theme, which is the default. It now sits on its own black background.

The second master exists because JUCE writes 16, 32, 48 and 256 into the
`.ico` and, with only `ICON_BIG` set, builds all four by downscaling the one
file. A 7 pixel stroke is a tenth of a pixel at 16, so the three small entries
were grey mush. `getBestIconForSize` picks the smaller of the two drawables
when both are large enough, so a 48 pixel `ICON_SMALL` covers 16, 32 and 48
while the 1024 covers 256. The small one is the mark's silhouette rather than
its outline, which keeps what identifies it and drops lines that are gone at
that size anyway.

Regenerate both with `python3 Assets/make_app_icon.py` rather than exporting
by hand, and check the result by looking at it at 16, 32, 48 and 256 on a
light background as well as a dark one. An icon has no other test, and the
light background is the one that found this.

**Describing a waveform and reading one need different numbers of samples.**
`Synth/WaveTable.h` stores each mipmap level sixteen times longer than the
sampling theorem asks and reads it with a cubic, and both halves of that were
measured rather than chosen.

The first version used the obvious reading of the theorem: a waveform of H
harmonics is fully described by 2H samples, so each level was 2H samples long
and was read with linear interpolation. Every check about harmonic counts
passed, because the harmonic counts were right. The oscillator measured 28 dB
below its own fundamental, which is as bad as the naive sawtooth the design
exists to avoid.

A read lands between stored samples, and what the interpolator does in
between is an error that is not a harmonic of anything. Enough samples to
reconstruct the waveform with an exact reconstruction filter is not enough to
reconstruct it with four points and a cubic. `Tests/WaveTableProbe.cpp` has
the table of error against oversampling and interpolation order that settled
the numbers, kept so the choice can be rechecked.

Two things about testing this, both learned by watching deliberate breakages
walk through:

Not aliasing and being accurate are different claims. Interpolation error is
a periodic distortion, so most of it lands on harmonics of the note, where a
check for stray partials cannot see it. Halving the table length and replacing
the cubic with a linear read both stayed under the aliasing threshold while
putting the real error up by more than twenty decibels. There is now a check
that compares against additive synthesis, which sees everything wherever it
lands.

A test waveform has to exercise the join. The stored cycle carries a guard
sample either side so a four point read never has to wrap, and those guards
only matter where the waveform is doing something across the join. The first
test built its sawtooth from cosines with alternating signs, which has the
right spectrum and a perfectly smooth join, and three separate breakages of
the guards went unnoticed. The saw is built from sines now, which puts the
edge at the join.

And when comparing a normalised table against an unnormalised reference,
divide out the best fitting scale first. The table normalises by its loudest
stored sample and a continuous peak sits between samples, so the two differ
by about a tenth of a percent, which reads as -60 dB and hides everything the
check is for.

**A modulation routing is saved in the project, so it is named and not
numbered.** `Synth/ModMatrix.h` gives every source and destination a stable
string id, the same discipline `PanOneParams` uses for knobs, and the id is
what an `.ahp` stores.

The reason is the failure it prevents. If a destination were an index into a
list, inserting one new entry in the middle would leave every patch anybody
had ever saved modulating the wrong thing: the filter envelope on the pan,
the vibrato on the amplitude. The patch would still load and still play. That
is the shape of fault this project keeps meeting, and an index is the easiest
way to invite it.

For the same reason an id nobody knows resolves to minus one and the routing
is dropped. Losing a routing is a patch that is less than it was; redirecting
it is a patch that is wrong, and the second is much worse because nothing
about it looks broken.

Two more things in there that are decisions rather than details. Every
destination declares what a depth of one is worth in its own units, so the
same number in any row means a comparable amount of movement, and a slot at
full depth is a strong musical move rather than an unusable one. And a
routing can be scaled by a third source, which is what lets a mod wheel
control the depth of a vibrato rather than the vibrato itself.

When mutation testing anything in `Source`, compile the broken copy **with
the sanitizers**, the way `Tests/run.sh` does. Four deliberate breakages of
the bounds checks in here looked like they survived, and every one of them
was an out of bounds read that the suite catches and a plain `-O2` build does
not. A mutation harness that is weaker than the real test run will report
gaps that are not there and hide the ones that are.

**A band of energy in a spectrum has to be a ratio, not a number of
harmonics.** The harmonics table's own comment said its first frame was a
sine. It was not: the band was three harmonics wide either side, and sitting
on the fundamental, three harmonics either side reaches upward only, so the
frame advertised as a sine had harmonics two and three on it at two thirds
and a third. The bank contained no sine at all, which is the one waveform a
wavefolder wants in front of it, because folding a sine gives almost entirely
odd harmonics and folding something with an octave already in it gives a
mess. A comment claiming the opposite of what the code does, again.

The band is measured in octaves now, and the centre climbs geometrically so
every frame is the same musical distance from the last. Linearly from
harmonic one to twenty four, the first step is more than an octave and the
last is a twentieth of one.

Fixing it showed why it had been written the wrong way. A constant ratio band
holds one partial at the bottom of the sweep and thirty at the top, and
thirty partials make a far larger peak than one, so sweeping the table became
a volume ride of 25 dB and the existing check caught it. A table is
normalised as a whole by its loudest stored sample, which cannot fix a
difference between its own frames: it only makes the quiet end quieter. So
each frame of that table is normalised to the same peak, which costs a 4096
point synthesis per frame at startup and nothing afterwards.

**A wavetable is built from a definition, never sampled from somewhere.**
`Synth/WaveBank.h` writes every table as either a known harmonic series or a
waveform as a function of phase, and takes the spectrum of the second kind by
transform. A sampled wavetable carries whatever harmonic content the
recording happened to have, including the aliasing of whatever made it, and
cannot then be band limited honestly.

Where a shape was invented here rather than taken from somewhere established,
the comment says so. A vowel table and a bell table are deliberately missing
rather than present with plausible numbers in them, because formant
frequencies and the partial ratios of a tuned bell are measured facts about
the world and guessing them is exactly the failure the research section is
about.

Two faults it shipped with before the test was written, both of the shape
this file keeps warning about:

A pulse had its amplitude divided by the width, which is the right shape
across the harmonics for a fixed width and so looks correct, and carries a
factor that depends on the width. A table sweeping from a square to a narrow
pulse came out with the narrow end six times louder, and since a table is
normalised as a whole that made the wide end six times quieter: sweeping the
width was a fade. The same function also turned every harmonic a quarter
back, which is what makes a sawtooth, where a pulse's turn grows with both
the harmonic and the width. At half width that is right for harmonic 1 and
wrong for 3 and 7, so the square had a square's amplitudes with some of its
harmonics inverted. It looked like a plausible waveform, which is why it
needed checking against the series rather than looking at.

The lesson for testing one of these: check a generated spectrum against a
closed form, not against another implementation of itself. The strongest
check in `WaveBankTest` is the phase modulation table against the
Jacobi-Anger expansion, because its amplitudes are not monotonic and nothing
about it is a simple series, so almost any error in the transform shows up
somewhere in it. Collect that expansion by harmonic carefully: the terms
below minus one fold back onto positive harmonics with their sign reversed,
so harmonic k carries `J(k-1) + (-1)^k J(k+1)` and not `J(k-1)` alone.

**Nothing in the instrument is built lazily.** Every wavetable is built when
the studio starts, not the first time a patch asks for one. That costs about
forty megabytes held from launch and a few hundred transforms at startup, and
it buys never pausing in the middle of somebody's work. Memory is cheap and
a hitch while playing is not.

**A wavefolder is the one nonlinearity that really needs both kinds of
antialiasing.** `Fx/WaveFolder.h` runs at four times the sample rate and
evaluates its characteristic by second order antiderivative antialiasing, and
both halves were arrived at by measurement rather than by choosing a round
number.

Folding generates far more harmonic content than an ordinary distortion, so
each remedy on its own falls short and for a different reason. Oversampling
alone stops improving past four times, because what is left is the order of
the antialiasing. First order antialiasing alone, or with any amount of
oversampling, leaves stray partials around 40 dB down on the harder settings.
Together they measure 95 to 105 dB down across the whole range the folder
claims.

Three things in there are worth not relearning.

**A triangle fold cannot be shipped.** The sharp cornered idealisation was
built, measured and taken out. Its clean range collapses to 371 Hz at two
times drive and to nothing above four, because a corner is a break in the
slope rather than in the value and throws harmonics that fall away only as
one over the square of their number. Eight and sixteen times oversampling
give the same answer. It is no loss: the circuits in the literature fold
through transistor curves, which are rounded, so the sharp triangle models
nothing that exists.

**The second antiderivative of a periodic nonlinearity is not periodic.** The
first one is, because the fold averages to nothing over a period, so it can
be written as a function of the wrapped input. The second integrates the
input times the fold, whose integrand still carries the unwrapped input, so a
term proportional to the number of whole periods goes missing if it is built
the same way. It is wrong by exactly four times the period count times the
fold, the folder still runs and still folds, and the aliasing is twenty
decibels worse. Check an antiderivative by differentiating it numerically and
comparing against the function, which finds this in one line.

**An interpolating filter has to be normalised per phase.** Scaling all the
taps so they add to one is right for decimation and wrong for interpolation:
the interpolator sees a zero between every input sample, so only every other
tap has anything under it and the two sets take turns, and each set has to
add to one on its own. Normalised together, a steady input comes out with a
ripple on it, which measured as five parts in a million and meant a folder
fed a steady voltage did not give back the fold of that voltage.

And two things about measuring any of this. A filter's own stopband is the
floor under every measurement behind it: an early probe used a Blackman
window, whose sidelobes are 58 dB down, and every reading sat at -55 dB
however much oversampling was thrown at it. And measure levels as a mean
square rather than a peak, because these filters delay by half a sample at
some factors, and at 18 kHz there are under three samples to a cycle, so a
half sample shift moves every sampled point off the top of the wave and the
peak reads 0.69 dB low with nothing wrong at all.

**Say which band an aliasing figure is about, or it will mislead you.**
`Fx/Saturator.h` was nearly shipped with half the oversampling it has,
because the first measurement counted every stray partial up to Nyquist. Read
that way the figures stop improving after two times oversampling and sit
around -50 to -67 dB, which looks like a wall that nothing can climb and an
obvious place to stop paying. Read up to 20 kHz instead, every curve at every
drive goes under the floor of the measurement at four times.

Both numbers are true. The oversampler's filters are specified to stop at the
oversampled rate minus 20 kHz rather than at Nyquist, exactly so that what
folds lands above 20 kHz where nothing can hear it, so the wider reading was
measuring that deliberate fold and nothing else. The wider band is the right
one to look at for a filter and the wrong one for a nonlinearity sitting
behind that filter.

**The drive on a filter, the distortions and any saturation on a bus are one
object.** `Fx/Saturator.h` is it, with three curves, and the curves differ in
more than their sound.

The hyperbolic tangent is the curve a transistor differential pair has, which
is why the published ladder filter models apply it at every stage
(Huovilainen, DAFx-04). **It can only carry first order antiderivative
antialiasing**, and that is a fact about the function rather than a corner
cut: its first antiderivative is `log(cosh x)`, exact and cheap, and its
second contains a dilogarithm and has no elementary form at all. The cubic
soft clipper and the plain clip have exact piecewise antiderivatives to any
order, so they get the second order form. At four times oversampling all
three disappear under the measurement floor anyway, which is the answer to
the obvious worry.

Three things in there worth not relearning.

**Write `log(cosh x)` as `|x| + log1p(exp(-2|x|)) - log 2`.** The naive form
overflows `cosh` past about 710 and has lost most of its significant figures
a long way before that, and a full scale signal at a drive of sixty-four
reaches those inputs in ordinary use.

**A bias belongs to the curve, not to the input.** The shaper is
`g(u) = f(u + bias) - f(bias)`, so it passes through the origin and leaves no
constant behind: a DC offset into a filter wastes its headroom and into a
gate it is a thump at every note. Written the other way, with the offset
added to the input, the antialiasing sees the whole bias change as a step in
the signal, so a bias sweep is a sequence of clicks and the first sample
after any reset is wrong by however far the bias sits from zero. Measured at
a bias of 0.8 that first sample came out at 0.30 with silence going in. For
the same reason, anything that changes the shaper, a curve or a bias, has to
re-read the antiderivatives the history is holding rather than clear them:
left stale, the next difference quotient subtracts an integral of the old
shaper from an integral of the new one and divides the mismatch by however
far the signal happened to move. Deliberately breaking that measured 545
times full scale on a slow signal.

**Test an antialiased nonlinearity against the integral it claims to
compute.** The first order form is the shaper averaged evenly across the step
and the second is it averaged under a triangle two samples wide, and with the
input linear between samples both are ordinary integrals that Simpson's rule
does to more places than the closed forms need. That one check killed six
deliberate breakages on its own, including both of the fallbacks that only
fire on nearly steady signals, where no other check is looking.

**A low pass gate is not a filter and an amplifier sharing an envelope.**
`Synth/LowPassGate.h` is one control taking brightness and level together,
through a vactrol, and both halves of that matter.

The pairing is why it sounds like something struck rather than switched on:
real objects lose their high partials faster than their fundamental, so quiet
and dark arrive together. A voice with a separate filter and amplifier can be
set up to imitate it and almost nobody does, because it means drawing the same
envelope twice and matching them by ear.

The vactrol is the other half. A lamp lights almost at once and the light
dependent resistor beside it takes a long time to relax, so the gate strikes
fast and releases slowly whatever the control does, and no envelope setting
reproduces that. Modelled from Parker and D'Angelo, "A Digital Model of the
Buchla Lowpass-Gate", DAFx-13: the time constant switches on the sign of the
control's slope and is further modulated by the current output, and the
measured constants from the VTL5C3/2 datasheet are about 12 ms rising against
250 ms falling. Twenty times slower to let go than to light up is the figure
that gives the sound its shape.

What is deliberately not taken from that paper is its component values. It
models one particular circuit down to its zener breakdown voltage, and
copying those would be claiming to be that module.

Test it by what identifies it rather than by its arithmetic: strike it, feed
it something rich, and measure the spectral centroid of the output falling as
the level falls. It measures a factor of 7.8 of darkening over a 43 dB decay.
Stop asking once the centroid reaches the fundamental, because the centre of
gravity of a spectrum cannot go below its lowest partial and what the
measurement reads after that is its own noise floor.

**One module is honestly three.** `Synth/Slope.h` is a rise and a fall that
will either happen once or go round forever: an envelope fired once, an LFO
left cycling, and an oscillator cycled fast, from the same arithmetic rather
than three code paths sharing a name. That is not a convenience. The slow
sweep in a west coast patch is an envelope that never stops, and the vibrato
is the same module turned up, and the growl is the same module turned up
again. `Envelope.h` cannot do it because an ADSR holds while a note is held,
and an LFO with a fixed shape cannot have a rise of one length and a fall of
another.

Its shape control bends the path and leaves both ends fixed, so changing it
while something runs changes the sound and not the level, and its centre is
exactly straight rather than nearly straight.

**In the voice, nothing is soldered except the one thing that has to be.**
`Synth/Voice.h` has four envelopes and two slopes and none of them is wired to
anything: what they reach is whatever the patch says. A patch can put envelope
one on the gate, envelope two on the fold, a slope on the wavetable position
and nothing at all on the amplifier, which is an ordinary west coast patch and
was not expressible with the old voice.

The exception is the amplitude, and it is written down rather than hidden.
Something has to tell the allocator a note has finished, and the only thing
that knows is the envelope that was released when the key came up, so envelope
one always scales the output. Without that a patch could route nothing to the
amplitude, the voice would never report itself finished, and sixteen notes
would exhaust the instrument with nothing audible to show for it.

The signal path is fixed even though the modulation is not: the folder is
before the gate. A folder after a gate is being fed something already made
quiet, so it stops folding as the note decays and the patch loses its
character exactly where it was interesting.

**A declared destination that nothing reaches is the failure this project
keeps meeting, so the voice's test asserts there are none.** The matrix
declares thirty two destinations and the voice reaches twenty one. The other
eleven are modules that are not built yet, and they are listed by name in
`VoiceTest`, with the reason, and the check works both ways: adding a
destination without reaching it fails until somebody writes down that it is
not built, and building one without taking it off the list fails too. So the
list of known gaps cannot quietly become a lie, which is the usual fate of
one.

The method is to play the same patch twice with one extra routing pointed at
the destination under test and see whether the output moved at all. The base
patch has to have something going on in every part of the voice or the test
lies: a slope's rate destination is only visible through whatever the slope
is driving, so the base patch wires both slopes to something before asking.

**Changing a destination's sound is not the same as changing it the right
way.** The completeness check above cannot tell a rate control that speeds a
slope up from one that slows it down, because both change the sound. The sign
was inverted deliberately and survived every other check in the file. A
destination declared in octaves now has its direction and its amount checked
against the figure it declares: an octave of modulation takes a slope from
five swells a second to ten.

And measure a swell rate off the envelope's spectrum rather than by counting
crossings of a windowed level. The window has to be short to see five swells
a second, and at that length it also sees the note going up and down inside
it: a window of 128 samples on a 110 Hz note read fifty swells a second where
there were five.

**Skipping work the patch is not using is most of the instrument's
performance, and the measurement is the only reason to believe any of it.**
The voice started at 161 per cent of a processor core for sixteen voices,
which does not run at all, and the simplest possible patch cost the same as
the busiest one. Three things fixed it, and none of them was the one that
looked obvious:

- **The folder and the saturator were in circuit even when mixed out.** The
  fold rests at a mix of nothing and the drive rests at zero, and running
  them anyway was two thirds of the cost of everything. Skipping is exact at
  a mix of nothing, and at the smallest drive the saturator's makeup gain
  cancels its curve to a part in ten thousand.
- **Setters called once a sample recomputed things that had not moved.**
  `Saturator::setDrive` costs a hyperbolic tangent and a pair of
  exponentials, and `setBias` also re-reads the history, which is four more
  evaluations. They are modulation destinations so the voice calls them every
  sample, and on almost every patch they are constant. Returning early when
  the value is unchanged took a fifth of a core off.
- **The voice retuned and recomputed its filter every sample.** Three powers
  of two for the pitches, three logarithms to pick the mipmap level, and a
  tangent inside the filter, all arriving at the same answers for the whole
  length of a note. Guarded on the modulated value having actually moved,
  that is another fifth of a core.

Sixty per cent for sixteen voices of a plain patch and a hundred for the
busiest. The oversampling is now the largest single item at a third of a
core, and the obvious next step is a polyphase form: the interpolating filter
multiplies by a zero every other tap, and the decimating one computes an
output that is thrown away, so each is exactly twice the work it needs to be.

**A guard like that is only safe if it is forgotten at the right moments.**
Both caches are seeded as quiet NaNs, which compare unequal to everything
including themselves, and are reset whenever the note or the patch changes.
Deliberately removing that reset survived every other check in the voice's
test, because they all play a note on a fresh voice. The real path does not:
the instrument reads its knobs once a block and pushes a patch into every
voice, so turning a filter knob during a held chord goes straight through it,
and the fault would have been that notes started after the change are right
and notes already sounding keep the old filter and the old tuning.

**Two deliberate breakages of the voice survive on purpose**, which is worth
writing down so nobody goes looking for the test that should have caught
them. Running the folder while it is mixed out produces identical output,
because the folded signal is multiplied by nothing: that one is a performance
regression and nothing else. And the two throwaway evaluations that fill a
skipped nonlinearity's history when it comes back cannot be measured on a
mix that ramps up, because at the moment it comes back the mix is nearly
zero and whatever the stale history produces is multiplied by nearly zero.
They are kept as insurance for the discontinuous case, a preset change that
takes the mix from nothing to most of the way in one sample, and they cost
two evaluations once.

**Every choice in the instrument is saved by its id, and that includes the
modulation matrix's own rows.** `PanOneParams.h` is the whole control surface
of PAN One as a table: 240 controls, of which 192 are the matrix at six
controls a row, and 22 presets.

The matrix is in the table rather than beside it, and that is a decision
rather than a convenience. A routing's source and destination are useless as
automation lanes, so keeping them out looks tidier. It would also mean a
second mechanism for saving them, restoring them, writing them when a preset
is chosen and handing them to the audio thread, and the whole argument for
the table is that one mechanism cannot be half implemented. A preset that
moved every knob and left the previous patch's wiring in place is exactly the
fault the table exists to prevent.

Every choice list is ordered so that its **first** entry is the one that is
safe to fall back to, because an id nobody recognises lands there: "none" for
a source or a destination, which drops the routing the way the matrix says an
unresolvable routing should be dropped, and the plainest setting for
everything else.

**A project from version one is translated, not just partly loaded.** Four of
its controls have no equivalent: two oscillator shapes, a pulse width, and a
filter envelope depth in octaves. Dropped, a project opens with the right
envelope times and entirely the wrong sound. They become a wavetable and a
position in it, a position in the pulse table, and a routing from envelope
two to the cutoff at the depth that means the same thing, and the check is to
play each of version one's four shapes and measure what comes out: the sine's
third harmonic at 0.006, the square's second at 0.012, the triangle's third
at the ninth it should be.

**Play every preset in the test.** It is not a check on the table at all and
it is the one that found the most. Three faults, each of which loads without
complaint and sounds wrong:

- **Pointing anything at the gate replaces the amplitude envelope**, by
  design, because a patch that has wired the gate deliberately does not want
  an envelope fighting it. So a preset that routed only the velocity there
  left the gate at one fixed opening for the whole note, and its amplitude is
  the square of the opening: 46 dB down.
- **An east coast patch has to turn the vactrol off.** The gate as a plain
  amplifier still has the lamp and the light dependent resistor in it, which
  take twelve milliseconds to light, and a hat with a half millisecond attack
  and a seventy millisecond decay cannot get through them. 26 dB down.
- **Four presets would have clipped on a chord** while sounding fine on one
  note. The instrument divides by four for its sixteen voices, so a voice is
  held to a peak of 1.5.

Measure the loudest tenth of a second anywhere in the render, not the level
over a fixed stretch of it. No fixed stretch suits both a drone with a two
and a half second attack and a struck sound that has finished in seventy
milliseconds: measured over the second half of three seconds, five presets
read as silent and none of them was.

**A destination's modulation has to be clamped to that destination's own
range.** This bit on the first run of the new voice and it is worth knowing
the shape of it. The helper that applies a modulation sum defaults to clamping
between nothing and one, which is right for a level and wrong for everything
that is not one. Pointed at the fold, whose useful travel is to sixty four,
and at the pitch, which is in semitones either side of nothing, the routing
stayed wired, the matrix was evaluated, the sum was computed, and the result
was flattened back to the number it started at.

Nothing failed. The test that caught it measured the spectrum with and without
the routing and got the same centroid to four decimal places, which is what
sent somebody to look at the matrix rather than at the one line that was
wrong. Every call in the voice now passes the range its parameter actually
has.
