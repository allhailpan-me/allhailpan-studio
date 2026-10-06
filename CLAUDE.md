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
| `LatencyDelay.h` | The fixed delay used to line signal paths up |
| `LatencyGraph.h` | Which path gets which delay, including the monitoring exemption. No JUCE, so the whole compensation graph is tested |
| `AudioDefaults.h` | The driver preference order and the buffer size arithmetic for first run. No JUCE |
| `RealFFT.h` | A radix-2 transform for real signals. No JUCE, so the spectral chain is testable end to end |
| `Spectrogram.h` | Short time analysis and weighted overlap-add resynthesis: the window, the hop, and the sum that has to come back flat. No JUCE |
| `SpectralEdit.h` | A region of the time against frequency plane and the gain applied to it, with tapered edges so a repair does not ring. No JUCE |
| `WarpMap.h` | Warp markers: the piecewise beat to source mapping. No JUCE, so it can be tested on its own |
| `CompModel.h` | Take folders: which take is heard where, and the equal-power crossfade at each join. In seconds, not beats, so a comp survives a tempo change. No JUCE |
| `PluginScanner.h` | Scanning in a child process, so a crashing plugin cannot take the studio down |
| `Tests/` | Standalone checks on the arithmetic, run by `./Tests/run.sh` and by CI on every push |

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
feels. Aligning the take with the arrangement is handled separately, by the
recorder.

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

**A reported latency of zero means "the driver did not say", never "instant".**
JUCE's ASIO backend zeroes both figures when `getLatencies` fails, and zero is
the best possible score, so a search that believes it stops on the one driver
that would not answer and then falls back to the slow one. `playable` therefore
requires a round trip above zero, and `roundTripOrEstimate` falls back to two
buffers, which is the floor any device can have. An audit caught this before it
shipped; the first version of the ASIO support would have been defeated by it.
