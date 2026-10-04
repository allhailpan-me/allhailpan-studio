# Notes for working on this codebase

ALLHAILPAN Studio is a digital audio workstation: a JUCE application in C++20,
about twelve thousand lines, built with CMake and FetchContent. It hosts VST3
plugins, records audio and MIDI, and exports finished mixes.

This file is what a newcomer would otherwise have to learn by breaking things.

## Build

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

JUCE is fetched automatically and pinned to a release tag, not master, so the
build is reproducible. Rubber Band is fetched the same way and compiled from
its single-file build.

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
without JUCE: copy the header into a scratch directory with a small stub for the
few JUCE types it uses, and compile it with sanitizers.

```
g++ -std=c++20 -O1 -fsanitize=address,undefined -I. -o test test.cpp && ./test
```

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
| `LoudnessMeter.h` | ITU-R BS.1770-4, verified against EBU Tech 3341 |
| `LatencyDelay.h` | The fixed delay used to line signal paths up |
| `WarpMap.h` | Warp markers: the piecewise beat to source mapping. No JUCE, so it can be tested on its own |
| `PluginScanner.h` | Scanning in a child process, so a crashing plugin cannot take the studio down |

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

Input monitoring deliberately skips delay compensation. That compensation
aligns internal paths with each other; on a monitor path it would only be
latency the player feels. Aligning the take with the arrangement is handled
separately, by the recorder.
